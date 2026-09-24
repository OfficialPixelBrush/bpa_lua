# bpa_lua
Lua Integration for Betrock++ Addon System

## Compilation

```bash
gcc -c -fPIC -Wall bpa_lua.c -o bpa_lua.o $(pkg-config --cflags lua5.4)
gcc -shared -fPIC bpa_lua.c -o bpa_lua.so $(pkg-config --cflags --libs lua5.4)
```