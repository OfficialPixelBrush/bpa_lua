cmake_minimum_required(VERSION 3.15)
project(bpa_lua CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Always build position-independent code, needed for a shared library on
# every platform (this is implicit on Windows, required on Linux/macOS).
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Put the built .so/.dll/.dylib next to the CMake build dir root instead of
# being buried in per-target subfolders, so CI can find it easily. Multi-config
# generators (Visual Studio) otherwise append a Debug/Release subdirectory, so
# pin those per-config variables too.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
foreach(_config Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER ${_config} _config_upper)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_config_upper} ${CMAKE_BINARY_DIR}/bin)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${_config_upper} ${CMAKE_BINARY_DIR}/bin)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${_config_upper} ${CMAKE_BINARY_DIR}/lib)
endforeach()

option(BPA_LUA_USE_SYSTEM_LUA
    "Link against a Lua already installed on this system instead of building one" OFF)
set(BPA_LUA_VERSION "5.4.6" CACHE STRING "Lua version to fetch/build when not using system Lua")

# ---------------------------------------------------------------------------
# Lua dependency
# ---------------------------------------------------------------------------
# Lua's own source tree ships no CMakeLists.txt, so when we're not linking a
# system-provided Lua we fetch the plain source release and compile it into a
# static library ourselves (see cmake/BundledLua.cmake). This keeps the build
# fully self-contained and reproducible in CI on Linux, macOS and Windows.
if(BPA_LUA_USE_SYSTEM_LUA)
    find_package(Lua REQUIRED)
    add_library(bpa_lua_lua_dep INTERFACE)
    target_include_directories(bpa_lua_lua_dep INTERFACE ${LUA_INCLUDE_DIR})
    target_link_libraries(bpa_lua_lua_dep INTERFACE ${LUA_LIBRARIES})
else()
    include(cmake/BundledLua.cmake)
    # Provides the "lua_static" target used below.
    add_library(bpa_lua_lua_dep ALIAS lua_static)
endif()

# ---------------------------------------------------------------------------
# bpa_lua addon module
# ---------------------------------------------------------------------------
add_library(bpa_lua MODULE
    src/bpa_lua.cpp
)

target_include_directories(bpa_lua PRIVATE src)
target_link_libraries(bpa_lua PRIVATE bpa_lua_lua_dep)

# Match the historical `g++ -shared -fPIC bpa_lua.cpp -o bpa_lua.so` naming:
# no "lib" prefix, and the platform's native module extension
# (.so on Linux, .dylib -> we force .so on macOS too since that's what most
# native addon loaders expect there, .dll on Windows).
set_target_properties(bpa_lua PROPERTIES
    PREFIX ""
    OUTPUT_NAME "bpa_lua"
)
if(APPLE)
    set_target_properties(bpa_lua PROPERTIES SUFFIX ".so")
endif()

if(MSVC)
    target_compile_options(bpa_lua PRIVATE /W4)
else()
    target_compile_options(bpa_lua PRIVATE -Wall -Wextra)
endif()

# std::filesystem needs an extra link library on some older GCC/libstdc++
# toolchains (GCC < 9). Harmless to add unconditionally when available.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS "9.0")
    target_link_libraries(bpa_lua PRIVATE stdc++fs)
endif()

install(TARGETS bpa_lua
    LIBRARY DESTINATION .
    RUNTIME DESTINATION .
)
