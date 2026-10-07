#pragma once

// Informe de errores (G3). Ante una caída (señal de violación de memoria, abort, una
// excepción que nadie captura) o una desincronización, se escribe una carpeta
// informes/informe-<fecha>/ junto al ejecutable con:
//
//   partida.rtsrep  la repetición hasta ese momento (se reproduce con --replay o
//                   --verify-replay: el fallo se puede repetir en otra máquina)
//   rts.log         el registro
//   informe.txt     qué pasó, en qué tick y con qué versión
//
// Ante una señal no todo es seguro de hacer; es un último intento, y después el
// programa termina como lo habría hecho.

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace rts::sim {
class World;
}

namespace rts::game {

class ReplayRecorder;

inline constexpr std::string_view kReportDir = "informes";

// La partida en curso, para el informe: mientras vive, un informe incluye su repetición.
class CrashGuard {
public:
    CrashGuard(const ReplayRecorder& recorder, const sim::World& world) noexcept;
    ~CrashGuard();
    CrashGuard(const CrashGuard&) = delete;
    CrashGuard& operator=(const CrashGuard&) = delete;
    CrashGuard(CrashGuard&&) = delete;
    CrashGuard& operator=(CrashGuard&&) = delete;
};

// Señales y std::terminate escriben el informe en report_root; log es el registro a
// copiar. Se llama una vez al arrancar.
void install_crash_handlers(const std::filesystem::path& report_root, const std::filesystem::path& log);

// Escribe el informe ahora (también para fallos que no tumban el programa, como una
// desincronización). Devuelve la carpeta, o nada si no se pudo.
std::optional<std::filesystem::path> write_crash_report(std::string_view reason);

}  // namespace rts::game
