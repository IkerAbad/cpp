# Grafo de dependencias. Las flechas solo van hacia abajo:
#
#   rts (ejecutable)
#    ├── rts_render ── rts_platform ── SDL3
#    │        └─────── imgui
#    ├── rts_game_core ── toml++
#    └── rts_sim ── EnTT            <- sin SDL, sin E/S, sin coma flotante
#
# rts_sim no enlaza SDL: un #include <SDL3/...> en src/sim/ no compila.

# --- Simulación pura -----------------------------------------------------------
add_library(rts_sim STATIC
    sim/world.cpp)
target_include_directories(rts_sim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(rts_sim PUBLIC EnTT::EnTT)
rts_set_warnings(rts_sim)

# --- Núcleo de juego sin plataforma (paso fijo, configuración) -----------------
add_library(rts_game_core STATIC
    game/fixed_step.cpp
    game/config.cpp)
target_link_libraries(rts_game_core
    PUBLIC rts_sim
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

# --- Presentación --------------------------------------------------------------
add_library(rts_render STATIC
    render/renderer.cpp)
target_link_libraries(rts_render
    PUBLIC rts_sim rts_platform
    PRIVATE imgui::imgui)
rts_set_warnings(rts_render)

# --- Ejecutable ----------------------------------------------------------------
add_executable(rts
    game/main.cpp
    game/app.cpp)
target_link_libraries(rts PRIVATE rts_game_core rts_render rts_platform spdlog::spdlog)
rts_set_warnings(rts)

# data/ se copia junto al ejecutable: el binario encuentra su configuración sin
# depender del directorio de trabajo.
add_custom_command(TARGET rts POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${PROJECT_SOURCE_DIR}/data $<TARGET_FILE_DIR:rts>/data
    COMMENT "Copiando data/ junto al ejecutable")
