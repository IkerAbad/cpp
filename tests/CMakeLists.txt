find_package(doctest CONFIG REQUIRED)

add_executable(rts_tests
    test_main.cpp
    test_ai.cpp
    test_ai_modules.cpp
    test_combat.cpp
    test_config.cpp
    test_economy.cpp
    test_fire.cpp
    test_supply.cpp
    test_medicine.cpp
    test_morale.cpp
    test_terrain_combat.cpp
    test_fatigue.cpp
    test_fortifications.cpp
    test_formation.cpp
    test_upgrades.cpp
    test_market.cpp
    test_roads.cpp
    test_vision.cpp
    test_orders.cpp
    test_minimap.cpp
    test_alerts.cpp
    test_target_priority.cpp
    test_fixed.cpp
    test_fixed_step.cpp
    test_map_gen.cpp
    test_movement.cpp
    test_path.cpp
    test_projection.cpp
    test_replay.cpp
    test_rng.cpp
    test_scene.cpp
    test_selection.cpp
    test_sim_regression.cpp)
target_link_libraries(rts_tests PRIVATE rts_game_core rts_render_core rts_sim doctest::doctest)
target_compile_definitions(rts_tests PRIVATE RTS_DATA_DIR="${PROJECT_SOURCE_DIR}/data")
rts_set_warnings(rts_tests)

add_test(NAME unit COMMAND rts_tests)

# Regla dura: src/sim/ sin coma flotante, sin reloj real, sin E/S, sin SDL.
add_test(NAME sim_purity
    COMMAND ${CMAKE_COMMAND} -DSIM_DIR=${PROJECT_SOURCE_DIR}/src/sim
            -P ${PROJECT_SOURCE_DIR}/cmake/CheckSimPurity.cmake)

# Humo: el ejecutable arranca, lee data/ y simula un minuto de juego sin ventana.
add_test(NAME headless_smoke COMMAND rts --headless --ticks 1200)
