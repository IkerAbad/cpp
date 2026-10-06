#include "render/art.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <span>
#include <tuple>
#include <utility>

namespace rts::render {

namespace {

constexpr float kByteMaxF = 255.0f;
constexpr float kPercentF = 100.0f;
constexpr int kPolySamples = 4;  // submuestreo por eje de los polígonos (4x4)

struct Color {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;
};

Color rgb(const Rgb& c, float alpha = 1.0f) {
    return {static_cast<float>(c[0]) / kByteMaxF, static_cast<float>(c[1]) / kByteMaxF,
            static_cast<float>(c[2]) / kByteMaxF, alpha};
}

Color shade(Color c, float k) {
    return {std::min(c.r * k, 1.0f), std::min(c.g * k, 1.0f), std::min(c.b * k, 1.0f), c.a};
}

Color gray(float v, float alpha = 1.0f) { return {v, v, v, alpha}; }

float percent(std::int32_t p) { return static_cast<float>(p) / kPercentF; }

// Ruido de valor determinista en [0, 1): mismo dibujo en todas las máquinas.
float hash01(std::int32_t x, std::int32_t y, std::uint32_t seed) {
    auto h = static_cast<std::uint32_t>(x) * 0x8da6b343U ^ static_cast<std::uint32_t>(y) * 0xd8163841U ^
             seed * 0xcb1ab31fU;
    h ^= h >> 13U;
    h *= 0x5bd1e995U;
    h ^= h >> 15U;
    constexpr float kInv = 1.0f / 4294967296.0f;
    return static_cast<float>(h) * kInv;
}

float smooth_noise(float x, float y, std::uint32_t seed) {
    const auto xi = static_cast<std::int32_t>(std::floor(x));
    const auto yi = static_cast<std::int32_t>(std::floor(y));
    const float fx = x - static_cast<float>(xi);
    const float fy = y - static_cast<float>(yi);
    const float sx = fx * fx * (3.0f - 2.0f * fx);
    const float sy = fy * fy * (3.0f - 2.0f * fy);
    const float a = hash01(xi, yi, seed);
    const float b = hash01(xi + 1, yi, seed);
    const float c = hash01(xi, yi + 1, seed);
    const float d = hash01(xi + 1, yi + 1, seed);
    return (a + (b - a) * sx) + ((c + (d - c) * sx) - (a + (b - a) * sx)) * sy;
}

float length(Vec2 v) { return std::hypot(v.x, v.y); }

float segment_distance(Vec2 p, Vec2 a, Vec2 b) {
    const Vec2 ab = b - a;
    const Vec2 ap = p - a;
    const float len2 = ab.x * ab.x + ab.y * ab.y;
    const float t = len2 > 0.0f ? std::clamp((ap.x * ab.x + ap.y * ab.y) / len2, 0.0f, 1.0f) : 0.0f;
    return length(p - (a + ab * t));
}

bool inside_polygon(std::span<const Vec2> pts, float x, float y) {
    bool in = false;
    for (std::size_t i = 0, j = pts.size() - 1; i < pts.size(); j = i++) {
        const Vec2 a = pts[i];
        const Vec2 b = pts[j];
        if ((a.y > y) != (b.y > y) && x < (b.x - a.x) * (y - a.y) / (b.y - a.y) + a.x) {
            in = !in;
        }
    }
    return in;
}

// Lienzo de flotantes con alfa recto.
class Canvas {
public:
    Canvas(std::int32_t w, std::int32_t h)
        : w_(w), h_(h), px_(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), Color{0.0f, 0.0f, 0.0f, 0.0f}) {}

    [[nodiscard]] std::int32_t width() const noexcept { return w_; }
    [[nodiscard]] std::int32_t height() const noexcept { return h_; }
    [[nodiscard]] Color& at(std::int32_t x, std::int32_t y) {
        return px_[static_cast<std::size_t>(y) * static_cast<std::size_t>(w_) + static_cast<std::size_t>(x)];
    }
    [[nodiscard]] const Color& at(std::int32_t x, std::int32_t y) const {
        return px_[static_cast<std::size_t>(y) * static_cast<std::size_t>(w_) + static_cast<std::size_t>(x)];
    }

    // "Sobre": el color con alfa c.a * cov encima de lo que hay.
    void blend(std::int32_t x, std::int32_t y, Color c, float cov) {
        const float a = c.a * cov;
        if (a <= 0.0f) {
            return;
        }
        Color& d = at(x, y);
        const float out_a = a + d.a * (1.0f - a);
        if (out_a <= 0.0f) {
            return;
        }
        d.r = (c.r * a + d.r * d.a * (1.0f - a)) / out_a;
        d.g = (c.g * a + d.g * d.a * (1.0f - a)) / out_a;
        d.b = (c.b * a + d.b * d.a * (1.0f - a)) / out_a;
        d.a = out_a;
    }
    void erase(std::int32_t x, std::int32_t y, float cov) { at(x, y).a *= 1.0f - std::clamp(cov, 0.0f, 1.0f); }

    // Esta capa debajo de top (que queda encima).
    void put_under(const Canvas& top) {
        for (std::int32_t y = 0; y < h_; ++y) {
            for (std::int32_t x = 0; x < w_; ++x) {
                const Color& t = top.at(x, y);
                blend(x, y, {t.r, t.g, t.b, 1.0f}, t.a);
            }
        }
    }

    // Contorno de un píxel alrededor de lo opaco: lee mejor sobre cualquier terreno.
    void outline(Color c) {
        constexpr float kSolid = 0.5f;
        std::vector<std::uint8_t> edge(px_.size(), 0);
        for (std::int32_t y = 0; y < h_; ++y) {
            for (std::int32_t x = 0; x < w_; ++x) {
                if (at(x, y).a >= kSolid) {
                    continue;
                }
                const bool near = (x > 0 && at(x - 1, y).a >= kSolid) || (x + 1 < w_ && at(x + 1, y).a >= kSolid) ||
                                  (y > 0 && at(x, y - 1).a >= kSolid) || (y + 1 < h_ && at(x, y + 1).a >= kSolid);
                edge[static_cast<std::size_t>(y) * static_cast<std::size_t>(w_) + static_cast<std::size_t>(x)] =
                    near ? 1 : 0;
            }
        }
        for (std::int32_t y = 0; y < h_; ++y) {
            for (std::int32_t x = 0; x < w_; ++x) {
                if (edge[static_cast<std::size_t>(y) * static_cast<std::size_t>(w_) + static_cast<std::size_t>(x)] != 0) {
                    Color& d = at(x, y);
                    const float a = std::max(d.a, c.a);
                    d = {c.r, c.g, c.b, a};
                }
            }
        }
    }

    [[nodiscard]] Image to_image(Vec2 anchor) const {
        Image img;
        img.width = w_;
        img.height = h_;
        img.anchor = anchor;
        img.rgba.resize(px_.size() * 4);
        for (std::size_t i = 0; i < px_.size(); ++i) {
            const Color& c = px_[i];
            img.rgba[i * 4 + 0] = to_byte(c.r);
            img.rgba[i * 4 + 1] = to_byte(c.g);
            img.rgba[i * 4 + 2] = to_byte(c.b);
            img.rgba[i * 4 + 3] = to_byte(c.a);
        }
        return img;
    }

private:
    static std::uint8_t to_byte(float v) {
        return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * kByteMaxF));
    }

    std::int32_t w_;
    std::int32_t h_;
    std::vector<Color> px_;
};

// Qué hace una forma con cada capa.
enum class Layer : std::uint8_t {
    Body,    // pinta el cuerpo y borra la capa del jugador
    Team,    // pinta el cuerpo (con el color sin dueño) y la capa del jugador (en grises)
    Shadow,  // solo la sombra, debajo de todo
};

// Pinta en las dos capas (y la de sombra) con coordenadas locales: origen en el ancla,
// escaladas por scale; +x a la derecha, +y hacia abajo.
class Painter {
public:
    Painter(std::int32_t w, std::int32_t h, Vec2 anchor, float scale)
        : body_(w, h), team_(w, h), shadow_(w, h), anchor_(anchor), scale_(scale) {}

    [[nodiscard]] Vec2 to_px(Vec2 p) const { return anchor_ + p * scale_; }
    [[nodiscard]] float scale() const noexcept { return scale_; }

    // Una cápsula (segmento con radio): miembros, armas, astiles, postes.
    void capsule(Vec2 a, Vec2 b, float r, Color c, Layer layer = Layer::Body, float team_shade = 1.0f) {
        const Vec2 pa = to_px(a);
        const Vec2 pb = to_px(b);
        const float pr = r * scale_;
        shape(std::min(pa.x, pb.x) - pr, std::min(pa.y, pb.y) - pr, std::max(pa.x, pb.x) + pr, std::max(pa.y, pb.y) + pr,
              [&](float x, float y) { return std::clamp(pr - segment_distance({x, y}, pa, pb) + 0.5f, 0.0f, 1.0f); }, c,
              layer, team_shade);
    }
    void circle(Vec2 c0, float r, Color c, Layer layer = Layer::Body, float team_shade = 1.0f) {
        capsule(c0, c0, r, c, layer, team_shade);
    }
    void ellipse(Vec2 c0, Vec2 r, Color c, Layer layer = Layer::Body, float team_shade = 1.0f) {
        const Vec2 pc = to_px(c0);
        const Vec2 pr = r * scale_;
        shape(pc.x - pr.x, pc.y - pr.y, pc.x + pr.x, pc.y + pr.y,
              [&](float x, float y) {
                  const float qx = (x - pc.x) / pr.x;
                  const float qy = (y - pc.y) / pr.y;
                  const float d = (std::hypot(qx, qy) - 1.0f) * std::min(pr.x, pr.y);
                  return std::clamp(0.5f - d, 0.0f, 1.0f);
              },
              c, layer, team_shade);
    }
    // Polígono cualquiera, suavizado por submuestreo.
    void polygon(std::span<const Vec2> local, Color c, Layer layer = Layer::Body, float team_shade = 1.0f) {
        std::vector<Vec2> pts;
        pts.reserve(local.size());
        for (const Vec2& p : local) {
            pts.push_back(to_px(p));
        }
        polygon_px(pts, [c](float, float) { return c; }, layer, team_shade);
    }
    void polygon(std::initializer_list<Vec2> local, Color c, Layer layer = Layer::Body, float team_shade = 1.0f) {
        polygon(std::span<const Vec2>(local.begin(), local.size()), c, layer, team_shade);
    }

    // Polígono en píxeles con color por píxel (texturas de muros y tejados).
    template <typename Shader>
    void polygon_px(const std::vector<Vec2>& pts, Shader&& color_at, Layer layer = Layer::Body, float team_shade = 1.0f) {
        float x0 = pts[0].x;
        float y0 = pts[0].y;
        float x1 = x0;
        float y1 = y0;
        for (const Vec2& p : pts) {
            x0 = std::min(x0, p.x);
            y0 = std::min(y0, p.y);
            x1 = std::max(x1, p.x);
            y1 = std::max(y1, p.y);
        }
        constexpr float kStep = 1.0f / static_cast<float>(kPolySamples);
        constexpr float kSamples = static_cast<float>(kPolySamples * kPolySamples);
        for_pixels(x0, y0, x1, y1, [&](std::int32_t x, std::int32_t y) {
            int hits = 0;
            for (int sy = 0; sy < kPolySamples; ++sy) {
                for (int sx = 0; sx < kPolySamples; ++sx) {
                    hits += inside_polygon(pts, static_cast<float>(x) + (static_cast<float>(sx) + 0.5f) * kStep,
                                           static_cast<float>(y) + (static_cast<float>(sy) + 0.5f) * kStep)
                                ? 1
                                : 0;
                }
            }
            if (hits > 0) {
                const float cx = static_cast<float>(x) + 0.5f;
                const float cy = static_cast<float>(y) + 0.5f;
                apply(x, y, color_at(cx, cy), static_cast<float>(hits) / kSamples, layer, team_shade);
            }
        });
    }

    // Sombra elíptica en el suelo, bajo todo lo demás.
    void ground_shadow(Vec2 c0, Vec2 r, float alpha) { ellipse(c0, r, {0.0f, 0.0f, 0.0f, alpha}, Layer::Shadow); }

    // Resultado: contorno al cuerpo, sombra debajo y recorte a lo que tiene algo.
    [[nodiscard]] SpriteImages finish(const ArtStyle& style) {
        body_.outline(rgb(style.outline, percent(style.outline_alpha_percent)));
        shadow_.put_under(body_);
        // Recorte común a las dos capas (el ancla se desplaza igual).
        std::int32_t x0 = shadow_.width();
        std::int32_t y0 = shadow_.height();
        std::int32_t x1 = -1;
        std::int32_t y1 = -1;
        for (std::int32_t y = 0; y < shadow_.height(); ++y) {
            for (std::int32_t x = 0; x < shadow_.width(); ++x) {
                if (shadow_.at(x, y).a > 0.0f || team_.at(x, y).a > 0.0f) {
                    x0 = std::min(x0, x);
                    y0 = std::min(y0, y);
                    x1 = std::max(x1, x);
                    y1 = std::max(y1, y);
                }
            }
        }
        SpriteImages out;
        if (x1 < x0) {
            return out;
        }
        const auto crop = [&](const Canvas& src) {
            Canvas c(x1 - x0 + 1, y1 - y0 + 1);
            for (std::int32_t y = y0; y <= y1; ++y) {
                for (std::int32_t x = x0; x <= x1; ++x) {
                    c.at(x - x0, y - y0) = src.at(x, y);
                }
            }
            return c.to_image(anchor_ - Vec2{static_cast<float>(x0), static_cast<float>(y0)});
        };
        out.body = crop(shadow_);
        bool any_team = false;
        for (std::int32_t y = y0; y <= y1 && !any_team; ++y) {
            for (std::int32_t x = x0; x <= x1 && !any_team; ++x) {
                any_team = team_.at(x, y).a > 0.0f;
            }
        }
        if (any_team) {
            out.team = crop(team_);
        }
        return out;
    }

private:
    template <typename Fn>
    void for_pixels(float x0, float y0, float x1, float y1, Fn&& fn) {
        const std::int32_t ix0 = std::max(0, static_cast<std::int32_t>(std::floor(x0)) - 1);
        const std::int32_t iy0 = std::max(0, static_cast<std::int32_t>(std::floor(y0)) - 1);
        const std::int32_t ix1 = std::min(body_.width() - 1, static_cast<std::int32_t>(std::ceil(x1)) + 1);
        const std::int32_t iy1 = std::min(body_.height() - 1, static_cast<std::int32_t>(std::ceil(y1)) + 1);
        for (std::int32_t y = iy0; y <= iy1; ++y) {
            for (std::int32_t x = ix0; x <= ix1; ++x) {
                fn(x, y);
            }
        }
    }

    template <typename Coverage>
    void shape(float x0, float y0, float x1, float y1, Coverage&& coverage, Color c, Layer layer, float team_shade) {
        for_pixels(x0, y0, x1, y1, [&](std::int32_t x, std::int32_t y) {
            const float cov = coverage(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
            if (cov > 0.0f) {
                apply(x, y, c, cov, layer, team_shade);
            }
        });
    }

    void apply(std::int32_t x, std::int32_t y, Color c, float cov, Layer layer, float team_shade) {
        switch (layer) {
            case Layer::Body:
                body_.blend(x, y, c, cov);
                team_.erase(x, y, cov * c.a);
                break;
            case Layer::Team:
                body_.blend(x, y, c, cov);
                team_.blend(x, y, gray(team_shade, c.a), cov);
                break;
            case Layer::Shadow:
                shadow_.blend(x, y, c, cov);
                break;
        }
    }

    Canvas body_;
    Canvas team_;
    Canvas shadow_;
    Vec2 anchor_;
    float scale_;
};

// ---------------------------------------------------------------------------------
// Figuras. Unidades locales: una persona de pie mide kPersonUnits de los pies (y = 0)
// a la coronilla; scale = height_px / kPersonUnits.

constexpr float kPersonUnits = 28.0f;
// Proporciones de la figura (en unidades locales).
constexpr float kHipY = -11.0f;
constexpr float kShoulderY = -19.5f;
constexpr float kHeadY = -23.5f;
constexpr float kHeadR = 3.3f;
constexpr float kLimbR = 1.25f;
constexpr float kArmR = 1.1f;
constexpr float kStride = 3.2f;   // paso de las piernas al andar
constexpr float kTorsoHalfTop = 3.6f;
constexpr float kTorsoHalfHip = 3.0f;
constexpr float kRobeHalfHem = 4.6f;
// Sombreado de la tela del jugador: luz en el pecho, sombra en la espalda.
constexpr float kClothLit = 1.0f;
constexpr float kClothShade = 0.72f;
constexpr float kShadowRx = 6.0f;
constexpr float kShadowRy = 2.2f;

struct PersonPose {
    float front_leg = 0.0f;  // avance del pie de delante (x)
    float back_leg = 0.0f;
    Vec2 front_hand{3.5f, -12.5f};
    Vec2 back_hand{-3.0f, -12.0f};
    float weapon_angle = 0.0f;  // radianes, sobre la vertical, hacia delante
    float lift = 0.0f;          // rebote al andar
};

PersonPose person_pose(Pose pose, Weapon weapon) {
    PersonPose p;
    const bool ranged = weapon == Weapon::Bow || weapon == Weapon::Crossbow;
    constexpr float kBob = 0.6f;
    switch (pose) {
        case Pose::Idle:
            break;
        case Pose::StepA:
            p.front_leg = kStride;
            p.back_leg = -kStride;
            p.lift = -kBob;
            p.front_hand = {1.5f, -12.0f};
            p.back_hand = {-1.0f, -12.5f};
            break;
        case Pose::StepB:
            p.front_leg = -kStride;
            p.back_leg = kStride;
            p.lift = -kBob;
            p.front_hand = {4.5f, -13.0f};
            p.back_hand = {-4.0f, -12.0f};
            break;
        case Pose::Act:  // preparar: arco tenso, arma alzada
            if (ranged) {
                p.front_hand = {7.0f, -17.0f};
                p.back_hand = {1.0f, -17.5f};
            } else {
                p.front_hand = {1.5f, -24.0f};
                p.back_hand = {-2.5f, -14.0f};
                p.weapon_angle = -0.6f;
            }
            p.back_leg = -1.5f;
            p.front_leg = 1.5f;
            break;
        case Pose::Strike:  // golpe o suelta
            if (ranged) {
                p.front_hand = {7.5f, -17.0f};
                p.back_hand = {-1.5f, -17.5f};
            } else {
                p.front_hand = {8.0f, -14.0f};
                p.back_hand = {-2.0f, -13.0f};
                p.weapon_angle = 1.3f;
            }
            p.back_leg = -2.5f;
            p.front_leg = 2.5f;
            break;
        case Pose::Count:
            break;
    }
    return p;
}

Vec2 rotated(Vec2 v, float angle) {
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    return {v.x * c - v.y * s, v.x * s + v.y * c};
}

// Arma en la mano h, con el ángulo dado (0 = vertical hacia arriba).
void paint_weapon(Painter& p, const UnitArt& art, Weapon weapon, Vec2 h, float angle, Pose pose) {
    const Color wood = rgb(art.wood);
    const Color metal = rgb(art.metal);
    const Color dark_metal = shade(metal, 0.7f);
    const auto along = [&](float up) { return h + rotated({0.0f, -up}, angle); };
    switch (weapon) {
        case Weapon::None:
        case Weapon::Satchel:
            break;
        case Weapon::Spear:
        case Weapon::Lance: {
            const float len = weapon == Weapon::Lance ? 22.0f : 18.0f;
            const Vec2 butt = along(-6.0f);
            const Vec2 tip = along(len);
            p.capsule(butt, tip, 0.55f, wood);
            p.polygon({tip + rotated({-1.0f, 0.0f}, angle), tip + rotated({0.0f, -3.5f}, angle),
                       tip + rotated({1.0f, 0.0f}, angle)},
                      metal);
            break;
        }
        case Weapon::Sword: {
            p.capsule(along(0.0f), along(8.5f), 0.6f, metal);
            p.capsule(along(0.0f) + rotated({-1.6f, 0.0f}, angle), along(0.0f) + rotated({1.6f, 0.0f}, angle), 0.45f,
                      dark_metal);
            break;
        }
        case Weapon::Axe:
        case Weapon::Hammer:
        case Weapon::Pick: {
            const Vec2 head = along(8.0f);
            p.capsule(along(-1.0f), head, 0.5f, wood);
            if (weapon == Weapon::Axe) {
                p.polygon({head, head + rotated({3.2f, -1.5f}, angle), head + rotated({3.2f, 2.0f}, angle),
                           head + rotated({0.0f, 1.8f}, angle)},
                          metal);
            } else if (weapon == Weapon::Hammer) {
                p.capsule(head + rotated({-1.6f, 0.0f}, angle), head + rotated({2.0f, 0.0f}, angle), 1.1f, dark_metal);
            } else {
                p.capsule(head + rotated({-3.0f, 1.2f}, angle), head + rotated({3.0f, 1.2f}, angle), 0.5f, dark_metal);
            }
            break;
        }
        case Weapon::Bow: {
            // Arco vertical delante de la mano adelantada; cuerda hasta la mano de atrás al tensar.
            const float bend = pose == Pose::Act ? 3.0f : 1.8f;
            const Vec2 top = h + Vec2{-bend * 0.4f, -8.0f};
            const Vec2 mid = h + Vec2{bend * 0.5f, 0.0f};
            const Vec2 bottom = h + Vec2{-bend * 0.4f, 8.0f};
            p.capsule(top, mid, 0.55f, wood);
            p.capsule(mid, bottom, 0.55f, wood);
            break;
        }
        case Weapon::Crossbow: {
            p.capsule(h + Vec2{-6.0f, 0.5f}, h + Vec2{1.5f, 0.0f}, 0.8f, wood);
            p.capsule(h + Vec2{1.0f, -3.5f}, h + Vec2{1.0f, 3.5f}, 0.55f, dark_metal);
            break;
        }
    }
}

void paint_head(Painter& p, const UnitArt& art, Vec2 head) {
    const Color skin = rgb(art.skin);
    p.circle(head, kHeadR, skin);
    const Color metal = rgb(art.metal);
    switch (art.helmet) {
        case Helmet::None:
            p.circle(head + Vec2{-0.8f, -1.6f}, 2.6f, shade(rgb(art.garment), 0.55f));  // pelo
            p.circle(head + Vec2{0.6f, 0.2f}, kHeadR * 0.8f, skin);
            break;
        case Helmet::Hood:
            p.circle(head + Vec2{-0.6f, -0.6f}, kHeadR + 0.7f, rgb(art.garment));
            p.circle(head + Vec2{1.2f, 0.3f}, kHeadR * 0.65f, skin);
            break;
        case Helmet::Kettle:
            p.ellipse(head + Vec2{0.0f, -1.6f}, {4.8f, 1.1f}, shade(metal, 0.85f));
            p.ellipse(head + Vec2{0.0f, -2.4f}, {3.0f, 2.2f}, metal);
            break;
        case Helmet::Great:
            p.polygon({head + Vec2{-3.4f, -3.8f}, head + Vec2{3.4f, -3.8f}, head + Vec2{3.4f, 3.4f},
                       head + Vec2{-3.4f, 3.4f}},
                      metal);
            p.capsule(head + Vec2{0.6f, -0.6f}, head + Vec2{3.2f, -0.6f}, 0.45f, shade(metal, 0.3f));
            break;
    }
}

// Persona mirando a la derecha. offset: dónde están sus pies; sin piernas si va montada.
void paint_person(Painter& p, const UnitArt& art, Pose pose, Vec2 offset, bool mounted) {
    const PersonPose pp = person_pose(pose, art.weapon);
    const Vec2 base = offset + Vec2{0.0f, pp.lift};
    const Color garment = rgb(art.garment);
    const Color metal = rgb(art.metal);
    const Color cloth = rgb(art.cloth);
    const Color skin = rgb(art.skin);
    const bool armored = art.armor != Armor::None;

    // Brazo de atrás (detrás del cuerpo).
    const Vec2 back_shoulder = base + Vec2{-1.6f, kShoulderY + 0.8f};
    p.capsule(back_shoulder, base + pp.back_hand, kArmR, armored ? shade(metal, 0.7f) : shade(garment, 0.7f));
    p.circle(base + pp.back_hand, kArmR * 0.9f, shade(skin, 0.85f));

    if (mounted) {
        // La pierna de este lado, por el costado del caballo hasta el estribo.
        const Color leg = art.armor == Armor::Plate ? metal : garment;
        p.capsule(base + Vec2{0.8f, kHipY}, base + Vec2{2.2f, kHipY + 7.5f}, kLimbR, leg);
    } else {
        const Color leg = art.armor == Armor::Plate ? metal : garment;
        p.capsule(base + Vec2{-0.8f, kHipY}, offset + Vec2{pp.back_leg, -0.8f}, kLimbR, shade(leg, 0.75f));
        p.capsule(base + Vec2{0.8f, kHipY}, offset + Vec2{pp.front_leg, -0.8f}, kLimbR, leg);
        p.capsule(offset + Vec2{pp.back_leg, -0.6f}, offset + Vec2{pp.back_leg + 1.6f, -0.6f}, 0.8f,
                  shade(garment, 0.4f));
        p.capsule(offset + Vec2{pp.front_leg, -0.6f}, offset + Vec2{pp.front_leg + 1.6f, -0.6f}, 0.8f,
                  shade(garment, 0.45f));
    }

    // Tronco: cota bajo la túnica del jugador (que deja ver mangas y faldón).
    const float hem_y = art.robe && !mounted ? -2.0f : kHipY + 2.5f;
    const float hem = art.robe && !mounted ? kRobeHalfHem : kTorsoHalfHip + 0.6f;
    if (armored) {
        p.polygon({base + Vec2{-kTorsoHalfTop - 0.6f, kShoulderY}, base + Vec2{kTorsoHalfTop + 0.6f, kShoulderY},
                   base + Vec2{hem + 0.5f, hem_y + 1.0f}, base + Vec2{-hem - 0.5f, hem_y + 1.0f}},
                  art.armor == Armor::Plate ? metal : shade(metal, 0.8f));
    }
    // Túnica: luz delante, sombra detrás (en la capa del jugador).
    p.polygon({base + Vec2{-kTorsoHalfTop, kShoulderY}, base + Vec2{0.4f, kShoulderY}, base + Vec2{0.4f, hem_y},
               base + Vec2{-hem, hem_y}},
              shade(cloth, kClothShade), Layer::Team, kClothShade);
    p.polygon({base + Vec2{0.3f, kShoulderY}, base + Vec2{kTorsoHalfTop, kShoulderY}, base + Vec2{hem, hem_y},
               base + Vec2{0.3f, hem_y}},
              shade(cloth, kClothLit), Layer::Team, kClothLit);
    // Cinturón.
    p.capsule(base + Vec2{-kTorsoHalfHip, kHipY + 0.5f}, base + Vec2{kTorsoHalfHip, kHipY + 0.5f}, 0.5f,
              shade(garment, 0.35f));
    if (art.weapon == Weapon::Satchel) {
        p.ellipse(base + Vec2{-3.2f, kHipY + 1.5f}, {2.0f, 2.4f}, shade(garment, 0.8f));
    }

    paint_head(p, art, base + Vec2{0.6f, kHeadY});

    // Escudo en el brazo de atrás, delante del cuerpo.
    if (art.shield) {
        const Vec2 c = base + Vec2{-0.5f, -14.0f};
        p.polygon({c + Vec2{-3.6f, -4.2f}, c + Vec2{3.6f, -4.2f}, c + Vec2{3.2f, 1.5f}, c + Vec2{0.0f, 5.5f},
                   c + Vec2{-3.2f, 1.5f}},
                  shade(cloth, 0.9f), Layer::Team, 0.9f);
        p.capsule(c + Vec2{0.0f, -3.6f}, c + Vec2{0.0f, 4.5f}, 0.45f, shade(metal, 0.8f));
    }

    // Arma y brazo de delante, lo último.
    const Vec2 hand = base + pp.front_hand;
    paint_weapon(p, art, art.weapon, hand, pp.weapon_angle, pose);
    const Vec2 front_shoulder = base + Vec2{1.8f, kShoulderY + 0.8f};
    p.capsule(front_shoulder, hand, kArmR, armored ? metal : garment);
    p.circle(hand, kArmR * 0.95f, skin);
}

// Cuadrúpedo mirando a la derecha: caballo (alto) o mula (baja, con fardos).
void paint_beast(Painter& p, const UnitArt& art, Pose pose, bool pack, bool caparison, Vec2 at = {}) {
    const Color hide = rgb(art.beast);
    const Color dark = shade(hide, 0.65f);
    const float h = pack ? 0.8f : 1.0f;  // la mula, más baja
    const float body_y = -11.0f * h;
    const float gait = pose == Pose::StepA ? 1.0f : (pose == Pose::StepB ? -1.0f : 0.0f);
    constexpr float kLegSwing = 2.6f;
    // Patas de atrás (lejos), cuerpo, patas de delante (cerca).
    const auto leg = [&](float hip_x, float swing, Color c) {
        p.capsule(at + Vec2{hip_x, body_y + 2.0f}, at + Vec2{hip_x + swing, -0.8f}, 1.0f, c);
    };
    leg(-5.5f, -gait * kLegSwing, dark);
    leg(5.0f, gait * kLegSwing, dark);
    p.capsule(at + Vec2{-8.5f, body_y - 1.0f}, at + Vec2{-10.5f, body_y + 5.0f}, 0.9f, shade(hide, 0.4f));  // cola
    p.ellipse(at + Vec2{0.0f, body_y}, {8.5f, 4.2f * h}, hide);
    // Cuello y cabeza.
    p.capsule(at + Vec2{5.5f, body_y - 1.0f}, at + Vec2{8.5f, body_y - 7.0f * h}, 2.0f, hide);
    p.capsule(at + Vec2{8.5f, body_y - 7.0f * h}, at + Vec2{11.5f, body_y - 4.5f * h}, 1.5f, hide);
    p.circle(at + Vec2{8.0f, body_y - 8.5f * h}, 0.8f, dark);  // oreja
    leg(-4.0f, gait * kLegSwing, hide);
    leg(6.5f, -gait * kLegSwing, hide);
    if (caparison) {
        p.polygon({at + Vec2{-8.0f, body_y - 3.5f}, at + Vec2{7.0f, body_y - 3.5f}, at + Vec2{7.5f, body_y + 6.0f},
                   at + Vec2{-8.5f, body_y + 6.0f}},
                  shade(rgb(art.cloth), 0.9f), Layer::Team, 0.9f);
    }
    if (pack) {
        // Manta del jugador bajo los fardos.
        p.polygon({at + Vec2{-5.5f, body_y - 3.5f}, at + Vec2{5.0f, body_y - 3.5f}, at + Vec2{5.5f, body_y + 2.5f},
                   at + Vec2{-6.0f, body_y + 2.5f}},
                  shade(rgb(art.cloth), 0.9f), Layer::Team, 0.9f);
        p.ellipse(at + Vec2{-2.0f, body_y - 3.0f}, {3.2f, 3.0f}, shade(rgb(art.garment), 0.9f));
        p.ellipse(at + Vec2{2.5f, body_y - 3.2f}, {3.0f, 2.8f}, rgb(art.garment));
        p.capsule(at + Vec2{-4.0f, body_y - 5.5f}, at + Vec2{4.5f, body_y - 5.5f}, 0.5f, shade(rgb(art.wood), 0.7f));
    }
}

void paint_wheel(Painter& p, const UnitArt& art, Vec2 c, float r) {
    const Color wood = rgb(art.wood);
    p.circle(c, r, shade(wood, 0.55f));
    p.circle(c, r - 1.0f, shade(wood, 0.9f));
    p.circle(c, r * 0.3f, shade(rgb(art.metal), 0.6f));
}

// Carreta con su animal de tiro delante.
void paint_cart(Painter& p, const UnitArt& art, Pose pose) {
    const Color wood = rgb(art.wood);
    paint_beast(p, art, pose, false, false, {14.0f, 0.0f});
    p.capsule({-2.0f, -8.0f}, {8.0f, -9.0f}, 0.6f, shade(wood, 0.6f));  // lanza
    p.polygon({{-16.0f, -16.0f}, {2.0f, -16.0f}, {2.0f, -7.0f}, {-16.0f, -7.0f}}, wood);
    p.polygon({{-16.0f, -16.0f}, {2.0f, -16.0f}, {0.0f, -19.0f}, {-14.0f, -19.0f}}, shade(wood, 1.15f));
    for (int i = 0; i < 4; ++i) {
        const float x = -14.0f + 4.0f * static_cast<float>(i);
        p.capsule({x, -15.5f}, {x, -7.5f}, 0.3f, shade(wood, 0.7f));
    }
    // Carga bajo una lona del jugador.
    p.ellipse({-7.0f, -19.5f}, {7.5f, 3.5f}, shade(rgb(art.cloth), 0.85f), Layer::Team, 0.85f);
    const float turn = pose == Pose::StepA ? 0.4f : (pose == Pose::StepB ? -0.4f : 0.0f);
    paint_wheel(p, art, {-7.0f + turn, -5.0f}, 5.0f);
}

void paint_ram(Painter& p, const UnitArt& art, Pose pose) {
    const Color wood = rgb(art.wood);
    const float push = pose == Pose::Strike ? 4.0f : (pose == Pose::Act ? -2.0f : 0.0f);
    // Tronco con cabeza de hierro bajo el cobertizo.
    p.capsule({-12.0f + push, -10.0f}, {12.0f + push, -10.0f}, 2.2f, shade(wood, 0.8f));
    p.circle({13.0f + push, -10.0f}, 2.6f, rgb(art.metal));
    // Cobertizo: postes y tejado a dos aguas cubierto de pieles (capa del jugador en el borde).
    p.capsule({-11.0f, -4.0f}, {-11.0f, -18.0f}, 0.9f, wood);
    p.capsule({10.0f, -4.0f}, {10.0f, -18.0f}, 0.9f, wood);
    p.polygon({{-14.0f, -17.0f}, {13.0f, -17.0f}, {9.0f, -25.0f}, {-10.0f, -25.0f}}, shade(rgb(art.beast), 0.9f));
    p.polygon({{-14.0f, -17.0f}, {13.0f, -17.0f}, {13.0f, -15.5f}, {-14.0f, -15.5f}}, rgb(art.cloth), Layer::Team);
    const float turn = pose == Pose::StepA ? 0.5f : (pose == Pose::StepB ? -0.5f : 0.0f);
    paint_wheel(p, art, {-8.0f + turn, -3.6f}, 3.6f);
    paint_wheel(p, art, {7.0f + turn, -3.6f}, 3.6f);
}

void paint_trebuchet(Painter& p, const UnitArt& art, Pose pose) {
    const Color wood = rgb(art.wood);
    const Color dark = shade(wood, 0.7f);
    // Bancada, caballetes, eje y brazo (alzado al disparar).
    p.capsule({-14.0f, -2.5f}, {14.0f, -2.5f}, 1.4f, dark);
    p.capsule({-8.0f, -2.5f}, {0.0f, -24.0f}, 1.1f, wood);
    p.capsule({8.0f, -2.5f}, {0.0f, -24.0f}, 1.1f, wood);
    const Vec2 pivot{0.0f, -24.0f};
    // En marcha, el brazo cabecea un poco con los tumbos de la bancada.
    const float wobble = pose == Pose::StepA ? 0.06f : (pose == Pose::StepB ? -0.06f : 0.0f);
    const float angle = pose == Pose::Strike ? -1.1f : (pose == Pose::Act ? 0.9f : 0.55f + wobble);
    const Vec2 arm = rotated({0.0f, -1.0f}, angle);
    const Vec2 long_end = pivot + arm * 20.0f;
    const Vec2 short_end = pivot - arm * 7.0f;
    p.capsule(short_end, long_end, 0.9f, shade(wood, 1.1f));
    p.polygon({short_end + Vec2{-3.0f, 0.0f}, short_end + Vec2{3.0f, 0.0f}, short_end + Vec2{3.0f, 6.0f},
               short_end + Vec2{-3.0f, 6.0f}},
              shade(rgb(art.metal), 0.6f));
    p.circle(pivot, 1.3f, rgb(art.metal));
    // Banderola del jugador en lo alto del caballete.
    p.capsule(pivot, pivot + Vec2{0.0f, -6.0f}, 0.4f, dark);
    p.polygon({pivot + Vec2{0.0f, -6.0f}, pivot + Vec2{5.0f, -5.0f}, pivot + Vec2{0.0f, -3.5f}}, rgb(art.cloth),
              Layer::Team);
}

}  // namespace

std::array<std::uint8_t, 4> Image::at(std::int32_t x, std::int32_t y) const noexcept {
    const std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4;
    return {rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]};
}

std::int32_t Image::opaque_pixels() const noexcept {
    std::int32_t n = 0;
    for (std::size_t i = 3; i < rgba.size(); i += 4) {
        n += rgba[i] > 0 ? 1 : 0;
    }
    return n;
}

SpriteImages paint_unit(const UnitArt& art, const ArtStyle& style, Pose pose) {
    const float scale = static_cast<float>(art.height_px) / kPersonUnits;
    // Lienzo holgado: se recorta al final.
    constexpr float kCanvasUnits = 64.0f;
    const auto size = static_cast<std::int32_t>(std::ceil(kCanvasUnits * scale));
    Painter p(size, size, {static_cast<float>(size) * 0.5f, static_cast<float>(size) * 0.8f}, scale);
    const float shadow = percent(style.shadow_alpha_percent);
    switch (art.figure) {
        case Figure::Person:
            p.ground_shadow({0.0f, 0.0f}, {kShadowRx, kShadowRy}, shadow);
            paint_person(p, art, pose, {0.0f, 0.0f}, false);
            break;
        case Figure::Rider:
            p.ground_shadow({0.0f, 0.0f}, {kShadowRx * 2.0f, kShadowRy * 1.4f}, shadow);
            paint_beast(p, art, pose, false, art.armor == Armor::Plate);
            paint_person(p, art, pose == Pose::StepA || pose == Pose::StepB ? Pose::Idle : pose, {-0.5f, -4.5f}, true);
            break;
        case Figure::PackAnimal:
            p.ground_shadow({0.0f, 0.0f}, {kShadowRx * 1.7f, kShadowRy * 1.3f}, shadow);
            paint_beast(p, art, pose, true, false);
            break;
        case Figure::Cart:
            p.ground_shadow({-3.0f, 0.0f}, {kShadowRx * 2.6f, kShadowRy * 1.6f}, shadow);
            paint_cart(p, art, pose);
            break;
        case Figure::Ram:
            p.ground_shadow({0.0f, 0.0f}, {kShadowRx * 2.6f, kShadowRy * 1.8f}, shadow);
            paint_ram(p, art, pose);
            break;
        case Figure::Trebuchet:
            p.ground_shadow({0.0f, 0.0f}, {kShadowRx * 2.8f, kShadowRy * 2.0f}, shadow);
            paint_trebuchet(p, art, pose);
            break;
    }
    return p.finish(style);
}

// ---------------------------------------------------------------------------------
// Edificios: bloques isométricos sobre la huella, con caras sombreadas y texturas.

namespace {

// Esquinas de un rombo de la huella (en píxeles del lienzo): arriba, derecha, abajo, izquierda.
struct Diamond {
    Vec2 t;
    Vec2 r;
    Vec2 b;
    Vec2 l;
};

Diamond inset(const Diamond& d, float k) {
    const Vec2 c = (d.t + d.b) * 0.5f;
    return {c + (d.t - c) * k, c + (d.r - c) * k, c + (d.b - c) * k, c + (d.l - c) * k};
}

Diamond raised(const Diamond& d, float h) {
    const Vec2 up{0.0f, -h};
    return {d.t + up, d.r + up, d.b + up, d.l + up};
}

// Color de un muro según el material: u a lo largo de la cara, v hacia arriba (píxeles).
Color surface_color(Surface s, Color base, float u, float v, float grain, std::uint32_t seed) {
    const float n = (smooth_noise(u * 0.5f, v * 0.5f, seed) - 0.5f) * 2.0f * grain;
    float k = 1.0f + n;
    constexpr float kJoint = 0.72f;
    switch (s) {
        case Surface::Stone: {
            constexpr float kRow = 4.0f;
            constexpr float kBlock = 7.0f;
            const float row = std::floor(v / kRow);
            const float fv = v / kRow - row;
            const float fu = (u + row * 3.5f) / kBlock - std::floor((u + row * 3.5f) / kBlock);
            if (fv < 0.2f || fu < 0.12f) {
                k *= kJoint;
            }
            k *= 0.92f + 0.16f * hash01(static_cast<std::int32_t>(std::floor((u + row * 3.5f) / kBlock)),
                                        static_cast<std::int32_t>(row), seed);
            break;
        }
        case Surface::Wood: {
            constexpr float kPlank = 4.0f;
            const float fu = u / kPlank - std::floor(u / kPlank);
            if (fu < 0.18f) {
                k *= kJoint;
            }
            k *= 0.94f + 0.12f * smooth_noise(std::floor(u / kPlank), v * 0.15f, seed + 7);
            break;
        }
        case Surface::Thatch: {
            k *= 0.85f + 0.3f * smooth_noise(u * 1.5f, v * 0.25f, seed + 3);
            break;
        }
        case Surface::Tile: {
            constexpr float kRow = 3.0f;
            const float row = std::floor(v / kRow);
            if (v / kRow - row < 0.25f) {
                k *= 0.78f;
            }
            const float fu = (u + row * 2.0f) / 4.0f - std::floor((u + row * 2.0f) / 4.0f);
            if (fu < 0.12f) {
                k *= 0.85f;
            }
            break;
        }
        case Surface::Canvas:
        case Surface::Plaster:
        case Surface::Soil:
            break;
    }
    return shade(base, k);
}

// Cara vertical entre a y b (borde de abajo), de alto h: color por material.
void wall_face(Painter& p, Vec2 a, Vec2 b, float h, Surface s, Color base, float grain, std::uint32_t seed) {
    const Vec2 up{0.0f, -h};
    const float len = length(b - a);
    p.polygon_px({a, b, b + up, a + up}, [&](float x, float y) {
        const float t = std::abs(b.x - a.x) > 0.0f ? std::clamp((x - a.x) / (b.x - a.x), 0.0f, 1.0f) : 0.0f;
        const float ground = a.y + (b.y - a.y) * t;
        return surface_color(s, base, t * len, ground - y, grain, seed);
    });
}

// Paralelogramo sobre una cara: de t0 a t1 a lo largo de a→b, de v0 a v1 de alto.
void face_patch(Painter& p, Vec2 a, Vec2 b, float t0, float t1, float v0, float v1, Color c, Layer layer = Layer::Body) {
    const Vec2 e = b - a;
    const Vec2 p0 = a + e * t0;
    const Vec2 p1 = a + e * t1;
    std::vector<Vec2> pts{p0 + Vec2{0.0f, -v0}, p1 + Vec2{0.0f, -v0}, p1 + Vec2{0.0f, -v1}, p0 + Vec2{0.0f, -v1}};
    p.polygon_px(pts, [c](float, float) { return c; }, layer, 1.0f);
}

void roof_polygon(Painter& p, std::vector<Vec2> pts, Surface s, Color base, float grain, std::uint32_t seed) {
    p.polygon_px(pts, [&](float x, float y) { return surface_color(s, base, x * 0.7f + y, y * 1.4f, grain, seed); });
}

void paint_banner(Painter& p, Vec2 base, float pole, const BuildingArt& art) {
    const Color pole_color = shade(rgb(art.trim), 0.7f);
    p.polygon_px({base + Vec2{-0.6f, 0.0f}, base + Vec2{0.6f, 0.0f}, base + Vec2{0.6f, -pole}, base + Vec2{-0.6f, -pole}},
                 [&](float, float) { return pole_color; });
    const Vec2 top = base + Vec2{0.6f, -pole};
    p.polygon_px({top, top + Vec2{pole * 0.55f, pole * 0.12f}, top + Vec2{0.0f, pole * 0.35f}},
                 [](float, float) { return Color{0.85f, 0.85f, 0.85f, 1.0f}; }, Layer::Team, 0.95f);
}

}  // namespace

SpriteImages paint_building(const BuildingArt& art, const ArtStyle& style, const ViewParams& view) {
    const float n = static_cast<float>(std::max(art.size_tiles, 1));
    const float tw = static_cast<float>(view.tile_width_px) * n;
    const float th = static_cast<float>(view.tile_height_px) * n;
    const float extra = static_cast<float>(art.wall_px + art.roof_px) + th;  // estandartes y chimeneas
    const auto w = static_cast<std::int32_t>(std::ceil(tw)) + 4;
    const auto h = static_cast<std::int32_t>(std::ceil(th + extra)) + 4;
    const Vec2 center{static_cast<float>(w) * 0.5f, static_cast<float>(h) - 2.0f - th * 0.5f};
    Painter p(w, h, center, 1.0f);
    const Diamond foot{center + Vec2{0.0f, -th * 0.5f}, center + Vec2{tw * 0.5f, 0.0f}, center + Vec2{0.0f, th * 0.5f},
                       center + Vec2{-tw * 0.5f, 0.0f}};
    const float grain = percent(style.grain_percent);
    const float lit_l = percent(style.light_left_percent);
    const float lit_r = percent(style.light_right_percent);
    const Color wall = rgb(art.wall);
    const Color roof = rgb(art.roof_color);
    const Color trim = rgb(art.trim);
    const Color dark{0.08f, 0.06f, 0.05f, 1.0f};
    const auto seed = static_cast<std::uint32_t>(art.wall[0] * 7 + art.roof_color[1] * 13 + art.size_tiles);
    const float wall_h = static_cast<float>(art.wall_px);
    const float roof_h = static_cast<float>(art.roof_px);

    // Sombra: el rombo del bloque corrido hacia abajo a la derecha (luz de arriba a la izquierda).
    if (art.shape != BuildingShape::Field && art.shape != BuildingShape::Road) {
        const Diamond g = inset(foot, std::min(1.0f, percent(art.inset_percent) + 0.06f));
        const Vec2 off{tw * 0.04f, th * 0.04f};
        p.polygon_px({g.t + off, g.r + off, g.b + off, g.l + off},
                     [&](float, float) { return Color{0.0f, 0.0f, 0.0f, percent(style.shadow_alpha_percent) * 0.7f}; },
                     Layer::Shadow);
    }

    switch (art.shape) {
        case BuildingShape::Field:
        case BuildingShape::Road: {
            const bool road = art.shape == BuildingShape::Road;
            const Diamond d = inset(foot, percent(art.inset_percent));
            p.polygon_px({d.t, d.r, d.b, d.l}, [&](float x, float y) {
                // Surcos paralelos a un eje (campo) o rodadas (camino).
                const Vec2 rel = Vec2{x, y} - d.l;
                const float along = rel.x / tw * 2.0f + rel.y / th * 2.0f;  // de l a b
                const float across = rel.x / tw * 2.0f - rel.y / th * 2.0f;  // de l a t
                const float k = road ? 0.9f + 0.2f * smooth_noise(x * 0.3f, y * 0.3f, seed) : 1.0f;
                if (road) {
                    const float f = across * n - std::floor(across * n);
                    return shade(wall, (f > 0.28f && f < 0.36f) || (f > 0.64f && f < 0.72f) ? k * 0.8f : k);
                }
                const float rows = n * 4.0f;
                const float f = along * rows - std::floor(along * rows);
                const Color c = f < 0.5f ? roof : wall;
                return shade(c, 0.9f + 0.2f * smooth_noise(x * 0.4f, y * 0.4f, seed));
            });
            if (!road) {
                // Lindes de seto: los dos bordes de delante.
                face_patch(p, d.l, d.b, 0.0f, 1.0f, 0.0f, 2.0f, shade(trim, lit_l));
                face_patch(p, d.b, d.r, 0.0f, 1.0f, 0.0f, 2.0f, shade(trim, lit_r));
            }
            break;
        }
        case BuildingShape::Tents: {
            // Tres tiendas de lona en la huella y un estandarte.
            const std::array<Vec2, 3> spots{Vec2{-0.22f, -0.1f}, Vec2{0.22f, -0.05f}, Vec2{0.0f, 0.18f}};
            for (const Vec2& s : spots) {
                const Vec2 c = center + Vec2{s.x * tw, s.y * th};
                const float r = tw * 0.16f;
                const float ht = static_cast<float>(art.wall_px);
                const Vec2 apex = c + Vec2{0.0f, -ht};
                p.polygon_px({c + Vec2{-r, 0.0f}, c + Vec2{0.0f, r * 0.5f}, apex}, [&](float, float) {
                    return shade(roof, lit_l);
                });
                p.polygon_px({c + Vec2{0.0f, r * 0.5f}, c + Vec2{r, 0.0f}, apex},
                             [&](float, float) { return shade(roof, lit_r); });
                p.polygon_px({c + Vec2{-r * 0.15f, r * 0.42f}, c + Vec2{r * 0.15f, r * 0.42f}, c + Vec2{0.0f, -ht * 0.45f}},
                             [&](float, float) { return dark; });
            }
            if (art.banner) {
                paint_banner(p, center + Vec2{0.0f, -th * 0.05f}, static_cast<float>(art.wall_px) * 1.6f, art);
            }
            break;
        }
        case BuildingShape::Stalls: {
            // Puestos: mostrador de madera, mercancía y toldo a dos aguas con rayas del jugador.
            const std::array<Vec2, 4> spots{Vec2{0.0f, -0.2f}, Vec2{-0.22f, 0.0f}, Vec2{0.22f, 0.0f}, Vec2{0.0f, 0.2f}};
            for (std::size_t si = 0; si < spots.size(); ++si) {
                const Vec2 sc = center + Vec2{spots[si].x * tw, spots[si].y * th};
                const Diamond d = inset({sc + Vec2{0.0f, -th * 0.5f}, sc + Vec2{tw * 0.5f, 0.0f}, sc + Vec2{0.0f, th * 0.5f},
                                         sc + Vec2{-tw * 0.5f, 0.0f}},
                                        0.3f);
                const float post = wall_h;
                const float counter = post * 0.35f;
                wall_face(p, d.l, d.b, counter, Surface::Wood, shade(wall, lit_l), grain, seed);
                wall_face(p, d.b, d.r, counter, Surface::Wood, shade(wall, lit_r), grain, seed + 1);
                const Diamond ct = raised(d, counter);
                p.polygon_px({ct.t, ct.r, ct.b, ct.l}, [&](float, float) { return shade(wall, 1.05f); });
                // Mercancía: sacos y cestos.
                for (int k = 0; k < 3; ++k) {
                    const Vec2 g = ct.l + (ct.r - ct.l) * (0.3f + 0.2f * static_cast<float>(k));
                    p.circle(g - center + Vec2{0.0f, -1.5f}, 1.8f,
                             k == 1 ? Color{0.75f, 0.62f, 0.3f, 1.0f} : Color{0.55f, 0.6f, 0.3f, 1.0f});
                }
                for (const Vec2& c : {d.l, d.b, d.r}) {
                    p.polygon_px({c + Vec2{-0.6f, 0.0f}, c + Vec2{0.6f, 0.0f}, c + Vec2{0.6f, -post}, c + Vec2{-0.6f, -post}},
                                 [&](float, float) { return shade(trim, 0.8f); });
                }
                const Diamond top = raised(inset(d, 1.15f), post);
                const Vec2 up{0.0f, -post * 0.35f};
                const Vec2 m1 = (top.t + top.l) * 0.5f + up;
                const Vec2 m2 = (top.r + top.b) * 0.5f + up;
                const auto stripes = [&](float k) {
                    return [&, k](float x, float y) {
                        const float f = (x - top.l.x) * 0.45f + (y - top.t.y) * 0.9f;
                        const bool band = f - std::floor(f / 4.0f) * 4.0f < 2.0f;
                        return band ? Color{k, k, k, 1.0f} : Color{0.95f * k, 0.93f * k, 0.88f * k, 1.0f};
                    };
                };
                p.polygon_px({top.t, top.r, m2, m1}, stripes(lit_r * 0.9f), Layer::Team, lit_r * 0.9f);
                p.polygon_px({top.l, top.b, m2, m1}, stripes(lit_l), Layer::Team, lit_l);
            }
            break;
        }
        case BuildingShape::Block:
        case BuildingShape::Wall:
        case BuildingShape::Tower:
        case BuildingShape::Mill: {
            const Diamond d = inset(foot, percent(art.inset_percent));
            const Diamond top = raised(d, wall_h);
            wall_face(p, d.l, d.b, wall_h, art.wall_surface, shade(wall, lit_l), grain, seed);
            wall_face(p, d.b, d.r, wall_h, art.wall_surface, shade(wall, lit_r), grain, seed + 1);
            if (art.timber) {
                const Color beam = shade(trim, 0.8f);
                for (const auto& [a, b, k] : {std::tuple{d.l, d.b, lit_l}, std::tuple{d.b, d.r, lit_r}}) {
                    face_patch(p, a, b, 0.0f, 1.0f, wall_h - 1.6f, wall_h, shade(beam, k));
                    face_patch(p, a, b, 0.0f, 1.0f, 0.0f, 1.4f, shade(beam, k));
                    face_patch(p, a, b, 0.0f, 0.06f, 0.0f, wall_h, shade(beam, k));
                    face_patch(p, a, b, 0.47f, 0.53f, 0.0f, wall_h, shade(beam, k));
                    face_patch(p, a, b, 0.94f, 1.0f, 0.0f, wall_h, shade(beam, k));
                }
            }
            if (art.gate) {
                for (const auto& [a, b] : {std::pair{d.l, d.b}, std::pair{d.b, d.r}}) {
                    face_patch(p, a, b, 0.3f, 0.7f, 0.0f, wall_h * 0.62f, dark);
                    face_patch(p, a, b, 0.36f, 0.64f, wall_h * 0.62f, wall_h * 0.7f, dark);
                }
            }
            if (art.door) {
                face_patch(p, d.l, d.b, 0.4f, 0.6f, 0.0f, std::min(wall_h * 0.6f, 11.0f), shade(trim, 0.45f));
            }
            for (std::int32_t i = 0; i < art.windows; ++i) {
                const float t = (static_cast<float>(i) + 0.5f) / static_cast<float>(art.windows);
                const float half = 0.06f / std::max(1.0f, n * 0.5f);
                const float v0 = wall_h * 0.55f;
                const float v1 = std::min(wall_h * 0.8f, v0 + 6.0f);
                face_patch(p, d.b, d.r, t - half, t + half, v0, v1, dark);
                if (!art.door || std::abs(t - 0.5f) > 0.2f) {
                    face_patch(p, d.l, d.b, t - half, t + half, v0, v1, dark);
                }
            }
            // Tejado.
            switch (art.roof) {
                case Roof::None:
                    break;
                case Roof::Flat:
                case Roof::Battlements:
                case Roof::Stakes: {
                    roof_polygon(p, {top.t, top.r, top.b, top.l}, art.roof_surface, roof, grain, seed + 2);
                    if (art.roof == Roof::Battlements) {
                        // Almenas en los dos bordes de delante.
                        constexpr int kMerlons = 4;
                        for (const auto& [a, b, k] : {std::tuple{top.l, top.b, lit_l}, std::tuple{top.b, top.r, lit_r}}) {
                            for (int i = 0; i < kMerlons; ++i) {
                                const float t0 = static_cast<float>(i) / kMerlons;
                                face_patch(p, a, b, t0, t0 + 0.5f / kMerlons, 0.0f, std::max(3.0f, roof_h),
                                           shade(wall, k));
                            }
                        }
                    } else if (art.roof == Roof::Stakes) {
                        // Puntas de las estacas por encima del borde.
                        constexpr int kStakes = 7;
                        for (const auto& [a, b, k] : {std::tuple{top.l, top.b, lit_l}, std::tuple{top.b, top.r, lit_r}}) {
                            for (int i = 0; i < kStakes; ++i) {
                                const float t = (static_cast<float>(i) + 0.5f) / kStakes;
                                const Vec2 c = a + (b - a) * t;
                                const float half = length(b - a) / kStakes * 0.5f;
                                p.polygon_px({c + Vec2{-half, 0.0f}, c + Vec2{half, 0.0f}, c + Vec2{0.0f, -roof_h}},
                                             [&](float, float) { return shade(wall, k); });
                            }
                        }
                    }
                    break;
                }
                case Roof::Gable: {
                    // Cumbrera de l-t a b-r: faldón de detrás, faldón de delante y hastial.
                    const Vec2 up{0.0f, -roof_h};
                    const Vec2 m1 = (top.t + top.l) * 0.5f + up;
                    const Vec2 m2 = (top.r + top.b) * 0.5f + up;
                    const float overhang = 1.08f;
                    const Diamond o = inset(top, overhang);
                    roof_polygon(p, {o.t, o.r, m2, m1}, art.roof_surface, shade(roof, lit_r * 0.9f), grain, seed + 2);
                    p.polygon_px({top.r, top.b, m2}, [&](float, float) { return shade(wall, lit_r * 0.92f); });
                    roof_polygon(p, {o.l, o.b, m2, m1}, art.roof_surface, shade(roof, lit_l), grain, seed + 3);
                    break;
                }
                case Roof::Hip:
                case Roof::Cone: {
                    const Vec2 apex = (top.t + top.b) * 0.5f + Vec2{0.0f, -roof_h};
                    const Diamond o = inset(top, art.roof == Roof::Cone ? 1.0f : 1.08f);
                    roof_polygon(p, {o.t, o.r, apex}, art.roof_surface, shade(roof, lit_r * 0.8f), grain, seed + 2);
                    roof_polygon(p, {o.l, o.t, apex}, art.roof_surface, shade(roof, lit_l * 0.95f), grain, seed + 3);
                    roof_polygon(p, {o.l, o.b, apex}, art.roof_surface, shade(roof, lit_l), grain, seed + 4);
                    roof_polygon(p, {o.b, o.r, apex}, art.roof_surface, shade(roof, lit_r), grain, seed + 5);
                    break;
                }
            }
            if (art.chimney) {
                // Sobre el faldón, entre la cumbrera y el alero de la derecha.
                const Vec2 mid = (top.t + top.b) * 0.5f;
                const Vec2 c = mid + (top.r - mid) * 0.45f + Vec2{0.0f, -roof_h * 0.5f};
                const Diamond cd{c + Vec2{0.0f, -2.0f}, c + Vec2{4.0f, 0.0f}, c + Vec2{0.0f, 2.0f}, c + Vec2{-4.0f, 0.0f}};
                wall_face(p, cd.l, cd.b, roof_h * 0.6f + 4.0f, Surface::Stone, shade(trim, lit_l), grain, seed);
                wall_face(p, cd.b, cd.r, roof_h * 0.6f + 4.0f, Surface::Stone, shade(trim, lit_r), grain, seed);
            }
            if (art.shape == BuildingShape::Mill) {
                // Aspas del molino sobre la cara derecha.
                const Vec2 hub = (d.b + d.r) * 0.5f + Vec2{2.0f, -wall_h * 0.9f};
                const float blade = (wall_h + roof_h) * 0.95f;
                constexpr int kBlades = 4;
                for (int i = 0; i < kBlades; ++i) {
                    const float a = std::numbers::pi_v<float> * 0.25f + std::numbers::pi_v<float> * 0.5f * static_cast<float>(i);
                    const Vec2 dir{std::cos(a), std::sin(a) * 0.8f};
                    const Vec2 tip = hub + dir * blade;
                    const Vec2 side{-dir.y * 4.5f, dir.x * 4.5f};
                    p.capsule(hub - center, tip - center, 0.7f, shade(trim, 0.7f));
                    p.polygon_px({hub + dir * 4.0f + side * 0.15f, tip + side * 0.15f, tip + side, hub + dir * 4.0f + side},
                                 [&](float, float) { return Color{0.92f, 0.9f, 0.84f, 1.0f}; });
                }
                p.polygon_px({hub + Vec2{-1.5f, -1.5f}, hub + Vec2{1.5f, -1.5f}, hub + Vec2{1.5f, 1.5f},
                              hub + Vec2{-1.5f, 1.5f}},
                             [&](float, float) { return dark; });
            }
            if (art.banner) {
                const Vec2 peak = art.roof == Roof::Gable ? (top.r + top.b) * 0.5f + Vec2{0.0f, -roof_h}
                                                          : (top.t + top.b) * 0.5f + Vec2{0.0f, -roof_h};
                paint_banner(p, peak, std::max(8.0f, wall_h * 0.6f), art);
            }
            break;
        }
    }
    return p.finish(style);
}

// ---------------------------------------------------------------------------------
// Recursos.

Image paint_node(const NodeArt& art, const ArtStyle& style, const ViewParams& view, std::int32_t variant) {
    const float n = static_cast<float>(std::max(art.size_tiles, 1));
    const float tw = static_cast<float>(view.tile_width_px) * n;
    const float th = static_cast<float>(view.tile_height_px) * n;
    const float ht = static_cast<float>(art.height_px);
    const auto w = static_cast<std::int32_t>(std::ceil(tw)) + 4;
    const auto h = static_cast<std::int32_t>(std::ceil(th + ht)) + 4;
    const Vec2 center{static_cast<float>(w) * 0.5f, static_cast<float>(h) - 2.0f - th * 0.5f};
    Painter p(w, h, center, 1.0f);
    const Color main = rgb(art.main);
    const Color accent = rgb(art.accent);
    const auto seed = static_cast<std::uint32_t>(variant * 31 + art.main[1]);
    const float shadow = percent(style.shadow_alpha_percent);
    const auto rnd = [&](std::int32_t i) { return hash01(i, variant, seed); };
    // Uno de cada tres árboles frondosos es un pino: el bosque no parece clonado.
    constexpr std::int32_t kPineEvery = 3;
    const NodeShape shape =
        art.shape == NodeShape::Tree && variant % kPineEvery == kPineEvery - 1 ? NodeShape::Pine : art.shape;
    switch (shape) {
        case NodeShape::Tree: {
            p.ground_shadow({tw * 0.06f, 0.0f}, {tw * 0.3f, th * 0.26f}, shadow);
            const float trunk_h = ht * 0.4f;
            p.capsule({0.0f, 0.0f}, {0.0f, -trunk_h}, std::max(1.6f, tw * 0.045f), shade(accent, 0.9f));
            // Copa: bolas con luz arriba a la izquierda.
            const int blobs = 5 + variant % 3;
            for (int i = 0; i < blobs; ++i) {
                const float a = static_cast<float>(i) / static_cast<float>(blobs) * 2.0f * std::numbers::pi_v<float>;
                const float r = ht * (0.17f + 0.06f * rnd(i));
                const Vec2 c{std::cos(a) * ht * 0.18f, -trunk_h - ht * 0.22f + std::sin(a) * ht * 0.13f};
                p.circle(c, r, shade(main, 0.75f + 0.15f * rnd(i + 10)));
            }
            p.circle({-ht * 0.06f, -trunk_h - ht * 0.32f}, ht * 0.2f, main);
            p.circle({-ht * 0.1f, -trunk_h - ht * 0.38f}, ht * 0.11f, shade(main, 1.25f));
            break;
        }
        case NodeShape::Pine: {
            p.ground_shadow({tw * 0.05f, 0.0f}, {tw * 0.22f, th * 0.2f}, shadow);
            p.capsule({0.0f, 0.0f}, {0.0f, -ht * 0.25f}, std::max(1.4f, tw * 0.04f), shade(accent, 0.9f));
            constexpr int kTiers = 3;
            for (int i = 0; i < kTiers; ++i) {
                const float base_y = -ht * (0.18f + 0.24f * static_cast<float>(i));
                const float half = ht * (0.3f - 0.07f * static_cast<float>(i)) * (0.9f + 0.2f * rnd(i));
                const float tip_y = base_y - ht * 0.42f;
                p.polygon({{-half, base_y}, {0.0f, base_y + half * 0.2f}, {0.0f, tip_y}}, shade(main, 1.1f));
                p.polygon({{0.0f, base_y + half * 0.2f}, {half, base_y}, {0.0f, tip_y}}, shade(main, 0.75f));
            }
            break;
        }
        case NodeShape::Bush: {
            p.ground_shadow({0.0f, 0.0f}, {tw * 0.24f, th * 0.2f}, shadow);
            for (int i = 0; i < 5; ++i) {
                const Vec2 c{(rnd(i) - 0.5f) * tw * 0.3f, -ht * 0.35f + (rnd(i + 5) - 0.5f) * ht * 0.3f};
                p.circle(c, ht * 0.3f, shade(main, 0.8f + 0.25f * rnd(i + 3)));
            }
            for (int i = 0; i < 9; ++i) {
                const Vec2 c{(rnd(i + 20) - 0.5f) * tw * 0.36f, -ht * 0.15f - rnd(i + 30) * ht * 0.55f};
                p.circle(c, std::max(1.0f, ht * 0.07f), accent);
            }
            break;
        }
        case NodeShape::Rocks:
        case NodeShape::Ore:
        case NodeShape::Rubble: {
            const bool rubble = art.shape == NodeShape::Rubble;
            p.ground_shadow({0.0f, 0.0f}, {tw * 0.36f, th * 0.3f}, shadow * (rubble ? 0.5f : 1.0f));
            const int stones = rubble ? 9 : 5;
            for (int i = 0; i < stones; ++i) {
                const Vec2 c{(rnd(i) - 0.5f) * tw * 0.5f, (rnd(i + 7) - 0.5f) * th * 0.45f};
                const float r = (rubble ? ht * 0.35f : ht * 0.45f) * (0.6f + 0.5f * rnd(i + 14));
                // Bloque irregular: cara de arriba clara, caras de los lados.
                const Vec2 top_c = c + Vec2{0.0f, -r * 0.9f};
                const std::array<Vec2, 6> ring{Vec2{-r, 0.0f},        Vec2{-r * 0.5f, r * 0.45f}, Vec2{r * 0.6f, r * 0.4f},
                                               Vec2{r, -r * 0.1f},    Vec2{r * 0.4f, -r * 0.5f}, Vec2{-r * 0.6f, -r * 0.45f}};
                std::vector<Vec2> body{c + ring[0], c + ring[1], c + ring[2], c + ring[3], top_c + ring[3],
                                       top_c + ring[4], top_c + ring[5], top_c + ring[0]};
                const bool plank = rubble && i % 3 == 0;
                p.polygon(body, shade(plank ? accent : main, 0.7f));
                std::vector<Vec2> cap;
                for (const Vec2& v : ring) {
                    cap.push_back(top_c + v);
                }
                p.polygon(cap, shade(plank ? accent : main, 1.1f));
                if (art.shape == NodeShape::Ore) {
                    for (int k = 0; k < 3; ++k) {
                        p.circle(top_c + Vec2{(rnd(i * 5 + k + 40) - 0.5f) * r, (rnd(i * 5 + k + 60) - 0.5f) * r * 0.5f},
                                 std::max(0.9f, r * 0.14f), accent);
                    }
                }
            }
            break;
        }
    }
    return p.finish(style).body;
}

// ---------------------------------------------------------------------------------
// Terreno, efectos y anillo.

Image paint_terrain(const TerrainArt& art, const ViewParams& view, std::int32_t variant) {
    const std::int32_t w = view.tile_width_px;
    const std::int32_t h = view.tile_height_px;
    Canvas c(w, h);
    const float hw = static_cast<float>(w) * 0.5f;
    const float hh = static_cast<float>(h) * 0.5f;
    const float grain = percent(art.grain_percent);
    const auto seed = static_cast<std::uint32_t>(variant + 1) * 977U + static_cast<std::uint32_t>(art.texture);
    constexpr int kSub = 4;
    for (std::int32_t y = 0; y < h; ++y) {
        for (std::int32_t x = 0; x < w; ++x) {
            // Cobertura del rombo por submuestreo.
            int hits = 0;
            for (int sy = 0; sy < kSub; ++sy) {
                for (int sx = 0; sx < kSub; ++sx) {
                    const float u = static_cast<float>(x) + (static_cast<float>(sx) + 0.5f) / kSub;
                    const float v = static_cast<float>(y) + (static_cast<float>(sy) + 0.5f) / kSub;
                    hits += std::abs(u - hw) / hw + std::abs(v - hh) / hh <= 1.0f ? 1 : 0;
                }
            }
            if (hits == 0) {
                continue;
            }
            const auto fx = static_cast<float>(x);
            const auto fy = static_cast<float>(y);
            float k = 1.0f + (smooth_noise(fx * 0.18f, fy * 0.36f, seed) - 0.5f) * grain;
            switch (art.texture) {
                case TerrainTexture::Grass: {
                    const float blade = hash01(x, y, seed);
                    if (blade > 0.93f) {
                        k *= 1.18f;
                    } else if (blade < 0.06f) {
                        k *= 0.8f;
                    }
                    break;
                }
                case TerrainTexture::Water: {
                    const float wave = std::sin(fx * 0.35f + fy * 0.9f + static_cast<float>(variant) * 1.7f +
                                                smooth_noise(fx * 0.1f, fy * 0.2f, seed) * 4.0f);
                    if (wave > 0.92f) {
                        k *= 1.22f;
                    }
                    break;
                }
                case TerrainTexture::Sand:
                    k *= 0.96f + 0.08f * hash01(x, y, seed);
                    break;
                case TerrainTexture::Rock: {
                    const float crack = std::abs(smooth_noise(fx * 0.12f, fy * 0.24f, seed + 5) - 0.5f);
                    if (crack < 0.03f) {
                        k *= 0.72f;
                    }
                    break;
                }
                case TerrainTexture::Soil: {
                    if (hash01(x, y, seed + 2) > 0.9f) {
                        k *= 0.78f;
                    }
                    break;
                }
            }
            // Borde duro: las casillas vecinas se solapan medio píxel en vez de dejar ver
            // el fondo por las juntas (con alfa parcial, cada junta se oscurecería dos veces).
            const float v = std::clamp(k * 0.88f, 0.0f, 1.0f);
            c.at(x, y) = {v, v, v, 1.0f};
        }
    }
    return c.to_image({hw, 0.0f});
}

Image paint_flame(const EffectArt& art, std::int32_t frame, std::int32_t frames) {
    const auto hgt = static_cast<float>(art.flame_px);
    const auto w = static_cast<std::int32_t>(std::ceil(hgt * 0.7f)) + 2;
    const auto h = static_cast<std::int32_t>(std::ceil(hgt)) + 2;
    Painter p(w, h, {static_cast<float>(w) * 0.5f, static_cast<float>(h) - 1.0f}, 1.0f);
    const float phase = static_cast<float>(frame) / static_cast<float>(std::max(frames, 1)) * 2.0f * std::numbers::pi_v<float>;
    const float sway = std::sin(phase) * hgt * 0.08f;
    const float tall = hgt * (0.82f + 0.12f * std::sin(phase * 2.0f + 1.0f));
    const auto tongue = [&](float k, Color c) {
        const float r = hgt * 0.24f * k;
        p.circle({0.0f, -r}, r, c);
        p.polygon({{-r, -r}, {r, -r}, {sway * k, -tall * k}}, c);
    };
    tongue(1.0f, rgb(art.flame_outer, 0.92f));
    tongue(0.62f, rgb(art.flame_inner));
    ArtStyle none;
    return p.finish(none).body;
}

Image paint_smoke(const EffectArt& art) {
    const std::int32_t d = std::max(art.smoke_px, 2);
    Canvas c(d, d);
    const float r = static_cast<float>(d) * 0.5f;
    for (std::int32_t y = 0; y < d; ++y) {
        for (std::int32_t x = 0; x < d; ++x) {
            const float dist = std::hypot(static_cast<float>(x) + 0.5f - r, static_cast<float>(y) + 0.5f - r) / r;
            if (dist < 1.0f) {
                // Denso en el centro y deshilachado en el borde.
                const float a = (1.0f - dist * dist) * (0.75f + 0.25f * smooth_noise(static_cast<float>(x) * 0.4f,
                                                                                     static_cast<float>(y) * 0.4f, 11));
                c.at(x, y) = {1.0f, 1.0f, 1.0f, a};
            }
        }
    }
    return c.to_image({r, r});
}

Image paint_ground_ring(std::int32_t width_px) {
    const std::int32_t w = std::max(width_px, 4);
    const std::int32_t h = w / 2;
    Canvas c(w, h);
    const float rx = static_cast<float>(w) * 0.5f - 0.5f;
    const float ry = static_cast<float>(h) * 0.5f - 0.5f;
    constexpr float kThickness = 1.4f;
    for (std::int32_t y = 0; y < h; ++y) {
        for (std::int32_t x = 0; x < w; ++x) {
            const float qx = (static_cast<float>(x) + 0.5f - static_cast<float>(w) * 0.5f) / rx;
            const float qy = (static_cast<float>(y) + 0.5f - static_cast<float>(h) * 0.5f) / ry;
            const float d = std::abs(std::hypot(qx, qy) - 1.0f) * ry;
            const float cov = std::clamp(kThickness * 0.5f - d + 0.5f, 0.0f, 1.0f);
            if (cov > 0.0f) {
                c.at(x, y) = {1.0f, 1.0f, 1.0f, cov};
            }
        }
    }
    return c.to_image({static_cast<float>(w) * 0.5f, static_cast<float>(h) * 0.5f});
}

}  // namespace rts::render
