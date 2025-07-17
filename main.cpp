#include <iostream>
#include <coroutine>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <thread>
#include <vector>
#include <asm-generic/int-ll64.h>
#include <sys/eventfd.h>
#include <linux/io_uring.h>


int io_uring_setup(const unsigned entries, io_uring_params *params) {
    const int ring_fd = syscall(__NR_io_uring_setup, entries, params);
    return (ring_fd < 0) ? -errno : ring_fd;
}
int io_uring_enter(
    const int ring_fd,
    const unsigned int to_submit,
    const unsigned int min_complete,
    const unsigned int flags
) {
    const int result = syscall(__NR_io_uring_enter, ring_fd, to_submit, min_complete, flags, NULL, 0);
    return (result < 0) ? -errno : result;
}

int main(int argc, char *argv[]) {
    auto params = io_uring_params {};
    params.sq_thread_idle = 5000;
    params.flags |= IORING_SETUP_SQPOLL;
    io_uring_wa

    const auto ring_fd = io_uring_setup(4096, &params);
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