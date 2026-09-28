# =============================================================================
# PyroWave codec
#
# PyroWave is a C++ Vulkan library, not FFmpeg, so it is built separately from
# the usual dependency set. It lives outside the repo and is expected at
# PYROWAVE_SOURCE_DIR (an out-of-tree checkout, matching how the CMake build
# treats third-party code that is not vendored).
#
# Only the wrapper translation unit needs the pyrowave headers; the rest of the
# host talks to src/pyrowave_codec.h, which hides them (AGENTS.md rule 6).
# =============================================================================

set(PYROWAVE_SOURCE_DIR "" CACHE PATH
        "Path to a PyroWave source checkout (the directory containing pyrowave.h)")

if(NOT PYROWAVE_SOURCE_DIR)
    # Fall back to the workspace layout used during development, where the codec
    # is cloned as a sibling of this repository.
    set(_pyrowave_guess "${CMAKE_SOURCE_DIR}/../PyroWave")
    if(EXISTS "${_pyrowave_guess}/pyrowave.h")
        set(PYROWAVE_SOURCE_DIR "${_pyrowave_guess}" CACHE PATH "" FORCE)
        message(STATUS "PyroWave: using ${PYROWAVE_SOURCE_DIR}")
    endif()
endif()

if(PYROWAVE_SOURCE_DIR AND WIN32)
    set(PYROWAVE_ENABLED ON)

    # Build dir: reuse the MinGW build when the compiler matches, otherwise build
    # a dedicated one. Apollo is a MinGW/ucrt64 project, so that is the default
    # (AGENTS.md: Apollo is CMake + MinGW, never MSVC).
    if(NOT PYROWAVE_BUILD_DIR)
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            set(PYROWAVE_BUILD_DIR "${PYROWAVE_SOURCE_DIR}/build")
        else()
            set(PYROWAVE_BUILD_DIR "${PYROWAVE_SOURCE_DIR}/build-${CMAKE_CXX_COMPILER_ID}")
        endif()
    endif()

    if(NOT EXISTS "${PYROWAVE_BUILD_DIR}")
        message(STATUS "PyroWave: configuring ${PYROWAVE_BUILD_DIR}")
        file(MAKE_DIRECTORY "${PYROWAVE_BUILD_DIR}")
        execute_process(
                COMMAND "${CMAKE_COMMAND}"
                -S "${PYROWAVE_SOURCE_DIR}"
                -B "${PYROWAVE_BUILD_DIR}"
                -G "${CMAKE_GENERATOR}"
                -DCMAKE_BUILD_TYPE=Release
                -DBUILD_TESTS=OFF
                -DBUILD_SHARED_LIBS=OFF
                -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
                -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
                RESULT_VARIABLE _pyrowave_cfg_result
                OUTPUT_VARIABLE _pyrowave_cfg_out
                ERROR_VARIABLE _pyrowave_cfg_err)
        if(NOT _pyrowave_cfg_result EQUAL 0)
            message(FATAL_ERROR
                    "PyroWave configure failed (${_pyrowave_cfg_result}):\n${_pyrowave_cfg_out}\n${_pyrowave_cfg_err}")
        endif()
    endif()

    # An imported interface target. The C API (pyrowave.h) is implemented in the
    # codec's `pyrowave-shared` target, NOT in its `pyrowave` static library: the
    # static lib is the C++ core and exports no pyrowave_* symbols at all, so
    # linking it gives "undefined reference" on every entry point. This links the
    # import library for the shared build instead, which means
    # libpyrowave-shared-0.dll has to ship next to sunshine.exe.
    add_library(pyrowave_static INTERFACE IMPORTED GLOBAL)

    # The Vulkan headers come from the PyroWave checkout's Granite submodule, which
    # is where the codec's own build gets them from. There is no Vulkan SDK
    # dependency on this machine, and duplicating the headers into Apollo would
    # be a second source of truth for the same ABI.
    set(_pyrowave_vulkan_include "${PYROWAVE_SOURCE_DIR}/Granite/third_party/khronos/vulkan-headers/include")

    if(NOT EXISTS "${_pyrowave_vulkan_include}/vulkan/vulkan.h")
        message(FATAL_ERROR
                "PyroWave: Vulkan headers not found at ${_pyrowave_vulkan_include}. "
                "Run checkout_granite.sh in the PyroWave checkout (or set PYROWAVE_VULKAN_INCLUDE).")
    endif()

    # The C API (pyrowave.h) lives in the `pyrowave-shared` target, not in the
    # `pyrowave` static library: the static lib is the C++ core and exports no
    # pyrowave_* symbols at all, so linking it produces "undefined reference" on
    # every entry point. This links the import library for the shared build
    # instead, which means libpyrowave-shared-0.dll must ship next to
    # sunshine.exe.
    if(NOT EXISTS "${PYROWAVE_BUILD_DIR}/libpyrowave-shared.dll.a")
        message(STATUS "PyroWave: building ${PYROWAVE_BUILD_DIR}")
        execute_process(
                COMMAND "${CMAKE_COMMAND}" --build "${PYROWAVE_BUILD_DIR}" --config Release
                --target pyrowave-shared
                RESULT_VARIABLE _pyrowave_build_result
                OUTPUT_VARIABLE _pyrowave_build_out
                ERROR_VARIABLE _pyrowave_build_err)
        if(NOT _pyrowave_build_result EQUAL 0)
            message(FATAL_ERROR
                    "PyroWave build failed (${_pyrowave_build_result}):\n${_pyrowave_build_out}\n${_pyrowave_build_err}")
        endif()
    endif()

    set(PYROWAVE_DLL "${PYROWAVE_BUILD_DIR}/libpyrowave-shared-0.dll")

    set_target_properties(pyrowave_static PROPERTIES
            IMPORTED_LOCATION "${PYROWAVE_BUILD_DIR}/libpyrowave-shared.dll.a"
            INTERFACE_INCLUDE_DIRECTORIES
            "${PYROWAVE_SOURCE_DIR};${PYROWAVE_SOURCE_DIR}/eval-results;${_pyrowave_vulkan_include}")

    # The codec DLL also needs the Vulkan loader, but neither the MinGW toolchain
    # nor this machine's Windows SDK ships vulkan-1.lib. The loader ships with
    # every GPU driver, and it is a dependency of the codec DLL rather than of the
    # host executable, so it is resolved at run time without any link-time
    # involvement here.
    set(PYROWAVE_LINK_LIBRARIES "${PYROWAVE_BUILD_DIR}/libpyrowave-shared.dll.a")

elseif(PYROWAVE_SOURCE_DIR)
    message(STATUS "PyroWave: source found but only the Windows implementation exists; skipping")
else()
    message(STATUS "PyroWave: not configured (set PYROWAVE_SOURCE_DIR to enable)")
endif()
