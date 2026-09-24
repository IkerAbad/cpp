# Compilación de shaders HLSL con dxc (port directx-dxc de vcpkg) y empaquetado en
# el ejecutable. Un único fuente HLSL genera:
#   SPIR-V para Vulkan (Linux y Windows)
#   DXIL   para Direct3D 12 (solo Windows; en Windows dxc firma el DXIL con dxil.dll)
#
# rts_add_shaders(<target> NAMESPACE <ns> SHADERS <nombre>:<fichero.hlsl>:<vs|ps> ...)
# Genera funciones <ns>::<nombre>_spirv() y <ns>::<nombre>_dxil() que devuelven
# std::span<const std::uint8_t> (vacío si el formato no existe en la plataforma).

find_package(directx-dxc CONFIG REQUIRED)
if(NOT DIRECTX_DXC_TOOL OR NOT EXISTS "${DIRECTX_DXC_TOOL}")
    message(FATAL_ERROR "No se encuentra dxc (DIRECTX_DXC_TOOL='${DIRECTX_DXC_TOOL}')")
endif()

# En Linux, vcpkg instala dxc en tools/directx-dxc/ y libdxcompiler.so en lib/, pero el
# RPATH del binario es $ORIGIN/../lib (= tools/lib): sin ayuda no arranca. Se lanza
# con LD_LIBRARY_PATH apuntando al directorio real de la biblioteca importada.
set(RTS_DXC_COMMAND "${DIRECTX_DXC_TOOL}")
if(NOT WIN32)
    get_target_property(_dxc_lib Microsoft::DirectXShaderCompiler IMPORTED_LOCATION_RELEASE)
    get_filename_component(_dxc_lib_dir "${_dxc_lib}" DIRECTORY)
    set(RTS_DXC_COMMAND "${CMAKE_COMMAND}" -E env "LD_LIBRARY_PATH=${_dxc_lib_dir}" "${DIRECTX_DXC_TOOL}")
endif()

function(rts_add_shaders target)
    cmake_parse_arguments(ARG "" "NAMESPACE" "SHADERS" ${ARGN})
    set(out_dir "${CMAKE_CURRENT_BINARY_DIR}/shaders")
    file(MAKE_DIRECTORY "${out_dir}")

    set(entries "")
    set(blobs "")
    foreach(spec IN LISTS ARG_SHADERS)
        string(REPLACE ":" ";" parts "${spec}")
        list(GET parts 0 name)
        list(GET parts 1 source)
        list(GET parts 2 stage)
        set(source "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
        set(profile "${stage}_6_0")

        # -WX: un aviso de dxc rompe el build, igual que en C++.
        set(spirv "${out_dir}/${name}.spv")
        add_custom_command(
            OUTPUT "${spirv}"
            COMMAND ${RTS_DXC_COMMAND} -nologo -WX -T ${profile} -E main -spirv -fspv-target-env=vulkan1.0
                    -Fo "${spirv}" "${source}"
            DEPENDS "${source}"
            COMMENT "dxc ${name} -> SPIR-V"
            VERBATIM)
        list(APPEND blobs "${spirv}")
        list(APPEND entries "${name}_spirv=${spirv}")

        if(WIN32)
            set(dxil "${out_dir}/${name}.dxil")
            # Los atributos [[vk::...]] solo aplican a SPIR-V; en DXIL se ignoran a propósito.
            add_custom_command(
                OUTPUT "${dxil}"
                COMMAND ${RTS_DXC_COMMAND} -nologo -WX -Wno-ignored-attributes -T ${profile} -E main
                        -Fo "${dxil}" "${source}"
                DEPENDS "${source}"
                COMMENT "dxc ${name} -> DXIL"
                VERBATIM)
            list(APPEND blobs "${dxil}")
            list(APPEND entries "${name}_dxil=${dxil}")
        else()
            list(APPEND entries "${name}_dxil=")
        endif()
    endforeach()

    string(REPLACE ";" "|" entries_arg "${entries}")
    set(generated "${out_dir}/embedded_shaders.cpp")
    add_custom_command(
        OUTPUT "${generated}"
        COMMAND "${CMAKE_COMMAND}" "-DOUTPUT=${generated}" "-DNAMESPACE=${ARG_NAMESPACE}" "-DENTRIES=${entries_arg}"
                -P "${PROJECT_SOURCE_DIR}/cmake/EmbedFiles.cmake"
        DEPENDS ${blobs} "${PROJECT_SOURCE_DIR}/cmake/EmbedFiles.cmake"
        COMMENT "Empaquetando shaders en ${target}"
        VERBATIM)
    target_sources(${target} PRIVATE "${generated}")
endfunction()
