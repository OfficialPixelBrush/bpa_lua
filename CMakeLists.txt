cmake_minimum_required(VERSION 3.16)
project(bpa_lua LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Lua is built from source and linked statically into the addon, so there is
# no runtime dependency on it. To use a local checkout instead of downloading:
#   cmake -B build -DFETCHCONTENT_SOURCE_DIR_LUA=/path/to/lua
include(FetchContent)
FetchContent_Declare(
    lua
    GIT_REPOSITORY https://github.com/lua/lua.git
    GIT_TAG        v5.4.8
    GIT_SHALLOW    TRUE
)
# The upstream repo ships no CMakeLists.txt, so this only downloads the sources.
FetchContent_MakeAvailable(lua)

# Everything in the core + standard libraries; lua.c (the CLI) and luac.c are left out.
set(LUA_SOURCES
    lapi.c lcode.c lctype.c ldebug.c ldo.c ldump.c lfunc.c lgc.c llex.c lmem.c
    lobject.c lopcodes.c lparser.c lstate.c lstring.c ltable.c ltm.c lundump.c
    lvm.c lzio.c
    lauxlib.c lbaselib.c lcorolib.c ldblib.c linit.c liolib.c lmathlib.c
    loadlib.c loslib.c lstrlib.c ltablib.c lutf8lib.c
)
list(TRANSFORM LUA_SOURCES PREPEND "${lua_SOURCE_DIR}/")

add_library(lua_core STATIC ${LUA_SOURCES})
target_include_directories(lua_core PUBLIC "${lua_SOURCE_DIR}")
# Keep Lua's symbols private to the addon so they can't clash with the host or other addons.
set_target_properties(lua_core PROPERTIES C_VISIBILITY_PRESET hidden)
if(UNIX)
    # io.popen, os.tmpname, etc. plus dlopen for package.loadlib / C modules.
    target_compile_definitions(lua_core PRIVATE LUA_USE_POSIX LUA_USE_DLOPEN)
    target_link_libraries(lua_core PUBLIC ${CMAKE_DL_LIBS})
endif()
if(MSVC)
    target_compile_definitions(lua_core PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_library(bpa_lua MODULE src/bpa_lua.cpp)
target_link_libraries(bpa_lua PRIVATE lua_core)
target_compile_options(bpa_lua PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-fvisibility=hidden>)
# bp_addon() is exported explicitly in the source.
set_target_properties(bpa_lua PROPERTIES PREFIX "")
