# Warnings estrictos, aplicados solo a nuestros objetivos. Las cabeceras de vcpkg
# llegan como IMPORTED y CMake las trata como SYSTEM (-isystem, /external:I).
set(RTS_CLANG_GCC_WARNINGS
    -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast -Wnon-virtual-dtor
    -Woverloaded-virtual -Wnull-dereference -Wdouble-promotion -Wimplicit-fallthrough)

function(rts_set_warnings target)
    if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(${target} PRIVATE
            /W4 /permissive- /utf-8
            /w14242 /w14254 /w14263 /w14265 /w14287 /w14296 /w14311
            /w14545 /w14546 /w14547 /w14549 /w14555 /w14640 /w14826 /w14905
            /w14906 /w14928)
    elseif(MSVC)
        # clang-cl: /W4 equivale a -Wall -Wextra. Ojo: /Wall en clang-cl es -Weverything.
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 ${RTS_CLANG_GCC_WARNINGS})
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic ${RTS_CLANG_GCC_WARNINGS})
    endif()

    if(RTS_WERROR)
        if(MSVC)
            target_compile_options(${target} PRIVATE /WX)
        else()
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()
