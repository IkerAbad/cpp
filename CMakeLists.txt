cmake_minimum_required(VERSION 3.28)

option(REINOS_WERROR "Warnings como errores" ON)
option(REINOS_TRACY "Instrumentación con Tracy" OFF)
option(REINOS_BUILD_TESTS "Compilar las pruebas" ON)
option(REINOS_BUILD_BENCH "Compilar el banco de rendimiento" ON)
# OFF: solo el núcleo (simulación, sonido, red, configuración), las pruebas y los
# bancos, sin SDL3, imgui, spdlog ni dxc. Sirve para torneos y pruebas en máquinas
# sin entorno gráfico o con EnTT, toml++ y doctest del sistema, sin vcpkg.
option(REINOS_BUILD_GAME "Compilar el ejecutable con ventana (SDL3, imgui, shaders)" ON)

# Las features del manifiesto deben fijarse antes de project(), que es cuando
# el toolchain de vcpkg instala las dependencias.
if(REINOS_TRACY)
    list(APPEND VCPKG_MANIFEST_FEATURES "tracy")
endif()

project(reinos
    VERSION 0.0.1
    DESCRIPTION "Reinos: estrategia medieval en tiempo real con capa de logística (nombre provisional)"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
    message(FATAL_ERROR "Compila fuera del árbol de fuentes: cmake --preset <nombre>")
endif()

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(CompilerWarnings)

if(REINOS_BUILD_GAME)
    find_package(SDL3 CONFIG REQUIRED)
    # SDL3Config.cmake marca SDL3::Headers con SYSTEM 0, así que su include llega como -I
    # normal y nuestros warnings (-Wold-style-cast en clang-cl, por ejemplo) saltan dentro
    # de las cabeceras de SDL. Se devuelve a SYSTEM, como el resto de dependencias.
    if(TARGET SDL3::Headers)
        set_property(TARGET SDL3::Headers PROPERTY SYSTEM 1)
    endif()
    find_package(imgui CONFIG REQUIRED)
    find_package(spdlog CONFIG REQUIRED)
endif()
find_package(EnTT CONFIG REQUIRED)
find_package(tomlplusplus CONFIG REQUIRED)
if(REINOS_TRACY)
    find_package(Tracy CONFIG REQUIRED)
endif()

add_subdirectory(src)

if(REINOS_BUILD_BENCH)
    add_subdirectory(bench)
endif()

if(REINOS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
