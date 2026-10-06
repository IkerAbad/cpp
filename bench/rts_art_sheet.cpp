// Hoja de arte (F1): pinta todo el arte de data/art.toml en una sola imagen para verlo
// de un vistazo, sin abrir el juego. Escribe un PAM (Netpbm, RGBA sin comprimir) que
// cualquier visor o `convert hoja.pam hoja.png` abre.
//
//   rts_art_sheet --data data --out hoja.pam

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "game/config.hpp"
#include "render/art.hpp"

namespace {

using rts::render::Image;

constexpr std::int32_t kGap = 6;
constexpr std::int32_t kSheetWidth = 1400;
// Fondo verde hierba (como la pradera) y color de jugador de muestra (el azul del 0).
constexpr std::array<std::uint8_t, 3> kBackground{86, 128, 66};
constexpr std::array<std::uint8_t, 3> kTeam{70, 130, 235};

class Sheet {
public:
    Sheet() : rgba_(static_cast<std::size_t>(kSheetWidth) * 4, 0) {}

    // Pinta img (y su capa del jugador teñida) en la posición actual; avanza.
    void put(const Image& img, const Image* team = nullptr, std::array<std::uint8_t, 3> tint = {255, 255, 255}) {
        if (img.empty()) {
            return;
        }
        if (x_ + img.width > kSheetWidth) {
            new_row();
        }
        grow(y_ + img.height);
        blit(img, x_, y_, tint);
        if (team != nullptr && !team->empty()) {
            blit(*team, x_, y_, kTeam);
        }
        x_ += img.width + kGap;
        row_h_ = std::max(row_h_, img.height);
    }
    void new_row() {
        x_ = kGap;
        y_ += row_h_ + kGap;
        row_h_ = 0;
    }
    bool write(const std::string& path) {
        grow(y_ + row_h_ + kGap);
        std::ofstream out(path, std::ios::binary);
        out << "P7\nWIDTH " << kSheetWidth << "\nHEIGHT " << height_ << "\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n";
        out.write(reinterpret_cast<const char*>(rgba_.data()), static_cast<std::streamsize>(rgba_.size()));
        return static_cast<bool>(out);
    }

private:
    void grow(std::int32_t h) {
        if (h <= height_) {
            return;
        }
        const std::size_t old = rgba_.size();
        rgba_.resize(static_cast<std::size_t>(kSheetWidth) * static_cast<std::size_t>(h) * 4);
        for (std::size_t i = old; i < rgba_.size(); i += 4) {
            rgba_[i] = kBackground[0];
            rgba_[i + 1] = kBackground[1];
            rgba_[i + 2] = kBackground[2];
            rgba_[i + 3] = 255;
        }
        height_ = h;
    }
    void blit(const Image& img, std::int32_t ox, std::int32_t oy, std::array<std::uint8_t, 3> tint) {
        for (std::int32_t y = 0; y < img.height; ++y) {
            for (std::int32_t x = 0; x < img.width; ++x) {
                const auto px = img.at(x, y);
                const float a = static_cast<float>(px[3]) / 255.0f;
                const std::size_t i = (static_cast<std::size_t>(oy + y) * kSheetWidth + static_cast<std::size_t>(ox + x)) * 4;
                for (std::size_t c = 0; c < 3; ++c) {
                    const float src = static_cast<float>(px[c]) * static_cast<float>(tint[c]) / 255.0f;
                    rgba_[i + c] = static_cast<std::uint8_t>(src * a + static_cast<float>(rgba_[i + c]) * (1.0f - a));
                }
            }
        }
    }

    std::vector<std::uint8_t> rgba_;
    std::int32_t height_ = 1;
    std::int32_t x_ = kGap;
    std::int32_t y_ = kGap;
    std::int32_t row_h_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
    std::string data_dir = "data";
    std::string out = "hoja.pam";
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string_view arg = argv[i];
        if (arg == "--data") {
            data_dir = argv[i + 1];
        } else if (arg == "--out") {
            out = argv[i + 1];
        }
    }
    const auto data = rts::game::load_game_data(data_dir);
    if (!data) {
        std::fprintf(stderr, "%s\n", data.error().c_str());
        return 1;
    }
    const rts::render::ArtSpec& art = data->art;
    const rts::render::ViewParams& view = data->engine.view;
    Sheet sheet;
    for (const auto& u : art.units) {
        for (std::size_t p = 0; p < rts::render::kPoseCount; ++p) {
            const auto s = rts::render::paint_unit(u, art.style, static_cast<rts::render::Pose>(p));
            sheet.put(s.body, &s.team);
        }
        sheet.put({});
    }
    sheet.new_row();
    for (const auto& b : art.buildings) {
        const auto s = rts::render::paint_building(b, art.style, view);
        sheet.put(s.body, &s.team);
    }
    sheet.new_row();
    for (const auto& n : art.nodes) {
        for (std::int32_t v = 0; v < n.variants; ++v) {
            sheet.put(rts::render::paint_node(n, art.style, view, v));
        }
    }
    sheet.new_row();
    for (std::size_t t = 0; t < art.terrain.size(); ++t) {
        for (std::int32_t v = 0; v < art.terrain_variants; ++v) {
            sheet.put(rts::render::paint_terrain(art.terrain[t], view, v), nullptr, data->terrain.types[t].color);
        }
    }
    sheet.new_row();
    for (std::int32_t f = 0; f < art.flame_frames; ++f) {
        sheet.put(rts::render::paint_flame(art.effects, f, art.flame_frames));
    }
    sheet.put(rts::render::paint_smoke(art.effects), nullptr, art.effects.smoke);
    sheet.put(rts::render::paint_ground_ring(view.tile_width_px / 2));
    if (!sheet.write(out)) {
        std::fprintf(stderr, "no se pudo escribir %s\n", out.c_str());
        return 1;
    }
    std::printf("hoja de arte en %s\n", out.c_str());
    return 0;
}
