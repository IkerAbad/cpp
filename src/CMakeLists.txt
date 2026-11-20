# Grafo de dependencias. Las flechas solo van hacia abajo:
#
#   reinos (ejecutable)
#    ├── reinos_render ──── reinos_platform ── SDL3
#    │      ├── imgui, shaders (dxc)
#    │      └── reinos_render_core          <- proyección, atlas, escena: sin SDL
#    ├── reinos_game_core ── toml++, reinos_render_core, reinos_net (sockets: sin SDL)
#    └── reinos_sim ── EnTT                  <- sin SDL, sin E/S, sin coma flotante
#
# reinos_sim no enlaza SDL: un #include <SDL3/...> en src/sim/ no compila.

# --- Simulación pura -----------------------------------------------------------
add_library(reinos_sim STATIC
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
    sim/objectives.cpp
    sim/world.cpp)
target_include_directories(reinos_sim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(reinos_sim PUBLIC EnTT::EnTT)
reinos_set_warnings(reinos_sim)

# --- Sonido (F2): síntesis y mezcla, sin SDL ------------------------------------
add_library(reinos_audio STATIC
    audio/synth.cpp
    audio/mixer.cpp
    audio/bank.cpp)
target_include_directories(reinos_audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
reinos_set_warnings(reinos_audio)

# --- Red (E1): sockets TCP propios, sin dependencias ------------------------------
add_library(reinos_net STATIC
    net/socket.cpp)
target_include_directories(reinos_net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(WIN32)
    target_link_libraries(reinos_net PRIVATE ws2_32)
endif()
reinos_set_warnings(reinos_net)

# --- Presentación sin GPU (se prueba sin ventana) -------------------------------
add_library(reinos_render_core STATIC
    render/art.cpp
    render/atlas.cpp
    render/scene.cpp)
target_link_libraries(reinos_render_core PUBLIC reinos_sim)
reinos_set_warnings(reinos_render_core)

# --- Núcleo de juego sin plataforma (paso fijo, configuración, cámara, selección)
add_library(reinos_game_core STATIC
    game/camera_control.cpp
    game/config.cpp
    game/fixed_step.cpp
    game/replay.cpp
    game/wire.cpp
    game/lockstep.cpp
    game/sound_director.cpp
    game/campaign.cpp
    game/crash_report.cpp
    game/fuzz.cpp
    game/i18n.cpp
    game/options.cpp
    game/scenario.cpp
    game/alerts.cpp
    game/minimap.cpp
    game/selection.cpp)
target_link_libraries(reinos_game_core
    PUBLIC reinos_sim reinos_render_core reinos_net reinos_audio
    PRIVATE tomlplusplus::tomlplusplus)
reinos_set_warnings(reinos_game_core)

# --- Perfilado -----------------------------------------------------------------
add_library(reinos_profile INTERFACE)
if(REINOS_TRACY)
    target_compile_definitions(reinos_profile INTERFACE REINOS_TRACY)
    target_link_libraries(reinos_profile INTERFACE Tracy::TracyClient)
endif()

# Sin REINOS_BUILD_GAME acaba aquí: lo que sigue necesita SDL3, imgui, spdlog y dxc.
if(NOT REINOS_BUILD_GAME)
    return()
endif()

include(Shaders)

# --- Plataforma ----------------------------------------------------------------
add_library(reinos_platform STATIC
    platform/window.cpp
    platform/audio.cpp)
target_include_directories(reinos_platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(reinos_platform PUBLIC SDL3::SDL3 reinos_profile reinos_audio)
reinos_set_warnings(reinos_platform)

# --- Presentación con GPU ------------------------------------------------------
add_library(reinos_render STATIC
    render/renderer.cpp)
reinos_add_shaders(reinos_render
    NAMESPACE reinos::render::shaders
    SHADERS
        sprite_vert:render/shaders/sprite.vert.hlsl:vs
        sprite_frag:render/shaders/sprite.frag.hlsl:ps)
target_link_libraries(reinos_render
    PUBLIC reinos_render_core reinos_platform
    PRIVATE imgui::imgui spdlog::spdlog)
reinos_set_warnings(reinos_render)

# --- Ejecutable ----------------------------------------------------------------
add_executable(reinos
    game/main.cpp
    game/app.cpp)
target_link_libraries(reinos PRIVATE reinos_game_core reinos_render reinos_platform imgui::imgui spdlog::spdlog)
reinos_set_warnings(reinos)

# data/ se copia junto al ejecutable: el binario encuentra sus datos sin depender
# del directorio de trabajo.
add_custom_command(TARGET reinos POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${PROJECT_SOURCE_DIR}/data $<TARGET_FILE_DIR:reinos>/data
    COMMENT "Copiando data/ junto al ejecutable")
