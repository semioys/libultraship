# Nintendo Switch (devkitA64 + libnx) dependencies.
#
# Everything that devkitPro ships as a portlib is taken from
# $DEVKITPRO/portlibs/switch; the rest (nlohmann_json, tinyxml2, spdlog,
# libzip) is fetched and built statically below.
#
# Rendering goes through GLES3 on Mesa/nouveau, the same path Android uses.

set(USE_OPENGLES ON CACHE BOOL "Enable GLES3" FORCE)
add_compile_definitions(USE_OPENGLES=1)

# newlib hides fileno/fseeko/*_unlocked under strict -std=c++20.
add_compile_definitions(_GNU_SOURCE)

# No dlopen and no TCC backend for aarch64-none-elf.
set(ENABLE_SCRIPTING OFF CACHE BOOL "" FORCE)
set(DISABLE_DLL_LOADER ON CACHE BOOL "" FORCE)

#=================== SDL2 ===================
# The portlib only ships libSDL2.a, so its config never creates SDL2::SDL2.
find_package(SDL2 REQUIRED CONFIG)
# SDL2_INCLUDE_DIRS also holds the portlibs root, which code including
# <SDL2/SDL.h> and <GLES3/gl3.h> needs.
if(NOT TARGET SDL2::SDL2)
    add_library(SDL2::SDL2 INTERFACE IMPORTED GLOBAL)
    set_target_properties(SDL2::SDL2 PROPERTIES INTERFACE_LINK_LIBRARIES SDL2::SDL2-static)
endif()
# The game links libultraship from the parent directory, so these must be global.
foreach(_sdl_target SDL2::SDL2 SDL2::SDL2-static)
    if(TARGET ${_sdl_target})
        get_target_property(_sdl_real ${_sdl_target} ALIASED_TARGET)
        if(NOT _sdl_real)
            set(_sdl_real ${_sdl_target})
        endif()
        get_target_property(_sdl_global ${_sdl_real} IMPORTED_GLOBAL)
        if(NOT _sdl_global)
            set_target_properties(${_sdl_real} PROPERTIES IMPORTED_GLOBAL TRUE)
        endif()
        set_property(TARGET ${_sdl_real} APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES "${SDL2_INCLUDE_DIRS}")
    endif()
endforeach()

#=================== GLES / EGL ===================
find_package(OpenGL REQUIRED CONFIG)
find_library(OPENGL_GLESv2_LIBRARY GLESv2 REQUIRED)
# Link order matters for static archives: GLESv2 -> EGL -> glapi -> drm_nouveau -> nx
set(LUS_SWITCH_GL_LIBRARIES ${OPENGL_GLESv2_LIBRARY} ${OPENGL_LIBRARIES})

#=================== zlib / bzip2 ===================
find_package(ZLIB REQUIRED)
find_package(BZip2 QUIET)

#=================== Fetched libraries ===================
include(FetchContent)

find_package(nlohmann_json QUIET)
if(NOT ${nlohmann_json_FOUND})
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.12.0
        GIT_SHALLOW TRUE
        OVERRIDE_FIND_PACKAGE
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

find_package(tinyxml2 QUIET)
if(NOT ${tinyxml2_FOUND})
    set(tinyxml2_BUILD_TESTING OFF)
    FetchContent_Declare(
        tinyxml2
        GIT_REPOSITORY https://github.com/leethomason/tinyxml2.git
        GIT_TAG 11.0.0
        GIT_SHALLOW TRUE
        OVERRIDE_FIND_PACKAGE
    )
    FetchContent_MakeAvailable(tinyxml2)
endif()

find_package(spdlog QUIET)
if(NOT ${spdlog_FOUND})
    FetchContent_Declare(
        spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG v1.16.0
        GIT_SHALLOW TRUE
        OVERRIDE_FIND_PACKAGE
    )
    FetchContent_MakeAvailable(spdlog)
endif()

find_package(libzip QUIET)
if(NOT ${libzip_FOUND})
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    set(BUILD_TOOLS OFF)
    set(BUILD_REGRESS OFF)
    set(BUILD_EXAMPLES OFF)
    set(BUILD_DOC OFF)
    set(BUILD_OSSFUZZ OFF)
    set(BUILD_SHARED_LIBS OFF)
    set(ENABLE_BZIP2 OFF)
    set(ENABLE_LZMA OFF)
    set(ENABLE_ZSTD OFF)
    set(ENABLE_COMMONCRYPTO OFF)
    set(ENABLE_GNUTLS OFF)
    set(ENABLE_MBEDTLS OFF)
    set(ENABLE_OPENSSL OFF)
    FetchContent_Declare(
        libzip
        GIT_REPOSITORY https://github.com/nih-at/libzip.git
        GIT_TAG v1.11.4
        GIT_SHALLOW TRUE
        OVERRIDE_FIND_PACKAGE
    )
    FetchContent_MakeAvailable(libzip)
    list(APPEND ADDITIONAL_LIB_INCLUDES ${libzip_SOURCE_DIR}/lib ${libzip_BINARY_DIR})
endif()

#=================== Threads ===================
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

#=================== prism ===================
# prism's CMakeLists overwrites CMAKE_CXX_FLAGS, dropping the toolchain's
# -D__SWITCH__/-march/-mtp=soft/-ftls-model. Without -mtp=soft, thread_local
# reads the wrong thread register on Horizon. Put them back on the target.
if(TARGET prism)
    separate_arguments(_lus_switch_cxx_flags UNIX_COMMAND "${CMAKE_CXX_FLAGS}")
    target_compile_options(prism PRIVATE ${_lus_switch_cxx_flags})
endif()

#=================== ImGui ===================
target_link_libraries(ImGui PUBLIC SDL2::SDL2 ${LUS_SWITCH_GL_LIBRARIES})
target_compile_definitions(ImGui PUBLIC
    IMGUI_IMPL_OPENGL_ES3
    # The default "open in shell" handler forks and execs; Horizon has neither.
    IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS
)
