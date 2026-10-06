# Grafo de dependencias. Las flechas solo van hacia abajo:
#
#   rts (ejecutable)
#    ├── rts_render ──── rts_platform ── SDL3
#    │      ├── imgui, shaders (dxc)
#    │      └── rts_render_core          <- proyección, atlas, escena: sin SDL
#    ├── rts_game_core ── toml++, rts_render_core, rts_net (sockets: sin SDL)
#    └── rts_sim ── EnTT                  <- sin SDL, sin E/S, sin coma flotante
#
# rts_sim no enlaza SDL: un #include <SDL3/...> en src/sim/ no compila.

include(Shaders)

# --- Simulación pura -----------------------------------------------------------
add_library(rts_sim STATIC
    sim/ai.cpp
    sim/combat.cpp
    sim/economy.cpp
    sim/fire.cpp
    sim/supply.cpp
    sim/medicine.cpp
    sim/morale.cpp
    sim/fatigue.cpp
    sim/garrison.cpp
    sim/climb.cpp
    sim/formation.cpp
    sim/market.cpp
    sim/vision.cpp
    sim/map_gen.cpp
    sim/movement.cpp
    sim/path/flow_field.cpp
    sim/path/grid.cpp
    sim/path/hpa.cpp
    sim/tile_map.cpp
    sim/world.cpp)
target_include_directories(rts_sim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(rts_sim PUBLIC EnTT::EnTT)
rts_set_warnings(rts_sim)

# --- Red (E1): sockets TCP propios, sin dependencias ------------------------------
add_library(rts_net STATIC
    net/socket.cpp)
target_include_directories(rts_net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(WIN32)
    target_link_libraries(rts_net PRIVATE ws2_32)
endif()
rts_set_warnings(rts_net)

# --- Presentación sin GPU (se prueba sin ventana) -------------------------------
add_library(rts_render_core STATIC
    render/art.cpp
    render/atlas.cpp
    render/scene.cpp)
target_link_libraries(rts_render_core PUBLIC rts_sim)
rts_set_warnings(rts_render_core)

# --- Núcleo de juego sin plataforma (paso fijo, configuración, cámara, selección)
add_library(rts_game_core STATIC
    game/camera_control.cpp
    game/config.cpp
    game/fixed_step.cpp
    game/replay.cpp
    game/wire.cpp
    game/lockstep.cpp
    game/alerts.cpp
    game/minimap.cpp
    game/selection.cpp)
target_link_libraries(rts_game_core
    PUBLIC rts_sim rts_render_core rts_net
    PRIVATE tomlplusplus::tomlplusplus)
rts_set_warnings(rts_game_core)

# --- Perfilado -----------------------------------------------------------------
add_library(rts_profile INTERFACE)
if(RTS_TRACY)
    target_compile_definitions(rts_profile INTERFACE RTS_TRACY)
    target_link_libraries(rts_profile INTERFACE Tracy::TracyClient)
endif()

# --- Plataforma ----------------------------------------------------------------
add_library(rts_platform STATIC
    platform/window.cpp)
target_include_directories(rts_platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(rts_platform PUBLIC SDL3::SDL3 rts_profile)
rts_set_warnings(rts_platform)

# --- Presentación con GPU ------------------------------------------------------
add_library(rts_render STATIC
    render/renderer.cpp)
rts_add_shaders(rts_render
    NAMESPACE rts::render::shaders
    SHADERS
        sprite_vert:render/shaders/sprite.vert.hlsl:vs
        sprite_frag:render/shaders/sprite.frag.hlsl:ps)
target_link_libraries(rts_render
    PUBLIC rts_render_core rts_platform
    PRIVATE imgui::imgui spdlog::spdlog)
rts_set_warnings(rts_render)

# --- Ejecutable ----------------------------------------------------------------
add_executable(rts
    game/main.cpp
    game/app.cpp)
target_link_libraries(rts PRIVATE rts_game_core rts_render rts_platform imgui::imgui spdlog::spdlog)
rts_set_warnings(rts)

# data/ se copia junto al ejecutable: el binario encuentra sus datos sin depender
# del directorio de trabajo.
add_custom_command(TARGET rts POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${PROJECT_SOURCE_DIR}/data $<TARGET_FILE_DIR:rts>/data
    COMMENT "Copiando data/ junto al ejecutable")
