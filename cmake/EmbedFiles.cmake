# Genera un .cpp con el contenido binario de varios ficheros como arrays de bytes.
# Uso: cmake -DOUTPUT=<fichero.cpp> -DNAMESPACE=<ns> -DENTRIES=<nombre>=<ruta>|<nombre>=<ruta>... -P EmbedFiles.cmake
# Las entradas van separadas por '|' porque ';' se parte al pasar por add_custom_command.
# Una ruta vacía genera un array vacío (formato de shader no disponible en esta plataforma).

if(NOT DEFINED OUTPUT OR NOT DEFINED NAMESPACE OR NOT DEFINED ENTRIES)
    message(FATAL_ERROR "EmbedFiles.cmake: faltan OUTPUT, NAMESPACE o ENTRIES")
endif()

string(REPLACE "|" ";" entries "${ENTRIES}")
set(body "// Generado por cmake/EmbedFiles.cmake. No editar.\n#include <cstddef>\n#include <cstdint>\n#include <span>\n\nnamespace ${NAMESPACE} {\n\n")
foreach(entry IN LISTS entries)
    string(FIND "${entry}" "=" eq)
    string(SUBSTRING "${entry}" 0 ${eq} name)
    math(EXPR path_start "${eq} + 1")
    string(SUBSTRING "${entry}" ${path_start} -1 path)
    if(path STREQUAL "")
        string(APPEND body "std::span<const std::uint8_t> ${name}() { return {}; }\n\n")
        continue()
    endif()
    file(READ "${path}" hex HEX)
    string(LENGTH "${hex}" hex_len)
    math(EXPR size "${hex_len} / 2")
    # 16 bytes por línea: "0xab, " x16
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1, " bytes "${hex}")
    string(REGEX REPLACE "((0x[0-9a-f][0-9a-f], ){16})" "\\1\n    " bytes "${bytes}")
    string(APPEND body "namespace {\nconst std::uint8_t k_${name}[${size}] = {\n    ${bytes}\n};\n}  // namespace\n")
    string(APPEND body "std::span<const std::uint8_t> ${name}() { return k_${name}; }\n\n")
endforeach()
string(APPEND body "}  // namespace ${NAMESPACE}\n")

# Solo se reescribe si cambia, para no forzar recompilaciones.
if(EXISTS "${OUTPUT}")
    file(READ "${OUTPUT}" previous)
    if(previous STREQUAL body)
        return()
    endif()
endif()
file(WRITE "${OUTPUT}" "${body}")
