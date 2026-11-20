# Informe de errores (G3): una caída provocada (--crash-at) debe dejar en REPORT_DIR un
# informe con informe.txt, reinos.log y partida.rep, y la repetición debe reproducirse
# hasta el tick de la caída. Uso: cmake -DREINOS=<reinos> -DDATA=<data> -DREPORT_DIR=<dir>
# -P CheckCrashReport.cmake
file(REMOVE_RECURSE "${REPORT_DIR}")
execute_process(
    COMMAND "${REINOS}" --data "${DATA}" --headless --ticks 400 --record "${REPORT_DIR}/sin_caida.rep"
            --crash-at 200 --report-dir "${REPORT_DIR}"
    RESULT_VARIABLE crash_result)
if(crash_result EQUAL 0)
    message(FATAL_ERROR "la caída provocada no hizo fallar a reinos")
endif()
file(GLOB reports "${REPORT_DIR}/informe-*")
list(LENGTH reports count)
if(NOT count EQUAL 1)
    message(FATAL_ERROR "se esperaba un informe en ${REPORT_DIR} y hay ${count}")
endif()
foreach(f informe.txt reinos.log partida.rep)
    if(NOT EXISTS "${reports}/${f}")
        message(FATAL_ERROR "falta ${f} en el informe")
    endif()
endforeach()
file(READ "${reports}/informe.txt" text)
if(NOT text MATCHES "Tick: 200")
    message(FATAL_ERROR "el informe no dice el tick de la caída:\n${text}")
endif()
execute_process(COMMAND "${REINOS}" --verify-replay "${reports}/partida.rep" RESULT_VARIABLE verify_result)
if(NOT verify_result EQUAL 0)
    message(FATAL_ERROR "la repetición del informe no se reproduce")
endif()
