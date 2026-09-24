# Barrera de la regla 2 (determinismo) y la regla 1 (separación sim/presentación).
# Ningún compilador ofrece una opción portable para prohibir el float, así que se
# rechaza por texto. Se ejecuta como prueba de CTest: cmake -DSIM_DIR=... -P este_fichero
#
# Tokens prohibidos en src/sim/:
#   float, double, long double      -> no deterministas entre compiladores y CPU
#   <cmath>, <chrono>, <random>     -> matemática en coma flotante, reloj real, RNG del sistema
#   unordered_map, unordered_set    -> el orden de iteración cambia entre libstdc++, libc++ y MSVC STL
#   std::rand, srand, time(         -> RNG y reloj globales
#   SDL, imgui, <thread>, <fstream>, <iostream>, <filesystem> -> E/S o plataforma
if(NOT DEFINED SIM_DIR)
    message(FATAL_ERROR "Falta -DSIM_DIR=<ruta a src/sim>")
endif()

set(forbidden_patterns
    "(^|[^A-Za-z0-9_])float([^A-Za-z0-9_]|$)"
    "(^|[^A-Za-z0-9_])double([^A-Za-z0-9_]|$)"
    "#include[ \t]*<cmath>"
    "#include[ \t]*<math\\.h>"
    "#include[ \t]*<chrono>"
    "#include[ \t]*<random>"
    "#include[ \t]*<thread>"
    "#include[ \t]*<fstream>"
    "#include[ \t]*<iostream>"
    "#include[ \t]*<filesystem>"
    "unordered_(map|set|multimap|multiset)"
    "std::rand[^a-z_]"
    "(^|[^A-Za-z0-9_])srand[ \t]*\\("
    "(^|[^A-Za-z0-9_:])time[ \t]*\\("
    "SDL"
    "imgui"
)

file(GLOB_RECURSE sim_files "${SIM_DIR}/*.hpp" "${SIM_DIR}/*.cpp")
set(violations 0)
foreach(file IN LISTS sim_files)
    # Las listas de CMake se rompen con ';' y con corchetes desparejados; se neutralizan
    # antes de partir por líneas. Ningún patrón depende de esos caracteres.
    file(READ "${file}" content)
    string(REPLACE ";" "," content "${content}")
    string(REPLACE "[" "(" content "${content}")
    string(REPLACE "]" ")" content "${content}")
    string(REPLACE "\\" "/" content "${content}")
    string(REPLACE "\n" ";" lines "${content}")
    set(line_no 0)
    foreach(line IN LISTS lines)
        math(EXPR line_no "${line_no} + 1")
        # Se ignoran los comentarios de línea completos para poder documentar la prohibición.
        if(line MATCHES "^[ \t]*//")
            continue()
        endif()
        foreach(pattern IN LISTS forbidden_patterns)
            if(line MATCHES "${pattern}")
                message(SEND_ERROR "${file}:${line_no}: token prohibido en la simulación (${pattern}): ${line}")
                math(EXPR violations "${violations} + 1")
            endif()
        endforeach()
    endforeach()
endforeach()

list(LENGTH sim_files file_count)
if(violations GREATER 0)
    message(FATAL_ERROR "src/sim/: ${violations} violaciones en ${file_count} ficheros")
endif()
message(STATUS "src/sim/ limpio: ${file_count} ficheros revisados")
