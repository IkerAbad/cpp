#pragma once

// Traducción de lo que hay que dibujar (mapa, marcadores, selección) a un lote de
// sprites. Sin SDL ni GPU: se prueba contando y comprobando instancias.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "render/atlas.hpp"
#include "render/projection.hpp"
#include "render/sprite_batch.hpp"
#include "render/view_params.hpp"
#include "sim/tile_map.hpp"

namespace rts::render {

struct Marker {
    Vec2 screen_pos;  // centro (sin arte) o pies (con arte), píxeles de pantalla
    bool selected = false;
    Rgba color{};     // color propio (tipo de unidad); seleccionado usa marker_selected_color
    Rgba owner{};     // anillo fino del color del jugador (con arte: su tela); alfa 0 = nada
    Rgba badge{};     // punto pequeño encima (p. ej. lo que lleva un aldeano); alfa 0 = nada
    std::int32_t health_permille = -1;  // barra de vida encima; -1 = sin barra
    bool hero = false;                  // anillo de héroe
    // Arte (F1): tipo de unidad cuya figura se dibuja (-1: disco plano), pose y sentido.
    std::int32_t unit_type = -1;
    Pose pose = Pose::Idle;
    bool mirror = false;               // mira a la izquierda
    Rgba tint{255, 255, 255, 255};     // multiplica la figura (penumbra, recuerdo)
};

// Objeto estático en el suelo (edificio o nodo de recurso): un rombo del tamaño de su
// huella y otro interior, más pequeño, con el color del tipo.
struct SceneObject {
    sim::TileCoord origin;
    std::int32_t size = 1;
    Rgba base{};                      // rombo de toda la huella; alfa 0 = sin él
    Rgba body{};                      // rombo interior
    std::int32_t body_percent = 100;  // lado del rombo interior respecto a la huella
    bool highlighted = false;         // seleccionado: se cubre con hover_tile_color
    std::int32_t health_permille = -1;  // barra de vida sobre el centro; -1 = sin barra
    // Arte (F1). Con art_type >= 0 se dibuja el sprite del edificio o del recurso en vez
    // de los rombos; base y body se ignoran.
    enum class Art : std::uint8_t { None, Building, Node };
    Art art = Art::None;
    std::int32_t art_type = -1;
    std::int32_t variant = 0;               // recursos: variante del dibujo
    Rgba team{};                            // color del dueño para estandartes y lonas
    Rgba tint{255, 255, 255, 255};          // penumbra, recuerdo, ruina
    std::int32_t build_permille = 1000;     // en obra: se ve la parte de abajo
    std::int32_t fire_permille = 0;         // llamas y humo
};

// Casilla teñida para superposiciones de depuración (sectores, campo de flujo).
struct TileTint {
    sim::TileCoord tile;
    Rgba color;
};

struct Rect {
    Vec2 min;
    Vec2 max;
};

struct Scene {
    const sim::TileMap* map = nullptr;
    Camera camera;
    Vec2 screen;
    std::span<const SceneObject> objects;  // en orden de pintado, bajo los marcadores
    std::span<const Marker> markers;       // en orden de pintado
    std::optional<Rect> drag_rect;
    std::optional<sim::TileCoord> hovered_tile;
    std::span<const TileTint> tile_tints;  // se dibujan sobre el terreno, bajo los marcadores
    std::span<const Vec2> path_points;     // puntos de ruta en pantalla
    std::span<const Vec2> projectiles;     // proyectiles en vuelo, en pantalla
    // Niebla de guerra del jugador que mira (sim::Fog por casilla); vacía = sin niebla.
    std::span<const std::uint8_t> fog;
    // Reloj de la presentación (segundos) para llamas y humo: no viene de la simulación.
    float time_s = 0.0f;
};

struct SceneStats {
    std::int32_t tiles_drawn = 0;
    std::int32_t objects_drawn = 0;
    std::int32_t markers_drawn = 0;
};

class SceneBuilder {
public:
    // terrain_colors[id] es el color RGB provisional del terreno id.
    SceneBuilder(const ViewParams& view, const Atlas& atlas, std::vector<std::array<std::uint8_t, 3>> terrain_colors);

    SceneStats build(const Scene& scene, SpriteBatch& out);

    // Color final de una casilla (terreno + sombreado). Público para las pruebas.
    [[nodiscard]] Rgba tile_color(const sim::TileMap& map, sim::TileCoord c);

private:
    struct Bar {
        Vec2 center_top;
        std::int32_t permille = 0;
    };

    void refresh_tile_colors(const sim::TileMap& map);
    void add_health_bar(Vec2 center_top, std::int32_t permille, SpriteBatch& out) const;
    void add_sprite(const Sprite& sprite, Vec2 at, Rgba color, bool mirror, SpriteBatch& out,
                    float visible_fraction = 1.0f) const;
    void draw_flat_object(const Scene& scene, const SceneObject& o, SpriteBatch& out);
    void draw_art_object(const Scene& scene, const SceneObject& o, SpriteBatch& out);
    void draw_fire(const Scene& scene, const SceneObject& o, Vec2 center, SpriteBatch& out) const;
    void draw_marker(const Marker& m, SpriteBatch& out);
    [[nodiscard]] bool is_flat(const SceneObject& o) const;

    ViewParams view_;
    const Atlas* atlas_;
    std::vector<Bar> bars_;
    struct Drawable {
        float depth = 0.0f;
        bool marker = false;
        std::size_t index = 0;
    };
    std::vector<Drawable> drawables_;
    IsoProjection proj_;
    AtlasRegion diamond_;
    AtlasRegion disc_;
    AtlasRegion ring_;
    AtlasRegion solid_;
    std::vector<std::array<std::uint8_t, 3>> terrain_colors_;

    // Colores por casilla, recalculados solo cuando cambia el mapa (puntero o revisión).
    std::vector<Rgba> tile_colors_;
    const sim::TileMap* cached_map_ = nullptr;
    std::uint32_t cached_revision_ = 0;
};

}  // namespace rts::render
