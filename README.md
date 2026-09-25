# bpa_lua
Lua Integration for Betrock++ Addon System

## Compilation

One command, no build system required. You'll need Lua's headers/library
installed first (e.g. `apt install liblua5.4-dev`, `brew install lua`, or
MSYS2's `mingw-w64-x86_64-lua`).

### Linux

```bash
g++ -std=c++17 -shared -fPIC src/bpa_lua.cpp -o bpa_lua.so $(pkg-config --cflags --libs lua)
```

### macOS

```bash
g++ -std=c++17 -shared -fPIC -undefined dynamic_lookup src/bpa_lua.cpp -o bpa_lua.so $(pkg-config --cflags --libs lua)
```

### Windows (MSYS2 MinGW64 shell)

```bash
g++ -std=c++17 -shared src/bpa_lua.cpp -o bpa_lua.dll $(pkg-config --cflags --libs lua)
```