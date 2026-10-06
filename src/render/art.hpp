#pragma once

// Arte propio generado por código (F1). Nada se carga de fuera: cada figura, edificio,
// árbol, textura de terreno, llama o bocanada de humo se pinta al arrancar con
// primitivas suavizadas (círculos, cápsulas, polígonos) a partir de data/art.toml.
//
// Cada sprite tiene dos capas: el cuerpo, con sus colores, y la capa del jugador, en
// grises (el sombreado), que se tiñe con el color del dueño al dibujar: túnicas,
// gualdrapas, estandartes. Lo que tapa a la tela (un brazo, un escudo) la borra de la
// capa del jugador, así que dibujar una sobre otra da la figura correcta.

#include <array>
#include <cstdint>
#include <vector>

#include "render/projection.hpp"
#include "render/view_params.hpp"

namespace rts::render {

using Rgb = std::array<std::uint8_t, 3>;

enum class Figure : std::uint8_t { Person, Rider, PackAnimal, Cart, Ram, Trebuchet };
enum class Weapon : std::uint8_t { None, Spear, Lance, Sword, Axe, Bow, Crossbow, Hammer, Pick, Satchel };
enum class Helmet : std::uint8_t { None, Hood, Kettle, Great };
enum class Armor : std::uint8_t { None, Mail, Plate };

struct UnitArt {
    Figure figure = Figure::Person;
    Weapon weapon = Weapon::None;
    Helmet helmet = Helmet::None;
    Armor armor = Armor::None;
    bool shield = false;
    bool robe = false;           // túnica hasta los pies
    std::int32_t height_px = 0;  // alto de una persona de pie
    Rgb skin{};
    Rgb cloth{};    // color de la tela si no hay dueño (la capa del jugador la cubre)
    Rgb garment{};  // ropa que no es del jugador: calzas, capucha, mandil
    Rgb metal{};
    Rgb wood{};
    Rgb beast{};    // caballo, mula, buey
};

enum class BuildingShape : std::uint8_t { Block, Field, Road, Wall, Tower, Tents, Stalls, Mill };
enum class Roof : std::uint8_t { None, Gable, Hip, Flat, Battlements, Stakes, Cone };
enum class Surface : std::uint8_t { Stone, Wood, Plaster, Thatch, Tile, Canvas, Soil };

struct BuildingArt {
    std::int32_t size_tiles = 1;  // de buildings.toml
    BuildingShape shape = BuildingShape::Block;
    Roof roof = Roof::Gable;
    Surface wall_surface = Surface::Plaster;
    Surface roof_surface = Surface::Thatch;
    Rgb wall{};
    Rgb roof_color{};
    Rgb trim{};  // vigas, postes, marcos
    std::int32_t wall_px = 0;
    std::int32_t roof_px = 0;
    std::int32_t inset_percent = 100;  // lado del bloque respecto a la huella
    std::int32_t windows = 0;          // por cara
    bool door = false;
    bool gate = false;     // arco abierto en las dos caras (puertas de muralla)
    bool timber = false;   // entramado de madera sobre el enlucido
    bool chimney = false;
    bool banner = false;   // estandarte del jugador
};

enum class NodeShape : std::uint8_t { Tree, Pine, Bush, Rocks, Ore, Rubble };

struct NodeArt {
    std::int32_t size_tiles = 1;  // de resources.toml
    NodeShape shape = NodeShape::Tree;
    Rgb main{};
    Rgb accent{};  // bayas, vetas, tablones
    std::int32_t height_px = 0;
    std::int32_t variants = 1;
};

enum class TerrainTexture : std::uint8_t { Grass, Water, Sand, Rock, Soil };

struct TerrainArt {
    TerrainTexture texture = TerrainTexture::Grass;
    std::int32_t grain_percent = 0;  // amplitud del grano (ruido) sobre el color del terreno
};

struct EffectArt {
    Rgb flame_outer{};
    Rgb flame_inner{};
    Rgb smoke{};
    std::int32_t flame_px = 0;  // alto de una llama
    std::int32_t smoke_px = 0;  // diámetro de una bocanada
};

// Luz y contornos, comunes a todo.
struct ArtStyle {
    std::int32_t light_left_percent = 100;   // brillo de las caras que miran a la izquierda
    std::int32_t light_right_percent = 100;  // y a la derecha (la luz viene de arriba a la izquierda)
    Rgb outline{};
    std::int32_t outline_alpha_percent = 0;
    std::int32_t shadow_alpha_percent = 0;
    std::int32_t grain_percent = 0;  // ruido de los materiales
};

struct ArtSpec {
    ArtStyle style;
    std::vector<UnitArt> units;          // por UnitTypeId
    std::vector<BuildingArt> buildings;  // por BuildingTypeId
    std::vector<NodeArt> nodes;          // por NodeTypeId
    std::vector<TerrainArt> terrain;     // por TerrainId
    std::int32_t terrain_variants = 1;
    EffectArt effects;
    std::int32_t flame_frames = 1;
};

// Imagen RGBA8 con alfa recto. anchor: el punto que se pone sobre el mapa (los pies de
// una figura, el centro de la huella de un edificio o de un recurso).
struct Image {
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::vector<std::uint8_t> rgba;
    Vec2 anchor;

    [[nodiscard]] bool empty() const noexcept { return width == 0 || height == 0; }
    [[nodiscard]] std::array<std::uint8_t, 4> at(std::int32_t x, std::int32_t y) const noexcept;
    // Píxeles con algo de alfa.
    [[nodiscard]] std::int32_t opaque_pixels() const noexcept;
};

// Cuerpo y capa del jugador (vacía si no tiene).
struct SpriteImages {
    Image body;
    Image team;
};

// Poses de una figura, todas mirando a la derecha (a la izquierda se dibuja en espejo).
enum class Pose : std::uint8_t { Idle, StepA, StepB, Act, Strike, Count };
inline constexpr std::size_t kPoseCount = static_cast<std::size_t>(Pose::Count);

[[nodiscard]] SpriteImages paint_unit(const UnitArt& art, const ArtStyle& style, Pose pose);
[[nodiscard]] SpriteImages paint_building(const BuildingArt& art, const ArtStyle& style, const ViewParams& view);
[[nodiscard]] Image paint_node(const NodeArt& art, const ArtStyle& style, const ViewParams& view, std::int32_t variant);
// En grises: se tiñe con el color del terreno (que ya lleva el sombreado por altura).
[[nodiscard]] Image paint_terrain(const TerrainArt& art, const ViewParams& view, std::int32_t variant);
[[nodiscard]] Image paint_flame(const EffectArt& art, std::int32_t frame, std::int32_t frames);
// En grises con alfa decreciente hacia el borde: se tiñe al dibujar.
[[nodiscard]] Image paint_smoke(const EffectArt& art);
// Elipse 2:1 hueca a los pies de una unidad seleccionada, en blanco.
[[nodiscard]] Image paint_ground_ring(std::int32_t width_px);

}  // namespace rts::render
