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
#include <fcntl.h>   // for open()
#include <functional>
#include <unistd.h>  // for close()
#include <sys/eventfd.h>
#include <poll.h>
#include <csignal>
#include <linux/time_types.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>

constexpr auto OPERATIONS = 10000000;
constexpr auto MAX_SUBMISSION_BATCH = 1000;
constexpr unsigned long EVENTFD_EXIT = 0xDEADBEEF;
constexpr unsigned long EVENTFD_WAKE = 0xBEEFDEAD;
auto RUNNING = std::atomic(true);


void handle_signal(int signum) {
    std::cout << "\nReceived signal " << signum << ", shutting down..." << std::endl;
    RUNNING.store(false);
}

void setup_signal_handlers() {
    struct sigaction sa{};
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGINT, &sa, nullptr);   // Ctrl+C
    sigaction(SIGTERM, &sa, nullptr);  // kill
}

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
int io_uring_register(unsigned int ring_fd, unsigned int op, void *arg, unsigned int nr_args) {
    const int result = syscall(__NR_io_uring_register, ring_fd, op, arg, nr_args);
    return (result < 0) ? -errno : result;
}


struct BaseAwaitable {
    virtual void on_complete(const int res, const unsigned int flags) {}
    virtual ~BaseAwaitable() = default;
};

struct Task {
    struct promise_type {
        Task get_return_object() { return {}; }  // No handle needed
        std::suspend_never initial_suspend() { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() { std::terminate(); }
    };
};

template <typename F>
concept Predicate = requires(F f, int i) {
    { f(i) } -> std::same_as<bool>;
};


class Ring {
public:
    explicit Ring(io_uring_params& params) {
        fd_ = io_uring_setup(32000, &params);
        if (fd_ < 0) throw std::runtime_error("io_uring_setup failed");
        params_ = params;
        sq_ring_size_ = params.sq_off.array + params.sq_entries * sizeof(__u32);
        sq_ptr_ = mmap(nullptr, sq_ring_size_, PROT_READ | PROT_WRITE,MAP_SHARED | MAP_POPULATE, fd_, IORING_OFF_SQ_RING);
        if (sq_ptr_ == MAP_FAILED) throw std::runtime_error("mmap SQ ring failed");
        sqes_size_ = params.sq_entries * sizeof(io_uring_sqe);
        sqes_ = static_cast<io_uring_sqe*>(mmap(nullptr, sqes_size_, PROT_READ | PROT_WRITE,MAP_SHARED | MAP_POPULATE, fd_, IORING_OFF_SQES));
        if (sqes_ == MAP_FAILED) throw std::runtime_error("mmap SQEs failed");
        cq_ring_size_ = params.cq_off.cqes + params.cq_entries * sizeof(io_uring_cqe);
        cq_ptr_ = mmap(nullptr, cq_ring_size_, PROT_READ | PROT_WRITE,MAP_SHARED | MAP_POPULATE, fd_, IORING_OFF_CQ_RING);
        if (cq_ptr_ == MAP_FAILED) throw std::runtime_error("mmap CQ ring failed");
        sq_head_ = reinterpret_cast<std::atomic<uint32_t>*>((char*)sq_ptr_ + params.sq_off.head);
        sq_tail_ = reinterpret_cast<std::atomic<uint32_t>*>((char*)sq_ptr_ + params.sq_off.tail);
        sq_flags_ = reinterpret_cast<std::atomic<uint32_t>*>((char*)sq_ptr_ + params.sq_off.flags);
        sq_ring_mask_ = reinterpret_cast<uint32_t*>((char*)sq_ptr_ + params.sq_off.ring_mask);
        sq_array_ = reinterpret_cast<uint32_t*>((char*)sq_ptr_ + params.sq_off.array);
        cq_head_ = reinterpret_cast<std::atomic<uint32_t>*>((char*)cq_ptr_ + params.cq_off.head);
        cq_tail_ = reinterpret_cast<std::atomic<uint32_t>*>((char*)cq_ptr_ + params.cq_off.tail);
        cq_ring_mask_ = reinterpret_cast<uint32_t*>((char*)cq_ptr_ + params.cq_off.ring_mask);
        cqes_ = reinterpret_cast<io_uring_cqe*>((char*)cq_ptr_ + params.cq_off.cqes);
    }

    ~Ring() {
        if (sq_ptr_) munmap(sq_ptr_, sq_ring_size_);
        if (sqes_) munmap(sqes_, sqes_size_);
        if (cq_ptr_) munmap(cq_ptr_, cq_ring_size_);
        if (fd_ >= 0) close(fd_);
    }

    void start() const {
        while (RUNNING.load(std::memory_order_acquire)) {
            const auto current_head = cq_head_->load(std::memory_order_acquire);
            const auto current_tail = cq_tail_->load(std::memory_order_acquire);
            auto to_process = current_tail - current_head;

            if (to_process == 0) {
                io_uring_enter(fd_, 0, 1, IORING_ENTER_GETEVENTS, nullptr, 0);
                to_process = cq_tail_->load(std::memory_order_acquire) - cq_head_->load(std::memory_order_acquire);
            }

            if (to_process > 0) {
                for (unsigned int i = 0; i < to_process; ++i) {
                    if (const auto& cq = cqes_[(current_head + i) & *cq_ring_mask_]; cq.user_data == EVENTFD_EXIT) {
                        RUNNING.store(false, std::memory_order_release);
                    } else {
                        if (const auto base = reinterpret_cast<BaseAwaitable*>(cq.user_data)) {
                            base->on_complete(cq.res, cq.flags);
                        } else {
                            throw std::runtime_error("Got a non base awaitable error!");
                        }
                    }
                }

                cq_head_->fetch_add(to_process, std::memory_order_release);
            }
        }

        std::cout << "Broke out of here?" << std::endl;
    }

    void submit(const io_uring_sqe* passed) const {
        const unsigned int head = sq_head_->load(std::memory_order_acquire);
        const unsigned int tail = sq_tail_->load(std::memory_order_relaxed);
        const unsigned int used = tail - head;
        const unsigned int space = params_.sq_entries - used;
        if (space == 0) throw std::runtime_error("ran out of ring space!");

        const auto index = tail & *sq_ring_mask_;
        io_uring_sqe* sqe = &sqes_[index];
//        memset(&sqe, 0, sizeof(sqe));
        memcpy(&sqes_[index], passed, sizeof(io_uring_sqe));


        sq_array_[index] = index;
        sq_tail_->store(tail + 1, std::memory_order_release);

        if (params_.flags & IORING_SETUP_SQPOLL) {
            if (sq_flags_->load(std::memory_order_acquire) & IORING_SQ_NEED_WAKEUP) {
                if (io_uring_enter(fd_, 0, 0, IORING_ENTER_SQ_WAKEUP, nullptr, 0) < 0) {
                    throw std::runtime_error("io_uring_enter failed");
                }
            }
        } else if (io_uring_enter(fd_, 1, 0, 0, nullptr, 0)) {
            throw std::runtime_error("io_uring_enter failed");
        }
    }

    template<typename... Sqes>
    requires (std::same_as<Sqes, io_uring_sqe*> && ...)
    void submit_batch(const Sqes&... sqes_batch) const {
        const unsigned int head = sq_head_->load(std::memory_order_acquire);
        const unsigned int tail = sq_tail_->load(std::memory_order_relaxed);
        const unsigned int used = tail - head;
        const unsigned int space = params_.sq_entries - used;
        constexpr auto batch_size = sizeof...(sqes_batch);

        if (batch_size > space) {
            throw std::runtime_error("Not enough space in SQ ring for batch submission");
        }

        unsigned int i = 0;
        (void) std::initializer_list<int>{
            ([&] {
                const auto index = (tail + i) & *sq_ring_mask_;
                sqes_[index] = *sqes_batch;
                sq_array_[index] = index;
                return i++;
            }(), 0)...
        };
        sq_tail_->store(tail + batch_size, std::memory_order_release);

        if (params_.flags & IORING_SETUP_SQPOLL) {
            if (sq_flags_->load(std::memory_order_acquire) & IORING_SQ_NEED_WAKEUP) {
                if (io_uring_enter(fd_, 0, 0, IORING_ENTER_SQ_WAKEUP, nullptr, 0) < 0) {
                    throw std::runtime_error("io_uring_enter failed");
                }
            }
        } else {
            if (io_uring_enter(fd_, batch_size, 0, 0, nullptr, 0) < 0) {
                throw std::runtime_error("io_uring_enter failed");
            }
        }
    }

    // --- Public Getters (Read-only) ---
    [[nodiscard]] int fd() const { return fd_; }
    [[nodiscard]] const io_uring_params& params() const { return params_; }
    [[nodiscard]] const io_uring_cqe* cqes() const { return cqes_; }
    [[nodiscard]] std::atomic<uint32_t>* cq_head() const { return cq_head_; }
    [[nodiscard]] std::atomic<uint32_t>* cq_tail() const { return cq_tail_; }
    [[nodiscard]] const uint32_t* cq_ring_mask() const { return cq_ring_mask_; }

private:

    int fd_ = -1;
    io_uring_params params_{};

    // SQ
    void* sq_ptr_ = nullptr;
    io_uring_sqe* sqes_ = nullptr;
    std::atomic<uint32_t>* sq_head_ = nullptr;
    std::atomic<uint32_t>* sq_tail_ = nullptr;
    std::atomic<uint32_t>* sq_flags_ = nullptr;
    uint32_t* sq_ring_mask_ = nullptr;
    uint32_t* sq_array_ = nullptr;

    // CQ
    void* cq_ptr_ = nullptr;
    std::atomic<uint32_t>* cq_head_ = nullptr;
    std::atomic<uint32_t>* cq_tail_ = nullptr;
    uint32_t* cq_ring_mask_ = nullptr;
    io_uring_cqe* cqes_ = nullptr;

    // Sizes
    size_t sq_ring_size_ = 0;
    size_t sqes_size_ = 0;
    size_t cq_ring_size_ = 0;
};

struct Delay : BaseAwaitable {
    template<typename Rep, typename Period>
    Delay(Ring &ring, std::chrono::duration<Rep, Period> duration): ring_(ring) {
        const auto ms = duration_cast<std::chrono::milliseconds>(duration).count();
        ts_.tv_sec = ms / 1000;
        ts_.tv_nsec = (ms % 1000) * 1'000'000;
    }

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) {
        h_ = h;
        io_uring_sqe sqe{};
        memset(&sqe, 0, sizeof(sqe));
        sqe.opcode = IORING_OP_TIMEOUT;
        sqe.addr = reinterpret_cast<uint64_t>(&ts_);
        sqe.len = 1;
        sqe.user_data = reinterpret_cast<uint64_t>(this);
        ring_.submit(&sqe);
    }

    void on_complete(const int res, const unsigned int flags) override {
        h_.resume();
    }

    void await_resume() noexcept {
    }

private:
    Ring &ring_;
    __kernel_timespec ts_{};
    std::coroutine_handle<> h_;
};

struct NoOp : BaseAwaitable {
    explicit NoOp(Ring &ring): ring_(ring) {

    }

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) {
        h_ = h;
        io_uring_sqe sqe{};
        memset(&sqe, 0, sizeof(sqe));
        sqe.opcode = IORING_OP_NOP;
        sqe.user_data = reinterpret_cast<uint64_t>(this);
        ring_.submit(&sqe);
    }

    void on_complete(const int res, const unsigned int flags) override {
        h_.resume();
    }

    void await_resume() noexcept {
    }

private:
    Ring &ring_;
    __kernel_timespec ts_{};
    std::coroutine_handle<> h_;
};

Task after(Ring &ring, const std::chrono::milliseconds delay, const std::function<void()> &block) {
    co_await Delay{ring, delay};
    block();
}


template<Predicate Function>
Task every(Ring &ring, std::chrono::milliseconds interval, Function block) {
    int count = 0;
    while (true) {
        co_await Delay{ring, interval};
        if (block(count++)) break;
    }
}

Task benchmark(Ring& ring, int ops) {
    using Clock = std::chrono::high_resolution_clock;
    const auto start = Clock::now();

    int i = 0;
    while (i++ < ops) {
        co_await NoOp{ring};
    }

    const auto end = Clock::now();
    const auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
    const double ns_per_op = static_cast<double>(duration.count()) / ops;
    const double ops_per_sec = 1e9 / ns_per_op;

    std::cout << "Benchmark results:\n"
              << "  Operations:     " << ops << "\n"
              << "  Total time:     " << duration.count() << " ns\n"
              << "  Time per op:    " << ns_per_op << " ns\n"
              << "  Throughput:     " << ops_per_sec << " ops/sec\n";
}


template<typename T>
concept IsReadType =
        std::same_as<T, int> ||
        std::same_as<T, short> ||
        std::same_as<T, char> ||
        std::same_as<T, float> ||
        std::same_as<T, double> ||
        std::same_as<T, long> ||
        std::same_as<T, char *>;

template<typename T>
concept IsWriteType =
    std::same_as<T, int> ||
    std::same_as<T, short> ||
    std::same_as<T, char> ||
    std::same_as<T, float> ||
    std::same_as<T, double> ||
    std::same_as<T, long> ||
    std::same_as<T, char *>;

struct Connection {

};


struct ConnectAwaitable : BaseAwaitable {

    ConnectAwaitable(
        Ring &ring,
        const int client_socket,
        const size_t buffer_size,
        const sockaddr_in addr
    ) : buffer_size(buffer_size), ring(ring), serv_addr(addr), client_socket(client_socket) {
    }

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) {
        std::cout << "Client socket: " << client_socket << std::endl;
        this->handle = h;
        io_uring_sqe sqe{};
        memset(&sqe, 0, sizeof(sqe));
        sqe.opcode = IORING_OP_CONNECT;
        sqe.fd = client_socket;
        sqe.addr = reinterpret_cast<__u64>(reinterpret_cast<sockaddr*>(&serv_addr));
        sqe.addr_len = 0;
        sqe.off = reinterpret_cast<uint64_t>(sizeof(serv_addr));
        sqe.user_data = reinterpret_cast<uint64_t>(this);

        std::cout << "Connect address alignment: "
                  << (reinterpret_cast<uintptr_t>(&serv_addr) % alignof(sockaddr_in)) << "\n";

        int current_flags = fcntl(client_socket, F_GETFL);
        if (!(current_flags & O_NONBLOCK)) {
            std::cerr << "CRITICAL: Socket is blocking despite fcntl!\n";
            exit(1);
        }

        int err = 0;
        socklen_t len = sizeof(err);
        getsockopt(client_socket, SOL_SOCKET, SO_ERROR, &err, &len);
        if (err != 0) {
            std::cerr << "Socket has error state: " << strerror(err) << "\n";
        }

        ring.submit(&sqe);
    }

    Connection await_resume() noexcept {
        return Connection{};
    }

    void on_complete(const int res, const unsigned int flags) override {
        if (res < 0) {
            close(client_socket);
            throw std::runtime_error(std::string("connect failed: ") + std::strerror(-res));
        }

        handle.resume();
    }

private:
    std::coroutine_handle<> handle;
    size_t buffer_size;
    Ring &ring;
    sockaddr_in serv_addr;
    int client_socket;
};

struct AcceptAwaitable : BaseAwaitable {
    AcceptAwaitable(
        Ring& ring,
        const int server_socket,
        const size_t buffer_size,
        const sockaddr_in addr
    ) : buffer_size(buffer_size), ring(ring), addr(addr), server_socket(server_socket) {
    }

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) {
        this->handle = h;
        io_uring_sqe sqe{};
        memset(&sqe, 0, sizeof(sqe));
        sqe.opcode = IORING_OP_ACCEPT;
//        sqe.ioprio = IORING_ACCEPT_MULTISHOT;
        sqe.fd = server_socket;
        const auto addr_len = sizeof(addr);
        sqe.addr = reinterpret_cast<uintptr_t>(&addr);
        sqe.off = reinterpret_cast<uint64_t>(&addr_len);
        sqe.addr_len = 0;  // Must be zero for accept
        sqe.user_data = reinterpret_cast<uint64_t>(this);


        ring.submit(&sqe);
    }

    Connection await_resume() noexcept {
        return Connection{};
    }

    void on_complete(const int res, const unsigned int flags) override {
        std::cout << "Got the completion!" << std::endl;
        if (res < 0) {
            close(server_socket);
            int err = -res;
            std::cout << res << std::endl;
            throw std::runtime_error(std::string("accept failed: ") + std::strerror(err));
        }

        std::cout << "Got accept!" << std::endl;
        client_socket = res;
        handle.resume();
    }

private:
    std::coroutine_handle<> handle;
    size_t buffer_size;
    Ring &ring;
    sockaddr_in addr;
    int server_socket, client_socket;
};

class Provider {

    Ring& ring_;
    size_t buffer_size_;

public:
    Provider(Ring& ring, const size_t buffer_size): ring_(ring), buffer_size_(buffer_size) {

    }

    void connect(const char* addr, const auto port) {
        const auto client_socket = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, IPPROTO_TCP);
        if (client_socket < 0) {
            throw std::runtime_error(std::string("client socket creation failed: ") + std::strerror(errno));
        }

        const auto flags = fcntl(client_socket, F_GETFL, 0);
        if (flags == -1) throw std::runtime_error("fcntl get flags failed");
        if (fcntl(client_socket, F_SETFL, flags | O_NONBLOCK) == -1) {
            throw std::runtime_error("fcntl set O_NONBLOCK failed");
        }

        const auto flag = 1;
        if (setsockopt(client_socket, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(flag)) < 0) {
            throw std::runtime_error("setsockopt TCP_NODELAY failed");
        }

        sockaddr_in serv_addr{};
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(port);
        if (inet_pton(AF_INET, addr, &serv_addr.sin_addr) <= 0) {
            throw std::runtime_error("invalid IP address");
        }

        int ret = ::connect(client_socket, reinterpret_cast<sockaddr*>(&serv_addr), sizeof(serv_addr));
        if (ret < 0) {
            if (errno == EINPROGRESS) {
                std::cerr << "connect() in progress (EINPROGRESS), non-blocking socket.\n";
                // Good, expected for non-blocking sockets
            } else {
                std::cerr << "connect() syscall failed: " << std::strerror(errno) << "\n";
                close(client_socket);
                return;
            }
        } else {
            std::cout << "connect() succeeded immediately!\n";
        }

//        close(client_socket);

//        return ConnectAwaitable{ring_, client_socket, buffer_size_, serv_addr};
    }

    AcceptAwaitable accept(const char* addr, const auto port) {
        sockaddr_in serv_addr{};
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(port);
        if (inet_pton(AF_INET, addr, &serv_addr.sin_addr) <= 0) {
            throw std::runtime_error("invalid IP address");
        }

        const auto server_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (server_socket < 0) {
            throw std::runtime_error(std::string("server socket creation failed: ") + std::strerror(errno));
        }
//
//        const auto flags = fcntl(server_socket, F_GETFL, 0);
//        if (flags == -1) throw std::runtime_error("fcntl get flags failed");
//        if (fcntl(server_socket, F_SETFL, flags | O_NONBLOCK) == -1) {
//            throw std::runtime_error("fcntl set O_NONBLOCK failed");
//        }
//
//        const auto flag = 1;
//        if (setsockopt(server_socket, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(flag)) < 0) {
//            throw std::runtime_error("setsockopt TCP_NODELAY failed");
//        }

        const auto bind_result = bind(server_socket, reinterpret_cast<const sockaddr *>(&serv_addr), sizeof(serv_addr));
        if (bind_result < 0) {
            throw std::runtime_error(std::string("server bind failed: ") + std::strerror(errno));
        }
        if (listen(server_socket, SOMAXCONN) < 0) {
            throw std::runtime_error(std::string("server listen failed: ") + std::strerror(errno));
        }

        std::cout << "Going to awaitable" << std::endl;
        return AcceptAwaitable { ring_, server_socket, buffer_size_, serv_addr };
    }
};

//Task client(Ring& ring, Provider& provider, const char* addr, const auto port) {
//    try {
//        // co_await your connect awaitable
//        std::cout << "Now connecting" << std::endl;
//        auto connection = co_await provider.connect(addr, port);
//        std::cout << "Connected to server!\n";
//        co_await Delay { ring, std::chrono::milliseconds(5000) };
//        // Use connection ...
//    } catch (const std::exception& e) {
//        std::cerr << "Connect failed: " << e.what() << '\n';
//    }
//}

Task server(Ring& ring, Provider& provider, const char* addr, const auto port) {

    try {
        std::cout << "Waiting for connection?" << std::endl;
        auto accepted = co_await provider.accept(addr, port);
        std::cout << "Accepted connection!\n";
        co_await Delay { ring, std::chrono::milliseconds(5000) };
        // Use accepted connection ...
    } catch (const std::exception& e) {
        std::cerr << "Accept failed: " << e.what() << '\n';
    }
}

int main(int argc, char* argv[]) {
    setup_signal_handlers();
    io_uring_params params = {};
    params.sq_thread_idle = 10000000;
    params.flags |= IORING_SETUP_SQPOLL | IORING_SETUP_CQSIZE;
    params.cq_entries = 64000;


    Ring ring(params);

    // benchmark(ring, 1000000);
    auto provider = Provider(ring, 65535);
    server(ring, provider, "127.0.0.1", 6969);
    provider.connect("127.0.0.1", 6969);
//    client(ring, provider, "127.0.0.1", 6981);

    ring.start();

    return 0;
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
//         const sockaddr_in serv_addr,
//         io_uring *ring,
//         std::mutex *ring_mutex
//     ) : buffer_size(buffer_size), ring(ring), serv_addr(serv_addr), client_socket(client_socket) {
//     }
//
//     bool await_ready() const noexcept { return false; }
//
//     void await_suspend(std::coroutine_handle<> h) {
//         this->coro_handle = h;
//         std::lock_guard guard(*ring_mutex);
//         io_uring_sqe* sqe = io_uring_get_sqe(ring);
//         io_uring_prep_connect(sqe, client_socket, reinterpret_cast<sockaddr *>(&serv_addr), sizeof(serv_addr));
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
//     sockaddr_in serv_addr;
//     int client_socket;
// };
//
// struct AcceptAwaitable : BaseAwaitable {
//
//     ~AcceptAwaitable()() override = default;
//     AcceptAwaitable(
//         const int server_socket,
//         const size_t buffer_size,
//         const sockaddr_in serv_addr,
//         io_uring *ring,
//         std::mutex *mutex
//     ) : buffer_size(buffer_size), ring(ring), serv_addr(serv_addr), server_socket(server_socket) {
//     }
//
//     bool await_ready() const noexcept { return false; }
//
//     void await_suspend(std::coroutine_handle<> h) {
//         this->coro_handle = h;
//         std::lock_guard guard(*ring_mutex);
//         io_uring_sqe* sqe = io_uring_get_sqe(ring);
//         io_uring_prep_accept(sqe, server_socket, reinterpret_cast<sockaddr *>(&serv_addr), sizeof(serv_addr), 0);
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
//     sockaddr_in serv_addr;
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
//     ConnectAwaitable connect(const char* serv_addr, const auto port) {
//         const auto client_socket = socket(AF_INET, SOCK_STREAM, 0);
//         if (client_socket < 0) {
//             throw std::runtime_error(std::string("client socket creation failed: ") + std::strerror(errno));
//         }
//         sockaddr_in serv_addr{};
//         serv_addr.sin_family = AF_INET;
//         serv_addr.sin_port = htons(port);
//         inet_pton(AF_INET, serv_addr, &serv_addr.sin_addr);
//         return ConnectAwaitable{client_socket, buffer_size, serv_addr, &ring, &ring_mutex};
//     }
//
//     AcceptAwaitable accept(const sockaddr_in &serv_addr) {
//         const auto server_socket = socket(AF_INET, SOCK_STREAM, 0);
//         if (server_socket < 0) {
//             throw std::runtime_error(std::string("server socket creation failed: ") + std::strerror(errno));
//         }
//
//         const auto bind = bind(server_socket, reinterpret_cast<sockaddr *>(&serv_addr), sizeof(serv_addr));
//         if (bind < 0) {
//             throw std::runtime_error(std::string("server bind failed: ") + std::strerror(errno));
//         }
//
//         if (listen(server_socket, SOMAXCONN) < 0) {
//             throw std::runtime_error(std::string("server listen failed: ") + std::strerror(errno));
//         }
//
//         return AcceptAwaitable{server_socket, buffer_size, serv_addr, &ring, &ring_mutex};
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