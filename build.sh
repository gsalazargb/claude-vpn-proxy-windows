#!/bin/bash
# Compila claude_proxy.exe, prep_conf.exe y el instalador de forma
# reproducible: el mismo codigo con las mismas versiones de herramientas
# produce exactamente los mismos bytes, asi cualquiera puede recompilar y
# comparar el SHA-256 con el distribuido.
#
# Herramientas usadas para la version distribuida (ver docs/COMPILAR.md):
#   x86_64-w64-mingw32-gcc 16.2.1 (Fedora mingw64-gcc-16.2.1-1.fc44)
#   makensis 3.11 (Fedora mingw32-nsis-3.11-3.fc44)
set -euo pipefail
cd "$(dirname "$0")"

# -Wl,--no-insert-timestamp: no escribir la fecha de compilacion en la
# cabecera PE. -ffile-prefix-map: no filtrar la ruta local de compilacion.
CFLAGS="-Wall -Wextra -O2 -ffile-prefix-map=$PWD=. -Wl,--no-insert-timestamp"

# -mwindows: claude_proxy corre sin ventana de consola (los mensajes van a un log)
x86_64-w64-mingw32-gcc $CFLAGS -mwindows -o src/claude_proxy.exe src/claude_proxy.c -lws2_32 -liphlpapi
x86_64-w64-mingw32-gcc $CFLAGS -o src/prep_conf.exe src/prep_conf.c
(cd nsis && makensis -V2 installer.nsi)

sha256sum nsis/ClaudeVPNProxySetup.exe src/claude_proxy.exe src/prep_conf.exe nsis/launch-claude.cmd
