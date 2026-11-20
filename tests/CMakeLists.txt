find_package(doctest CONFIG REQUIRED)

add_executable(reinos_tests
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
    test_lockstep.cpp
    test_art.cpp
    test_audio.cpp
    test_campaign.cpp
    test_fuzz.cpp
    test_i18n.cpp
    test_scenario.cpp
    test_rng.cpp
    test_scene.cpp
    test_season.cpp
    test_selection.cpp
    test_sim_regression.cpp)
target_link_libraries(reinos_tests PRIVATE reinos_game_core reinos_render_core reinos_sim doctest::doctest)
target_compile_definitions(reinos_tests PRIVATE REINOS_DATA_DIR="${PROJECT_SOURCE_DIR}/data"
                                            REINOS_SOURCE_DIR="${PROJECT_SOURCE_DIR}/src")
reinos_set_warnings(reinos_tests)

add_test(NAME unit COMMAND reinos_tests)

# Regla dura: src/sim/ sin coma flotante, sin reloj real, sin E/S, sin SDL.
add_test(NAME sim_purity
    COMMAND ${CMAKE_COMMAND} -DSIM_DIR=${PROJECT_SOURCE_DIR}/src/sim
            -P ${PROJECT_SOURCE_DIR}/cmake/CheckSimPurity.cmake)

# Las dos últimas necesitan el ejecutable con ventana (REINOS_BUILD_GAME).
if(NOT REINOS_BUILD_GAME)
    return()
endif()

# Humo: el ejecutable arranca, lee data/ y simula un minuto de juego sin ventana.
add_test(NAME headless_smoke COMMAND reinos --headless --ticks 1200)

# Informe de errores (G3): una caída provocada deja repetición, registro e informe.
add_test(NAME crash_report
    COMMAND ${CMAKE_COMMAND} -DREINOS=$<TARGET_FILE:reinos> -DDATA=${PROJECT_SOURCE_DIR}/data
            -DREPORT_DIR=${CMAKE_CURRENT_BINARY_DIR}/informes_prueba
            -P ${PROJECT_SOURCE_DIR}/cmake/CheckCrashReport.cmake)
