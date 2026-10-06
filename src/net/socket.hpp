#pragma once

// Sockets TCP propios, sin dependencias (POSIX o Winsock). Todo es no bloqueante: el
// bucle del juego llama a pump() y espera como mucho lo que pida a wait(). Los mensajes
// viajan en tramas: u32 longitud en little-endian + bytes. Esta capa no sabe nada de la
// simulación; el protocolo de turnos está en game/lockstep.

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rts::net {

// Descriptor del sistema; el tipo concreto (int o SOCKET) queda en socket.cpp.
class Socket {
public:
    using Handle = std::int64_t;
    static constexpr Handle kInvalid = -1;

    Socket() = default;
    explicit Socket(Handle h) noexcept : h_(h) {}
    Socket(Socket&& o) noexcept : h_(o.h_) { o.h_ = kInvalid; }
    Socket& operator=(Socket&& o) noexcept;
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    ~Socket() { close(); }

    void close() noexcept;
    [[nodiscard]] Handle handle() const noexcept { return h_; }
    [[nodiscard]] bool valid() const noexcept { return h_ != kInvalid; }

private:
    Handle h_ = kInvalid;
};

// Conexión con tramas. send() solo encola; pump() envía lo que acepte el sistema y lee
// lo que haya llegado. Un error o el cierre del otro lado la dejan cerrada con el motivo.
class Connection {
public:
    Connection(Socket socket, std::uint32_t max_frame_bytes);

    // Empieza a conectar sin esperar: pump() termina la conexión cuando el otro acepta
    // (connecting() deja de ser cierto) o la cierra con el motivo si la rechaza. Lo
    // que se envíe antes sale al conectar.
    static std::expected<Connection, std::string> start_connect(std::string_view host, std::uint16_t port,
                                                                std::uint32_t max_frame_bytes);

    void send(std::span<const std::uint8_t> frame);
    // Falso si la conexión está cerrada (por error, por el otro lado o por close()).
    bool pump();
    // Siguiente trama completa recibida, si la hay.
    std::optional<std::vector<std::uint8_t>> receive();
    void close(std::string reason);

    [[nodiscard]] bool open() const noexcept { return socket_.valid(); }
    [[nodiscard]] bool sending() const noexcept { return sent_ < out_.size(); }
    [[nodiscard]] bool connecting() const noexcept { return connecting_; }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }
    [[nodiscard]] Socket::Handle handle() const noexcept { return socket_.handle(); }

private:
    bool finish_connect();

    Socket socket_;
    std::uint32_t max_frame_;
    bool connecting_ = false;
    std::vector<std::uint8_t> in_;
    std::vector<std::uint8_t> out_;
    std::size_t sent_ = 0;
    std::string error_;
};

class Listener {
public:
    // Escucha en todas las interfaces. Con port 0, el sistema elige uno libre (pruebas).
    static std::expected<Listener, std::string> listen(std::uint16_t port);

    // Una conexión pendiente, si la hay (no espera).
    std::optional<Socket> accept();
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
    [[nodiscard]] Socket::Handle handle() const noexcept { return socket_.handle(); }

private:
    Socket socket_;
    std::uint16_t port_ = 0;
};

// Espera hasta timeout_ms a que alguno de estos descriptores tenga algo que leer (o que
// escribir, si want_write). Vuelve antes si pasa algo.
void wait(std::span<const Socket::Handle> read, std::span<const Socket::Handle> write, std::int32_t timeout_ms);

}  // namespace rts::net
