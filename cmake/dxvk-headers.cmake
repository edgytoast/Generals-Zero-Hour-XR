# Header-only checkout of the DXVK fork, for platforms that do NOT build DXVK.
#
# The engine compiles its D3D8 code against DXVK's Wine-style header set
# (include/native/{windows,directx,wsi}, include/vulkan). `d3d8.h` and
# `d3d8types.h` live in the fork's include/native/directx SUBMODULE
# (mingw-directx-headers), so a plain clone of the fork does not contain them.
# visionOS renders through the engine's own GLES3 D3D8 backend (on ANGLE), so it
# needs only these headers: no meson, no MoltenVK, no Vulkan SDK, no glslang.
#
#   gx_fetch_dxvk_headers(<out-var-with-source-dir> <commit-sha>)
#
# Sets <out-var> to the directory holding the checkout, cloning it if needed.
# Location: $SAGE_DXVK_HEADERS_DIR if set, otherwise
# $ENV{GX_DEPS_ROOT}/src/fbraz3-dxvk when GX_DEPS_ROOT is exported (a checkout
# shared by the simulator and device builds), otherwise
# <build>/_deps/dxvk-headers-src. An existing checkout at exactly <sha> with the
# headers present is reused without touching the network. Any other state is
# (re)fetched: shallow, only the three header submodules.
# Runs `git -C <dir> <args...>` and aborts the configure with a pointer at the
# override when it fails.
function(_gx_dxvk_git dir)
    execute_process(COMMAND "${GX_GIT_EXECUTABLE}" -C "${dir}" ${ARGN}
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "DXVK headers: 'git ${ARGN}' failed in ${dir}:\n${_out}\n${_err}\n"
            "Set -DSAGE_DXVK_HEADERS_DIR=<checkout of https://github.com/fbraz3/dxvk with its "
            "include/native/directx, include/vulkan and include/spirv submodules> to use an existing one.")
    endif()
endfunction()

function(gx_fetch_dxvk_headers out_var sha)
    if(SAGE_DXVK_HEADERS_DIR)
        set(_dir "${SAGE_DXVK_HEADERS_DIR}")
    elseif(DEFINED ENV{GX_DEPS_ROOT} AND NOT "$ENV{GX_DEPS_ROOT}" STREQUAL "")
        set(_dir "$ENV{GX_DEPS_ROOT}/src/fbraz3-dxvk")
    else()
        set(_dir "${CMAKE_BINARY_DIR}/_deps/dxvk-headers-src")
    endif()

    find_program(GX_GIT_EXECUTABLE git REQUIRED)
    set(_marker "${_dir}/include/native/directx/d3d8.h")
    set(_have_sha "")
    if(EXISTS "${_dir}/.git")
        execute_process(COMMAND "${GX_GIT_EXECUTABLE}" -C "${_dir}" rev-parse HEAD
            OUTPUT_VARIABLE _have_sha OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    endif()

    if(NOT _have_sha STREQUAL "${sha}" OR NOT EXISTS "${_marker}")
        message(STATUS "DXVK headers: fetching fork commit ${sha} into ${_dir}")
        file(MAKE_DIRECTORY "${_dir}")
        if(NOT EXISTS "${_dir}/.git")
            _gx_dxvk_git("${_dir}" init)
            _gx_dxvk_git("${_dir}" remote add origin https://github.com/fbraz3/dxvk.git)
        endif()
        _gx_dxvk_git("${_dir}" fetch --depth 1 origin "${sha}")
        _gx_dxvk_git("${_dir}" checkout --force FETCH_HEAD)
        _gx_dxvk_git("${_dir}" submodule update --init --depth 1
            include/native/directx include/vulkan include/spirv)
        if(NOT EXISTS "${_marker}")
            message(FATAL_ERROR "DXVK headers: ${_marker} is missing after the checkout")
        endif()
    else()
        message(STATUS "DXVK headers: using existing checkout at ${_dir} (${sha})")
    endif()
    set(${out_var} "${_dir}" PARENT_SCOPE)
endfunction()
