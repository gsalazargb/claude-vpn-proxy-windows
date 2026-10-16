# Cómo compilar y verificar el binario

La compilación es reproducible: el mismo código fuente con las mismas versiones de herramientas produce exactamente los mismos bytes. Así cualquiera puede recompilar y confirmar que el SHA-256 coincide con el del instalador distribuido.

## Herramientas (versiones exactas usadas)

- MinGW-w64 GCC 16.2.1. En Fedora 44: `mingw64-gcc-16.2.1-1.fc44`.
- NSIS 3.11. En Fedora 44: `mingw32-nsis-3.11-3.fc44`.

Con otras versiones el programa funciona igual, pero los bytes pueden cambiar y el hash no va a coincidir.

## Compilar

Desde la raíz del repositorio (o la carpeta `fuente/` del zip):

```
./build.sh
```

Lo que hace `build.sh`:

```
CFLAGS="-Wall -Wextra -O2 -ffile-prefix-map=$PWD=. -Wl,--no-insert-timestamp"
x86_64-w64-mingw32-gcc $CFLAGS -o src/claude_proxy.exe src/claude_proxy.c -lws2_32 -liphlpapi
x86_64-w64-mingw32-gcc $CFLAGS -o src/prep_conf.exe src/prep_conf.c
cd nsis && makensis installer.nsi
```

`-Wl,--no-insert-timestamp` evita que la fecha de compilación quede en el ejecutable, `-ffile-prefix-map` evita que quede la ruta local, y `SetDateSave off` en `installer.nsi` evita que el instalador guarde la fecha de los archivos. Esas tres cosas son las que hacen que el resultado no dependa de quién, dónde ni cuándo se compile.

Al terminar, `build.sh` imprime los SHA-256. Deben coincidir con `SHA256SUMS.txt`.
