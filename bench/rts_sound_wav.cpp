// Sonidos en WAV (F2): escribe cada receta y cada pieza de data/sound.toml como un .wav
// (PCM de 16 bits, mono) para oírlos sin abrir el juego.
//
//   rts_sound_wav --data data --out sonidos/

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "audio/synth.hpp"
#include "game/config.hpp"

namespace {

void put_le(std::ofstream& out, std::uint32_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) {
        out.put(static_cast<char>((v >> (8 * i)) & 0xFFU));
    }
}

bool write_wav(const std::filesystem::path& path, const std::vector<float>& samples, std::int32_t rate) {
    std::ofstream out(path, std::ios::binary);
    constexpr std::uint32_t kBytesPerSample = 2;
    constexpr float kMax16 = 32767.0f;
    const auto data_bytes = static_cast<std::uint32_t>(samples.size()) * kBytesPerSample;
    out.write("RIFF", 4);
    put_le(out, 36 + data_bytes, 4);
    out.write("WAVEfmt ", 8);
    put_le(out, 16, 4);  // tamaño del bloque fmt
    put_le(out, 1, 2);   // PCM
    put_le(out, 1, 2);   // mono
    put_le(out, static_cast<std::uint32_t>(rate), 4);
    put_le(out, static_cast<std::uint32_t>(rate) * kBytesPerSample, 4);
    put_le(out, kBytesPerSample, 2);
    put_le(out, 16, 2);  // bits por muestra
    out.write("data", 4);
    put_le(out, data_bytes, 4);
    for (const float x : samples) {
        const auto v = static_cast<std::int16_t>(std::lround(std::clamp(x, -1.0f, 1.0f) * kMax16));
        put_le(out, static_cast<std::uint16_t>(v), 2);
    }
    return static_cast<bool>(out);
}

}  // namespace

int main(int argc, char** argv) {
    std::string data_dir = "data";
    std::filesystem::path out_dir = "sonidos";
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string_view arg = argv[i];
        if (arg == "--data") {
            data_dir = argv[i + 1];
        } else if (arg == "--out") {
            out_dir = argv[i + 1];
        }
    }
    const auto data = rts::game::load_game_data(data_dir);
    if (!data) {
        std::fprintf(stderr, "%s\n", data.error().c_str());
        return 1;
    }
    const rts::audio::SoundSpec& spec = data->sound;
    std::filesystem::create_directories(out_dir);
    for (std::size_t r = 0; r < spec.recipes.size(); ++r) {
        const auto& rec = spec.recipes[r];
        const auto s = rts::audio::render(rec, spec.config.sample_rate, 0, static_cast<std::uint32_t>(r + 1));
        if (!write_wav(out_dir / (rec.name + ".wav"), s, spec.config.sample_rate)) {
            return 1;
        }
    }
    for (const auto& m : spec.music) {
        if (!write_wav(out_dir / ("musica_" + m.name + ".wav"), rts::audio::compose(m, spec.config.sample_rate),
                       spec.config.sample_rate)) {
            return 1;
        }
    }
    std::printf("%zu sonidos y %zu piezas en %s\n", spec.recipes.size(), spec.music.size(), out_dir.string().c_str());
    return 0;
}
