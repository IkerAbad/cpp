#include "net/socket.hpp"

#include <algorithm>
#include <cstdint>
#include <chrono>
#include <format>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#endif

namespace rts::net {

namespace {

constexpr std::size_t kFrameHeader = sizeof(std::uint32_t);
constexpr unsigned kByteBits = 8;
constexpr unsigned kByteMask = 0xffU;
// Bytes leídos por llamada a recv.
constexpr std::size_t kReadChunk = 16 * 1024;

#ifdef _WIN32
using RawSocket = SOCKET;
constexpr RawSocket kRawInvalid = ~RawSocket{0};
using PollFd = WSAPOLLFD;

// Las macros MAKEWORD, FIONBIO e INADDR_ANY de Winsock llevan conversiones al estilo C
// que -Wold-style-cast señala donde se expanden: se escriben sus valores aquí.
constexpr WORD kWinsockVersion = 0x0202;                     // MAKEWORD(2, 2)
constexpr long kFionbio = static_cast<long>(0x8004667EUL);  // _IOW('f', 126, u_long)

// Winsock exige WSAStartup antes de nada; se hace una vez y se deja hasta el final.
bool ensure_init() {
    static const bool ok = [] {
        WSADATA wsa{};
        return WSAStartup(kWinsockVersion, &wsa) == 0;
    }();
    return ok;
}
int last_error() { return WSAGetLastError(); }
bool would_block(int err) { return err == WSAEWOULDBLOCK; }
bool in_progress(int err) { return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS; }
void close_raw(RawSocket s) { closesocket(s); }
bool set_nonblocking(RawSocket s) {
    u_long mode = 1;
    return ioctlsocket(s, kFionbio, &mode) == 0;
}
std::int64_t sys_send(RawSocket s, const std::uint8_t* data, std::size_t n) {
    const int len = static_cast<int>(std::min<std::size_t>(n, static_cast<std::size_t>(INT32_MAX)));
    return send(s, reinterpret_cast<const char*>(data), len, 0);
}
std::int64_t sys_recv(RawSocket s, std::uint8_t* data, std::size_t n) {
    return recv(s, reinterpret_cast<char*>(data), static_cast<int>(n), 0);
}
int sys_poll(PollFd* fds, std::size_t n, int timeout_ms) {
    return WSAPoll(fds, static_cast<ULONG>(n), timeout_ms);
}
std::string error_text(int err) { return std::format("error de Winsock {}", err); }
#else
using RawSocket = int;
constexpr RawSocket kRawInvalid = -1;
using PollFd = pollfd;

bool ensure_init() { return true; }
int last_error() { return errno; }
bool would_block(int err) { return err == EAGAIN || err == EWOULDBLOCK; }
bool in_progress(int err) { return err == EINPROGRESS; }
void close_raw(RawSocket s) { ::close(s); }
bool set_nonblocking(RawSocket s) {
    const int flags = fcntl(s, F_GETFL, 0);
    return flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
}
#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;  // sin SIGPIPE si el otro ya cerró
#else
constexpr int kSendFlags = 0;
#endif
std::int64_t sys_send(RawSocket s, const std::uint8_t* data, std::size_t n) { return ::send(s, data, n, kSendFlags); }
std::int64_t sys_recv(RawSocket s, std::uint8_t* data, std::size_t n) { return ::recv(s, data, n, 0); }
int sys_poll(PollFd* fds, std::size_t n, int timeout_ms) {
    return ::poll(fds, static_cast<nfds_t>(n), timeout_ms);
}
std::string error_text(int err) { return std::strerror(err); }
#endif

RawSocket raw(Socket::Handle h) { return static_cast<RawSocket>(h); }
Socket::Handle wrap(RawSocket s) { return s == kRawInvalid ? Socket::kInvalid : static_cast<Socket::Handle>(s); }

void set_no_delay(RawSocket s) {
    // Mensajes pequeños y frecuentes: sin el algoritmo de Nagle cada turno sale ya.
    const int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), static_cast<socklen_t>(sizeof(one)));
}

}  // namespace

Socket& Socket::operator=(Socket&& o) noexcept {
    if (this != &o) {
        close();
        h_ = o.h_;
        o.h_ = kInvalid;
    }
    return *this;
}

void Socket::close() noexcept {
    if (valid()) {
        close_raw(raw(h_));
        h_ = kInvalid;
    }
}

Connection::Connection(Socket socket, std::uint32_t max_frame_bytes)
    : socket_(std::move(socket)), max_frame_(max_frame_bytes) {}

std::expected<Connection, std::string> Connection::start_connect(std::string_view host, std::uint16_t port,
                                                                 std::uint32_t max_frame_bytes) {
    if (!ensure_init()) {
        return std::unexpected("no se pudo iniciar la red del sistema");
    }
    addrinfo hints{};
    hints.ai_family = AF_INET;  // el anfitrión escucha en IPv4
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* list = nullptr;
    const std::string host_str(host);
    const std::string port_str = std::to_string(port);
    if (getaddrinfo(host_str.c_str(), port_str.c_str(), &hints, &list) != 0 || list == nullptr) {
        return std::unexpected(std::format("no se encuentra el anfitrión '{}'", host));
    }
    Socket sock(wrap(socket(list->ai_family, list->ai_socktype, list->ai_protocol)));
    std::string error;
    if (!sock.valid() || !set_nonblocking(raw(sock.handle()))) {
        error = error_text(last_error());
    } else {
#ifdef _WIN32
        const int addr_len = static_cast<int>(list->ai_addrlen);
#else
        const socklen_t addr_len = list->ai_addrlen;
#endif
        if (connect(raw(sock.handle()), list->ai_addr, addr_len) != 0 && !in_progress(last_error())) {
            error = error_text(last_error());
        }
    }
    freeaddrinfo(list);
    if (!error.empty()) {
        return std::unexpected(std::format("no se pudo conectar con {}:{}: {}", host, port, error));
    }
    Connection c(std::move(sock), max_frame_bytes);
    c.connecting_ = true;
    return c;
}

bool Connection::finish_connect() {
    PollFd fd{};
    fd.fd = raw(socket_.handle());
    fd.events = POLLOUT;
    if (sys_poll(&fd, 1, 0) <= 0) {
        return false;  // aún no
    }
    int so_error = 0;
    auto len = static_cast<socklen_t>(sizeof(so_error));
    getsockopt(raw(socket_.handle()), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error), &len);
    if (so_error != 0) {
        close(error_text(so_error));
        return false;
    }
    // Windows señala el fallo de connect con POLLERR/POLLHUP y SO_ERROR a 0 en algunos casos.
    if ((fd.revents & (POLLERR | POLLHUP)) != 0) {
        close("conexión rechazada");
        return false;
    }
    set_no_delay(raw(socket_.handle()));
    connecting_ = false;
    return true;
}

void Connection::send(std::span<const std::uint8_t> frame) {
    if (!open()) {
        return;
    }
    // Lo ya enviado se descarta antes de crecer.
    if (sent_ > 0 && sent_ == out_.size()) {
        out_.clear();
        sent_ = 0;
    }
    const auto n = static_cast<std::uint32_t>(frame.size());
    for (unsigned i = 0; i < kFrameHeader; ++i) {
        out_.push_back(static_cast<std::uint8_t>((n >> (kByteBits * i)) & kByteMask));
    }
    out_.insert(out_.end(), frame.begin(), frame.end());
}

bool Connection::pump() {
    if (!open()) {
        return false;
    }
    if (connecting_ && !finish_connect()) {
        return open();
    }
    while (sent_ < out_.size()) {
        const std::int64_t n = sys_send(raw(socket_.handle()), out_.data() + sent_, out_.size() - sent_);
        if (n < 0) {
            const int err = last_error();
            if (would_block(err)) {
                break;
            }
            close("envío: " + error_text(err));
            return false;
        }
        sent_ += static_cast<std::size_t>(n);
    }
    if (sent_ == out_.size()) {
        out_.clear();
        sent_ = 0;
    }
    std::uint8_t buf[kReadChunk];
    for (;;) {
        const std::int64_t n = sys_recv(raw(socket_.handle()), buf, sizeof(buf));
        if (n == 0) {
            close("el otro extremo cerró la conexión");
            return false;
        }
        if (n < 0) {
            const int err = last_error();
            if (would_block(err)) {
                break;
            }
            close("recepción: " + error_text(err));
            return false;
        }
        in_.insert(in_.end(), buf, buf + n);
    }
    return true;
}

std::optional<std::vector<std::uint8_t>> Connection::receive() {
    if (in_.size() < kFrameHeader) {
        return std::nullopt;
    }
    std::uint32_t n = 0;
    for (unsigned i = 0; i < kFrameHeader; ++i) {
        n |= static_cast<std::uint32_t>(in_[i]) << (kByteBits * i);
    }
    if (n > max_frame_) {
        close(std::format("trama de {} bytes, más que el máximo ({})", n, max_frame_));
        in_.clear();
        return std::nullopt;
    }
    if (in_.size() < kFrameHeader + n) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> frame(in_.begin() + kFrameHeader, in_.begin() + kFrameHeader + n);
    in_.erase(in_.begin(), in_.begin() + kFrameHeader + n);
    return frame;
}

void Connection::close(std::string reason) {
    if (open()) {
        error_ = std::move(reason);
        socket_.close();
    }
}

std::expected<Listener, std::string> Listener::listen(std::uint16_t port) {
    if (!ensure_init()) {
        return std::unexpected("no se pudo iniciar la red del sistema");
    }
    Listener l;
    l.socket_ = Socket(wrap(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)));
    if (!l.socket_.valid()) {
        return std::unexpected("no se pudo crear el socket: " + error_text(last_error()));
    }
    const RawSocket s = raw(l.socket_.handle());
#ifndef _WIN32
    // Reabrir enseguida el mismo puerto tras cerrar una partida.
    const int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, static_cast<socklen_t>(sizeof(one)));
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = 0;  // INADDR_ANY: todas las interfaces
    addr.sin_port = htons(port);
    if (bind(s, reinterpret_cast<const sockaddr*>(&addr), static_cast<socklen_t>(sizeof(addr))) != 0) {
        return std::unexpected(std::format("no se pudo usar el puerto {}: {}", port, error_text(last_error())));
    }
    if (::listen(s, SOMAXCONN) != 0 || !set_nonblocking(s)) {
        return std::unexpected("no se pudo escuchar: " + error_text(last_error()));
    }
    auto len = static_cast<socklen_t>(sizeof(addr));
    getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len);
    l.port_ = ntohs(addr.sin_port);
    return l;
}

std::optional<Socket> Listener::accept() {
    Socket s(wrap(::accept(raw(socket_.handle()), nullptr, nullptr)));
    if (!s.valid()) {
        return std::nullopt;
    }
    if (!set_nonblocking(raw(s.handle()))) {
        return std::nullopt;
    }
    set_no_delay(raw(s.handle()));
    return s;
}

void wait(std::span<const Socket::Handle> read, std::span<const Socket::Handle> write, std::int32_t timeout_ms) {
    std::vector<PollFd> fds;
    fds.reserve(read.size() + write.size());
    for (const Socket::Handle h : read) {
        if (h != Socket::kInvalid) {
            PollFd fd{};
            fd.fd = raw(h);
            fd.events = POLLIN;
            fds.push_back(fd);
        }
    }
    for (const Socket::Handle h : write) {
        if (h != Socket::kInvalid) {
            PollFd fd{};
            fd.fd = raw(h);
            fd.events = POLLOUT;
            fds.push_back(fd);
        }
    }
    if (fds.empty()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
        return;
    }
    sys_poll(fds.data(), fds.size(), timeout_ms);
}

}  // namespace rts::net
