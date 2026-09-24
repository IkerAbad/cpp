# Banco de rendimiento de la simulación (M2): 1000 y 2000 unidades, criterio de 50 ms
# por tick. Se ejecuta en CI en Release; en Debug los números no significan nada.
add_executable(rts_bench rts_bench.cpp)
target_link_libraries(rts_bench PRIVATE rts_sim)
rts_set_warnings(rts_bench)
