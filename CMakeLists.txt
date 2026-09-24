cmake_minimum_required(VERSION 3.28)

option(RTS_WERROR "Warnings como errores" ON)
option(RTS_TRACY "Instrumentación con Tracy" OFF)
option(RTS_BUILD_TESTS "Compilar las pruebas" ON)

# Las features del manifiesto deben fijarse antes de project(), que es cuando
# el toolchain de vcpkg instala las dependencias.
if(RTS_TRACY)
    list(APPEND VCPKG_MANIFEST_FEATURES "tracy")
endif()

project(rts
    VERSION 0.0.1
    DESCRIPTION "RTS medieval con capa de logística (nombre provisional)"
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

find_package(SDL3 CONFIG REQUIRED)
# SDL3Config.cmake marca SDL3::Headers con SYSTEM 0, así que su include llega como -I
# normal y nuestros warnings (-Wold-style-cast en clang-cl, por ejemplo) saltan dentro
# de las cabeceras de SDL. Se devuelve a SYSTEM, como el resto de dependencias.
if(TARGET SDL3::Headers)
    set_property(TARGET SDL3::Headers PROPERTY SYSTEM 1)
endif()
find_package(imgui CONFIG REQUIRED)
find_package(EnTT CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(tomlplusplus CONFIG REQUIRED)
if(RTS_TRACY)
    find_package(Tracy CONFIG REQUIRED)
endif()

add_subdirectory(src)

if(RTS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
