#pragma once

// Configuración de arranque, leída de data/. Ningún parámetro ajustable vive en el código.

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "audio/sound_spec.hpp"
#include "game/lockstep.hpp"
#include "render/art.hpp"
#include "render/view_params.hpp"
#include "sim/world.hpp"

namespace rts::game {

// --- data/terrain.toml ---------------------------------------------------------

struct TerrainInfo {
    std::string name;
    bool passable = false;
    std::array<std::uint8_t, 3> color{};  // RGB provisional
    std::int32_t speed_percent = 100;        // B2: velocidad de marcha
    std::int32_t arrow_cover_percent = 100;  // B2: daño de proyectiles que llega
    bool charge = false;                     // B2: la caballería puede cargar
};

struct TerrainCatalog {
    // El índice en el vector es el TerrainId.
    std::vector<TerrainInfo> types;

    [[nodiscard]] std::optional<sim::TerrainId> find(std::string_view name) const;
};

// --- data/units.toml -----------------------------------------------------------

struct UnitInfo {
    std::string name;
    std::string label;  // inicial que se dibuja sobre el marcador
    sim::UnitType type;
    std::array<std::uint8_t, 3> color{};  // RGB provisional
};

struct UnitCatalog {
    // El índice en el vector es el UnitTypeId.
    std::vector<UnitInfo> types;
    // Clases de armadura ("classes" en units.toml); el índice es el ArmorClassId.
    std::vector<std::string> classes;

    [[nodiscard]] std::optional<sim::UnitTypeId> find(std::string_view name) const;
    [[nodiscard]] std::optional<sim::ArmorClassId> find_class(std::string_view name) const;
};

// --- Recursos ------------------------------------------------------------------

// Nombre de cada recurso en los ficheros de datos (claves de coste, "resource = ...").
[[nodiscard]] std::string_view resource_key(sim::Resource r) noexcept;
[[nodiscard]] std::optional<sim::Resource> find_resource(std::string_view key) noexcept;

// --- data/resources.toml ---------------------------------------------------------

struct NodeInfo {
    std::string name;
    sim::ResourceNodeType type;
    std::array<std::uint8_t, 3> color{};
};

struct NodeCatalog {
    std::vector<NodeInfo> types;  // el índice es el NodeTypeId

    [[nodiscard]] std::optional<sim::NodeTypeId> find(std::string_view name) const;
};

// --- data/buildings.toml ---------------------------------------------------------

struct BuildingInfo {
    std::string name;
    sim::BuildingType type;
    std::array<std::uint8_t, 3> color{};
};

// Mejora de la herrería (C1).
struct UpgradeInfo {
    std::string name;
    std::string label;  // nombre que se muestra
    sim::UpgradeType type;
};

struct BuildingCatalog {
    std::vector<BuildingInfo> types;  // el índice es el BuildingTypeId
    std::vector<UpgradeInfo> upgrades;  // el índice es el UpgradeId (opcional en el fichero)

    [[nodiscard]] std::optional<sim::BuildingTypeId> find(std::string_view name) const;
};

// --- data/scenarios/*.toml -------------------------------------------------------

enum class ScenarioAction : std::uint8_t { Move, Gather, Build, Train };

// Orden de un guion, al inicio de tick. first y count eligen unidades del jugador por
// su orden de creación. Move: a target. Gather: al nodo del recurso más cercano a esas
// unidades. Build: colocar building junto al primer edificio del jugador y
// construirlo con ellas. Train: encolar count unidades de tipo unit en el primer
// edificio del jugador de tipo building.
struct ScenarioOrder {
    sim::Tick tick = 0;
    ScenarioAction action = ScenarioAction::Move;
    sim::PlayerId player = 0;
    std::int32_t first = 0;
    std::int32_t count = 0;
    sim::TileCoord target;
    sim::Resource resource = sim::Resource::Food;
    sim::BuildingTypeId building = 0;
    sim::UnitTypeId unit = 0;
};

struct Scenario {
    std::vector<ScenarioOrder> orders;
    // construir: el sitio se busca en anillos a partir de gap casillas del edificio de
    // referencia, hasta search_radius.
    std::int32_t build_gap_tiles = 0;
    std::int32_t build_search_radius_tiles = 0;
};

// --- data/config/engine.toml ---------------------------------------------------

struct WindowConfig {
    std::string title;
    std::int32_t width = 0;
    std::int32_t height = 0;
    bool vsync = true;
};

struct LoopConfig {
    std::int32_t max_ticks_per_frame = 0;
};

struct CameraConfig {
    std::int32_t scroll_keys_px_per_s = 0;
    std::int32_t scroll_edge_px_per_s = 0;
    std::int32_t edge_margin_px = 0;
};

struct SelectionConfig {
    std::int32_t drag_threshold_px = 0;
    std::int32_t click_radius_px = 0;
};

// Repeticiones (M5).
struct ReplayConfig {
    std::int32_t checkpoint_interval_ticks = 0;  // hash de estado grabado cada tantos ticks
    std::string directory;                       // grabación automática, junto al ejecutable
    std::vector<std::int32_t> speeds;            // multiplicadores del reproductor
};

// Tipo de mapa (F3): lo que cambia respecto al de [map].
struct MapPreset {
    std::string name;
    std::int32_t rivers = 0;
    std::int32_t river_width_tiles = 0;
    std::int32_t fords = 0;
};

// Partidas en red (E1), sección [net].
struct NetConfig {
    LockstepConfig lockstep;
    std::int32_t poll_wait_ms = 0;  // espera máxima por vuelta mientras faltan órdenes
    std::int32_t default_port = 0;  // puerto que proponen el menú y la sala
    std::string default_address;    // dirección que propone el menú al unirse
    ProbeConfig probe;
};

// Ajustes de una partida elegidos en el menú. Viajan como un fichero de datos más
// (config/partida.toml) para que la repetición y la partida guardada los lleven.
struct MatchSettings {
    std::uint64_t seed = 0;     // mapa y preparación
    std::string rival;          // perfil de IA de los jugadores que controla la IA
    bool fog = true;            // niebla de guerra
    // Partidas en red (E2): "humano" o "ia" por puesto, del 0 en adelante. Vacío: los
    // jugadores de engine.toml tal cual.
    std::vector<std::string> seats;
    std::string map;  // tipo de mapa (F3, [[map.preset]]); vacío: el de engine.toml
};

inline constexpr std::string_view kMatchSettingsFile = "config/partida.toml";

// Texto TOML de unos ajustes.
[[nodiscard]] std::string match_settings_toml(const MatchSettings& s);

// data/config/engine.toml, sección [alerts] (avisos al jugador).
struct AlertParams {
    std::int32_t cooldown_ticks = 0;  // el mismo aviso, en la misma zona, no se repite antes
    std::int32_t zone_tiles = 1;      // tamaño de zona para la espera
    std::int32_t show_ticks = 0;      // tiempo en pantalla
    std::int32_t max_shown = 1;
};

struct EngineConfig {
    WindowConfig window;
    LoopConfig loop;
    sim::WorldParams world;
    render::ViewParams view;
    CameraConfig camera;
    SelectionConfig selection;
    ReplayConfig replay;
    NetConfig net;
    AlertParams alerts;
    std::vector<std::array<std::uint8_t, 3>> player_colors;  // por PlayerId
    // Todos los puestos de [[player]], también los libres (E2): inicio y color.
    std::vector<sim::TileCoord> seat_starts;
    std::vector<MapPreset> map_presets;  // F3
    std::vector<std::array<std::uint8_t, 3>> seat_colors;
    std::vector<std::string> hero_names;                     // por índice de CombatParams
    std::vector<std::string> ai_profile_names;               // por índice de AiParams::profiles
};

// Los errores son textos legibles que nombran el fichero y la clave que falla.
std::expected<TerrainCatalog, std::string> parse_terrain_catalog(std::string_view toml_text,
                                                                 std::string_view source_name = "<memoria>");
std::expected<UnitCatalog, std::string> parse_unit_catalog(std::string_view toml_text,
                                                           std::string_view source_name = "<memoria>");
std::expected<NodeCatalog, std::string> parse_node_catalog(std::string_view toml_text,
                                                           std::string_view source_name = "<memoria>");
// Los edificios nombran las unidades que producen.
std::expected<BuildingCatalog, std::string> parse_building_catalog(std::string_view toml_text,
                                                                   const UnitCatalog& units,
                                                                   std::string_view source_name = "<memoria>");

// Catálogos contra los que se resuelven los nombres de los ficheros de configuración.
struct Catalogs {
    const TerrainCatalog& terrain;
    const UnitCatalog& units;
    const BuildingCatalog& buildings;
    const NodeCatalog& nodes;
};

std::expected<Scenario, std::string> parse_scenario(std::string_view toml_text,
                                                                      const Catalogs& catalogs,
                                                                      std::string_view source_name = "<memoria>");

// Las bandas del mapa nombran terrenos, la demo y la preparación nombran unidades,
// edificios y nodos: se resuelven contra los catálogos, que además aportan
// transitabilidad y tipos a la simulación.
std::expected<EngineConfig, std::string> parse_engine_config(std::string_view toml_text, const Catalogs& catalogs,
                                                             std::string_view source_name = "<memoria>");

// Fichero de datos en texto, con su ruta relativa a data/ (separador '/'). Las
// repeticiones llevan una copia de todos: se reproducen con los datos con que se jugaron.
struct DataFile {
    std::string path;
    std::string text;

    friend bool operator==(const DataFile&, const DataFile&) = default;
};

struct GameData {
    std::vector<DataFile> files;  // los textos de los que sale todo lo demás
    TerrainCatalog terrain;
    UnitCatalog units;
    NodeCatalog nodes;
    BuildingCatalog buildings;
    EngineConfig engine;
    Scenario headless_scenario;
    render::ArtSpec art;  // F1; vacío si los datos no traen art.toml (repeticiones antiguas)
    audio::SoundSpec sound;  // F2; vacío si no traen sound.toml
    std::string scenario_name;  // F3: escenario hecho a mano (vacío: mapa generado)
};

// data/art.toml: cómo se pinta cada tipo (F1). Todo tipo de los catálogos debe tener su arte.
std::expected<render::ArtSpec, std::string> parse_art_spec(std::string_view toml_text, const Catalogs& catalogs,
                                                          std::string_view source_name = "<memoria>");

// data/sound.toml: recetas de efectos, música y qué suena con cada suceso (F2).
std::expected<audio::SoundSpec, std::string> parse_sound_spec(std::string_view toml_text,
                                                             std::string_view source_name = "<memoria>");

// Rutas de los ficheros de datos, en el orden en que se analizan.
[[nodiscard]] std::span<const std::string_view> data_file_paths() noexcept;

// Lee los ficheros de data_file_paths() bajo data_dir, sin analizarlos.
std::expected<std::vector<DataFile>, std::string> read_data_files(const std::filesystem::path& data_dir);

// Analiza los textos (de disco o de una repetición). Deben estar todos.
// Los datos de base con estos ajustes de partida (sustituyen a los que hubiera).
std::expected<GameData, std::string> with_match_settings(const GameData& base, const MatchSettings& settings);
// Lo mismo con el texto TOML ya escrito (el que llega del anfitrión de una partida en red).
std::expected<GameData, std::string> with_match_settings_text(const GameData& base, std::string text);
std::expected<GameData, std::string> parse_game_data(std::vector<DataFile> files);

// read_data_files + parse_game_data.
std::expected<GameData, std::string> load_game_data(const std::filesystem::path& data_dir);

}  // namespace rts::game
