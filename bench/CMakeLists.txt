# Banco de rendimiento de la simulación (M2): 1000 y 2000 unidades, criterio de 50 ms
# por tick. Se ejecuta en CI en Release; en Debug los números no significan nada.
add_executable(reinos_bench reinos_bench.cpp)
target_link_libraries(reinos_bench PRIVATE reinos_sim reinos_game_core)
reinos_set_warnings(reinos_bench)

# Torneo entre perfiles de IA (M7): mide si un perfil gana a otro.
add_executable(reinos_ai_match reinos_ai_match.cpp)
target_link_libraries(reinos_ai_match PRIVATE reinos_sim reinos_game_core)
reinos_set_warnings(reinos_ai_match)

# Hoja de arte (F1): todo el arte generado por código en una imagen, para revisarlo.
add_executable(reinos_art_sheet reinos_art_sheet.cpp)
target_link_libraries(reinos_art_sheet PRIVATE reinos_game_core reinos_render_core)
reinos_set_warnings(reinos_art_sheet)

# Sonidos en WAV (F2): cada receta y cada pieza, para oírlas sin abrir el juego.
add_executable(reinos_sound_wav reinos_sound_wav.cpp)
target_link_libraries(reinos_sound_wav PRIVATE reinos_game_core reinos_audio)
reinos_set_warnings(reinos_sound_wav)

# Pruebas aleatorias de órdenes largas (G2): varias semillas, partidas y capítulos.
add_executable(reinos_fuzz reinos_fuzz.cpp)
target_link_libraries(reinos_fuzz PRIVATE reinos_sim reinos_game_core)
reinos_set_warnings(reinos_fuzz)

# Inicios de partida: qué rodea a cada inicio en cada mapa (equidad de los mapas).
add_executable(reinos_start_report reinos_start_report.cpp)
target_link_libraries(reinos_start_report PRIVATE reinos_sim reinos_game_core)
reinos_set_warnings(reinos_start_report)
