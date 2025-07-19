#include <condition_variable>
#include <iostream>
#include <coroutine>
#include <cstring>
#include <mutex>
#include <queue>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <thread>
#include <vector>
#include <asm-generic/int-ll64.h>
#include <linux/io_uring.h>
#include <sys/mman.h>

int io_uring_setup(const unsigned entries, io_uring_params *params) {
    const int ring_fd = syscall(__NR_io_uring_setup, entries, params);
    return (ring_fd < 0) ? -errno : ring_fd;
}
int io_uring_enter(
    const int ring_fd,
    const unsigned int to_submit,
    const unsigned int min_complete,
    const unsigned int flags,
    sigset_t *sig,
    size_t sz
) {
    const int result = syscall(__NR_io_uring_enter, ring_fd, to_submit, min_complete, flags, sig, sz);
    return (result < 0) ? -errno : result;
}

const uint32_t OPERATIONS = 10000000;
const unsigned int WORKER_THREADS = 10;

int main(int argc, char *argv[]) {
    struct io_uring_params params = {};
    // params.sq_thread_idle = 50000;
    // params.flags |= IORING_SETUP_SQPOLL;

    const auto ring_fd = io_uring_setup(4096, &params);
    if (ring_fd < 0) {
        perror("io_uring_setup");
        return 1;
    }

    const auto sq_ring_size = params.sq_off.array + params.sq_entries * sizeof(__u32);
    void* sq_ptr = mmap(
        NULL,
        sq_ring_size,
        PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_POPULATE,
        ring_fd,
        IORING_OFF_SQ_RING
    );
    if (sq_ptr == MAP_FAILED) {
        perror("mmap sq_ring");
        return 1;
    }

    const auto sqes_size = params.sq_entries * sizeof(struct io_uring_sqe);
    auto* sqes = static_cast<struct io_uring_sqe *>(
        mmap(
            NULL,
            sqes_size,
            PROT_READ | PROT_WRITE,
            MAP_SHARED | MAP_POPULATE,
            ring_fd,
            IORING_OFF_SQES
            )
        );
    if (sqes == MAP_FAILED) {
        perror("mmap sqes");
        return 1;
    }

    const auto cq_ring_size = params.cq_off.cqes + params.cq_entries * sizeof(struct io_uring_cqe);
    const auto cq_ptr = mmap(NULL, cq_ring_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, ring_fd, IORING_OFF_CQ_RING);
    if (cq_ptr == MAP_FAILED) {
        perror("mmap cq_ring");
        return 1;
    }

    const auto cq_head = reinterpret_cast<std::atomic<uint32_t> *>(static_cast<char *>(sq_ptr) + params.cq_off.head);
    const auto cq_tail = reinterpret_cast<std::atomic<uint32_t> *>(static_cast<char *>(sq_ptr) + params.cq_off.tail);
    const auto cq_ring_mask = reinterpret_cast<uint32_t *>(static_cast<char *>(cq_ptr) + params.cq_off.ring_mask);
    const auto *cqes = reinterpret_cast<struct io_uring_cqe *>(static_cast<char *>(cq_ptr) + params.cq_off.cqes);


    const auto sq_head = reinterpret_cast<std::atomic<uint32_t>*>(static_cast<char*>(sq_ptr) + params.sq_off.head);
    const auto sq_tail = reinterpret_cast<std::atomic<uint32_t>*>(static_cast<char*>(sq_ptr) + params.sq_off.tail);
    const auto sq_ring_mask = reinterpret_cast<uint32_t*>(static_cast<char*>(sq_ptr) + params.sq_off.ring_mask);
    const auto sq_array = reinterpret_cast<uint32_t *>(static_cast<char *>(sq_ptr) + params.sq_off.array);
    const auto sq_flags = reinterpret_cast<std::atomic<uint32_t> *>(static_cast<char *>(sq_ptr) + params.sq_off.flags);



    std::atomic<long> start;
    std::atomic<uint32_t> submitted{0};
    std::atomic<uint32_t> completed{0};

    std::vector<std::thread> workers;
    std::queue<io_uring_cqe> completions;
    std::mutex completionLock;
    std::condition_variable completionCv;

    std::mutex submissionLock;
    std::queue<io_uring_sqe> submissions;
    std::condition_variable submissionCv;

    std::thread completer([&]() {
        while (true) {
            const auto current_head = cq_head->load();
            const auto current_tail = cq_tail->load();

            if (current_head == current_tail) {
                io_uring_enter(ring_fd, 0, 1, IORING_ENTER_GETEVENTS, nullptr, 0);
                std::this_thread::yield();
            } else {
                const auto to_process = current_tail - current_head;
                for (unsigned int i = 0; i < to_process; i++) {
                    {
                        std::lock_guard lock(completionLock);
                        completions.push(cqes[(current_head + i) & *cq_ring_mask]);
                        completionCv.notify_one();
                    }
                }

                // completed.fetch_add(to_process);

                // completionCv.notify_one();
                // completionCv.notify_all();
                cq_head->fetch_add(to_process);
            }
        }
    });

    for (int i = 0; i < WORKER_THREADS; i++) {
        workers.emplace_back([&]() {
            while (true) {
                {
                    std::unique_lock<std::mutex> lock(completionLock);
                    completionCv.wait(lock, [&] { return !completions.empty(); });
                    io_uring_cqe cqe = completions.front();
                    completions.pop();
                    auto count = completed.fetch_add(1);
                }
            }
        });
    }

    std::thread submitter([&]() {
        std::vector<io_uring_sqe> to_submit;
        while (true) {
            {
                std::unique_lock lock(submissionLock);
                submissionCv.wait(lock, [&] { return !submissions.empty(); });

                size_t count = 0;
                while (!submissions.empty() && count < 64) {
                   to_submit.push_back(submissions.front());
                   submissions.pop();
                   ++count;
               }
            }

            size_t needed = to_submit.size();
            uint32_t head, tail;
            while (true) {
                head = sq_head->load();
                tail = sq_tail->load();

                if ((tail - head) < params.sq_entries - needed) {
                    break; // Enough space to submit
                }

                // Not enough space, wait
                std::this_thread::yield();
            }

            for (int i = 0; i < to_submit.size(); i++) {
                const auto index = (tail + i) & *sq_ring_mask;
                const auto sqe = &sqes[index];
                memcpy(sqe, &to_submit[i], sizeof(io_uring_sqe));
                sq_array[index] = index;
            }

            sq_tail->store(tail + to_submit.size());

            if (params.flags & IORING_SETUP_SQPOLL) {
                if (sq_flags->load() & IORING_SQ_NEED_WAKEUP) {
                    if (io_uring_enter(ring_fd, 0, 0, IORING_ENTER_SQ_WAKEUP, nullptr, 0) < 0) {
                        perror("io_uring_enter 1");
                    }
                }
            } else {
                if (io_uring_enter(ring_fd, to_submit.size(), 0, 0, nullptr, 0) < 0) {
                    perror("io_uring_enter 2");
                }
            }


            to_submit.clear();
        }
    });


    std::vector<std::thread> submitters;

    for (int i = 0; i < 1; ++i) {
        submitters.emplace_back([&, &submitted, &start]() {
            int i1 = 0;
            while (true) {
                auto index = submitted.fetch_add(1);
                if (index == 0) {
                    auto now = std::chrono::system_clock::now();
                    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch()
                    ).count();
                    start.store(millis);
                }
                if (index >= OPERATIONS) break;
                {
                    io_uring_sqe sqe{};
                    sqe.opcode = IORING_OP_NOP;
                    sqe.user_data = i1;

                    std::lock_guard<std::mutex> lock(submissionLock);
                    submissions.push(sqe);
                }
                submissionCv.notify_one();
            }
        });
    }

    while (true) {
        auto count = completed.load();
        std::cout << "\nSubmitted: " << submitted.load() << std::endl;
        std::cout << "Completed: " << count << std::endl;
        std::cout << "sq head: " << sq_head->load() << std::endl;
        std::cout << "sq tail: " << sq_tail->load() << std::endl;
        std::cout << "cq head: " << cq_head->load() << std::endl;
        std::cout << "cq tail: " << cq_tail->load() << std::endl;
        if (count >= OPERATIONS) {
            auto now = std::chrono::system_clock::now();
            auto end_millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now.time_since_epoch()
            ).count();
            auto start_millis = start.load();
            auto millis = end_millis - start_millis;
            auto seconds = (double) millis / 1000.0;
            auto ops = (int) ((double) OPERATIONS / seconds);
            std::cout << ops << " OP/S, " << OPERATIONS << " operations, " << seconds << " seconds" << std::endl;
            break;
        }

        std::this_thread::yield();
    }

    close(ring_fd);
}
//
// template<typename T>
// concept IsReadType =
//         std::same_as<T, int> ||
//         std::same_as<T, short> ||
//         std::same_as<T, char> ||
//         std::same_as<T, float> ||
//         std::same_as<T, double> ||
//         std::same_as<T, long> ||
//         std::same_as<T, char *>;
//
// template<typename T>
// concept IsWriteType =
//     std::same_as<T, int> ||
//     std::same_as<T, short> ||
//     std::same_as<T, char> ||
//     std::same_as<T, float> ||
//     std::same_as<T, double> ||
//     std::same_as<T, long> ||
//     std::same_as<T, char *>;
//
// struct BaseAwaitable {
//     std::coroutine_handle<> coro_handle;
//     virtual void on_complete(const int res, const unsigned int flags) {}
//     virtual ~BaseAwaitable() = default;
// };
//
// template <IsWriteType T>
// struct WriteAwaitable {
//     bool await_ready() const noexcept { return false; }
//
//     void await_suspend(std::coroutine_handle<> h) {
//     }
//
//     int await_resume() noexcept { return 1; }
// };
//
// template <IsReadType T>
// struct ReadAwaitable{
//     ReadAwaitable(const int socket, io_uring *ring) {
//
//     }
//
//     bool await_ready() const noexcept { return false; }
//
//     void await_suspend(std::coroutine_handle<> h) {
//     }
//
//     T await_resume() noexcept { return 1; }
// };
//
// struct Read {
//     Read(
//         const size_t buffer_size,
//         const int socket,
//         io_uring *ring
//     ) : intReader(socket, ring),
//         shortReader(socket, ring),
//         byteReader(socket, ring),
//         floatReader(socket, ring),
//         doubleReader(socket, ring),
//         longReader(socket, ring),
//         bytesReader(socket, ring) {
//         ring->sq.kflags
//     }
//
//     ReadAwaitable<int> get_int() const {
//         return intReader;
//     }
//
// private:
//     ReadAwaitable<int> intReader;
//     ReadAwaitable<short> shortReader;
//     ReadAwaitable<char> byteReader;
//     ReadAwaitable<float> floatReader;
//     ReadAwaitable<double> doubleReader;
//     ReadAwaitable<long> longReader;
//     ReadAwaitable<char*> bytesReader;
// };
//
// struct Write {
//     Write(const size_t buffer_size, const int socket, io_uring *ring) {
//
//     }
//
// };
//
// struct CloseAwaitable : BaseAwaitable{
//
//     CloseAwaitable(const int socket, io_uring *ring) {
//
//     }
//
//     bool await_ready() const noexcept { return false; }
//
//     void await_suspend(std::coroutine_handle<> h) {
//     }
//
//     bool await_resume() noexcept { return 1; }
// };
//
// struct Connection{
//     Connection(
//         const size_t buffer_size,
//         const int fd,
//         io_uring *ring
//     ) : reader(buffer_size, fd, ring),
//         writer(buffer_size, fd, ring),
//         closer(fd, ring),
//         fd(fd) {}
//
//     CloseAwaitable close() {
//         return closer;
//     }
//
//     Read read() const {
//         return reader;
//     }
//
//     Write write() const {
//         return writer;
//     }
//
// private:
//     Read reader;
//     Write writer;
//     CloseAwaitable closer;
//     int fd;
// };
//
// struct ConnectAwaitable : BaseAwaitable {
//     ~ConnectAwaitable()() override = default;
//
//     ConnectAwaitable(
//         const int client_socket,
//         const size_t buffer_size,
//         const sockaddr_in addr,
//         io_uring *ring,
//         std::mutex *ring_mutex
//     ) : buffer_size(buffer_size), ring(ring), addr(addr), client_socket(client_socket) {
//     }
//
//     bool await_ready() const noexcept { return false; }
//
//     void await_suspend(std::coroutine_handle<> h) {
//         this->coro_handle = h;
//         std::lock_guard guard(*ring_mutex);
//         io_uring_sqe* sqe = io_uring_get_sqe(ring);
//         io_uring_prep_connect(sqe, client_socket, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
//         sqe->user_data = reinterpret_cast<uint64_t>(this);
//         io_uring_submit(ring);
//     }
//
//     Connection await_resume() noexcept {
//         return Connection{buffer_size, client_socket, ring};
//     }
//
//     void on_complete(const int res, const unsigned int flags) override {
//         if (res < 0) {
//             close(client_socket);
//             throw std::runtime_error(std::string("connect failed: ") + std::strerror(errno));
//         }
//     }
//
// private:
//     std::mutex *ring_mutex;
//     size_t buffer_size;
//     io_uring* ring;
//     sockaddr_in addr;
//     int client_socket;
// };
//
// struct AcceptAwaitable : BaseAwaitable {
//
//     ~AcceptAwaitable()() override = default;
//     AcceptAwaitable(
//         const int server_socket,
//         const size_t buffer_size,
//         const sockaddr_in addr,
//         io_uring *ring,
//         std::mutex *mutex
//     ) : buffer_size(buffer_size), ring(ring), addr(addr), server_socket(server_socket) {
//     }
//
//     bool await_ready() const noexcept { return false; }
//
//     void await_suspend(std::coroutine_handle<> h) {
//         this->coro_handle = h;
//         std::lock_guard guard(*ring_mutex);
//         io_uring_sqe* sqe = io_uring_get_sqe(ring);
//         io_uring_prep_accept(sqe, server_socket, reinterpret_cast<sockaddr *>(&addr), sizeof(addr), 0);
//         sqe->user_data = reinterpret_cast<uint64_t>(this);
//         io_uring_submit(ring);
//     }
//
//     Connection await_resume() noexcept {
//         return Connection{buffer_size, client_socket, ring};
//     }
//
//     void on_complete(const int res, const unsigned int flags) override {
//         if (res < 0) {
//
//             close(server_socket);
//             throw std::runtime_error(std::string("accept failed: ") + std::strerror(errno));
//         }
//
//         client_socket = res;
//     }
//
// private:
//     std::mutex* ring_mutex;
//     size_t buffer_size;
//     io_uring* ring;
//     sockaddr_in addr;
//     int server_socket, client_socket;
// };
//
// class Provider {
//     std::mutex ring_mutex;
//     io_uring ring{};
//     int ring_fd;
//     size_t buffer_size;
//     std::vector<std::thread> workers;
//     std::atomic<bool> shutdown{false};
//     int event_fd{};
//
//     Provider(const size_t buffer_size, const size_t workers_size) {
//         if (io_uring_queue_init(256, &ring, 0)) {
//             throw std::runtime_error("io_uring init failed");
//         }
//         this->buffer_size = buffer_size;
//         ring_fd = ring.ring_fd;
//
//         for (size_t i = 0; i < workers_size; ++i) {
//             workers.emplace_back([this] {
//                 while (!shutdown.load()) {
//                     io_uring_cqe *cqe = nullptr;
//                     io_uring_cq_advance()
//                     io_uring_wait_cqes()
//                     if (const auto result = io_uring_wait_cqe(&ring, &cqe); result < 0) {
//                         perror("io_uring_wait_cqe failed");
//                         break;
//                     }
//
//                     if (cqe->user_data == 0) {
//                         std::cout << "got shut down" << std::endl;
//                         break;
//                     }
//
//                     const auto user_data = cqe->user_data;
//                     const auto res = cqe->res;
//                     const auto flags = cqe->flags;
//                     io_uring_cqe_seen(&ring, cqe);
//
//                     if (user_data != 0) {
//                         auto *base = reinterpret_cast<BaseAwaitable*>(user_data);
//                         base->on_complete(res, flags);
//                         base->coro_handle.resume();
//                     }
//                 }
//             });
//         }
//     }
//
//
//     ~Provider() {
//         shutdown.store(true);
//
//         for (auto& worker : workers) {
//             if (worker.joinable()) worker.join();
//         }
//
//         // close(event_fd);
//         io_uring_queue_exit(&ring);
//     }
//
//     ConnectAwaitable connect(const char* addr, const auto port) {
//         const auto client_socket = socket(AF_INET, SOCK_STREAM, 0);
//         if (client_socket < 0) {
//             throw std::runtime_error(std::string("client socket creation failed: ") + std::strerror(errno));
//         }
//         sockaddr_in serv_addr{};
//         serv_addr.sin_family = AF_INET;
//         serv_addr.sin_port = htons(port);
//         inet_pton(AF_INET, addr, &serv_addr.sin_addr);
//         return ConnectAwaitable{client_socket, buffer_size, serv_addr, &ring, &ring_mutex};
//     }
//
//     AcceptAwaitable accept(const sockaddr_in &addr) {
//         const auto server_socket = socket(AF_INET, SOCK_STREAM, 0);
//         if (server_socket < 0) {
//             throw std::runtime_error(std::string("server socket creation failed: ") + std::strerror(errno));
//         }
//
//         const auto bind = bind(server_socket, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
//         if (bind < 0) {
//             throw std::runtime_error(std::string("server bind failed: ") + std::strerror(errno));
//         }
//
//         if (listen(server_socket, SOMAXCONN) < 0) {
//             throw std::runtime_error(std::string("server listen failed: ") + std::strerror(errno));
//         }
//
//         return AcceptAwaitable{server_socket, buffer_size, addr, &ring, &ring_mutex};
//     }
// };
//
// struct Task {
//     struct promise_type {
//         Task get_return_object() { return {}; }
//         std::suspend_never initial_suspend() { return {}; }
//         std::suspend_never final_suspend() noexcept { return {}; }
//         void return_void() {}
//         void unhandled_exception() { std::terminate(); }
//     };
// };
//
// Task run_server(const Provider &provider) {
//     const auto connection = co_await provider.connect();
//     std::cout<<"run_server 2"<<std::endl;
//
// }
//
// Task run_client() {
//     std::cout<<"run_client"<<std::endl;
//     std::cout<<"run_client2"<<std::endl;
// }
// //
// // int main() {
// //     run_server();
// //     // run_client();
// //     std::cout<<"main"<<std::endl;
// //     // Keep alive to see output
// //     std::this_thread::sleep_for(std::chrono::seconds(10));
// }