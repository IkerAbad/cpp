# Banco de rendimiento de la simulación (M2): 1000 y 2000 unidades, criterio de 50 ms
# por tick. Se ejecuta en CI en Release; en Debug los números no significan nada.
add_executable(rts_bench rts_bench.cpp)
target_link_libraries(rts_bench PRIVATE rts_sim rts_game_core)
rts_set_warnings(rts_bench)

# Torneo entre perfiles de IA (M7): mide si un perfil gana a otro.
add_executable(rts_ai_match rts_ai_match.cpp)
target_link_libraries(rts_ai_match PRIVATE rts_sim rts_game_core)
rts_set_warnings(rts_ai_match)

# Hoja de arte (F1): todo el arte generado por código en una imagen, para revisarlo.
add_executable(rts_art_sheet rts_art_sheet.cpp)
target_link_libraries(rts_art_sheet PRIVATE rts_game_core rts_render_core)
rts_set_warnings(rts_art_sheet)
