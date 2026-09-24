# bpa_lua
Lua Integration for Betrock++ Addon System

## Compilation

```bash
g++ -shared -fPIC bpa_lua.cpp -o bpa_lua.so $(pkg-config --cflags --libs lua5.4)
```