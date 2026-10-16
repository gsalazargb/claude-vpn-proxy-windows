# Claude VPN Proxy: qué hace y cómo verificarlo

Este documento describe todo lo que el instalador y sus componentes hacen en el equipo, para que cualquier persona pueda confirmar que el software es benigno y que el binario corresponde a este código fuente.

## Qué hace, en una frase

Crea un túnel WireGuard con el `.conf` que el propio usuario proporciona, usando el cliente oficial de WireGuard, y un proxy local que solo escucha en `127.0.0.1:18080`. Claude Desktop se lanza apuntando a ese proxy, así que solo su tráfico sale por el túnel. El resto del equipo no se toca.

## Componentes

| Archivo | Tipo | Qué hace |
|---|---|---|
| `ClaudeVPNProxySetup.exe` | Instalador NSIS, requiere admin | Ejecuta los pasos de la sección "Cambios en el sistema". Fuente: `nsis/installer.nsi`. |
| `claude_proxy.exe` | Proxy HTTP, corre como el usuario y sin ventana | Escucha solo en `127.0.0.1`. Atiende `CONNECT host:puerto` (HTTPS) y HTTP plano, y abre la conexión de salida atada a la interfaz `ClaudeVPN` con `IP_UNICAST_IF`. No descifra, no inspecciona ni guarda tráfico. Su único archivo es un log de diagnóstico en `%LOCALAPPDATA%\ClaudeVPNProxy\claude_proxy.log` (destinos y errores de conexión, sin contenido), que se reinicia en cada arranque. Fuente: `src/claude_proxy.c` (587 líneas). |
| `prep_conf.exe` | Utilidad de texto | Copia el `.conf` agregando `Table = off` y valida que la clave privada no venga enmascarada. Solo lee un archivo y escribe otro. Fuente: `src/prep_conf.c` (158 líneas). |
| `launch-claude.cmd` | Script de texto | Arranca `claude_proxy.exe` si no está corriendo y lanza Claude Desktop con `--proxy-server=127.0.0.1:18080`. Son texto plano, se leen tal cual. |

## Cambios en el sistema

Instalación:

1. Verifica que el cliente oficial de WireGuard esté instalado. No lo descarga ni lo empaqueta.
2. Copia los tres archivos de arriba a `C:\Program Files\ClaudeVPNProxy\`.
3. Copia el `.conf` del usuario a `C:\Program Files\ClaudeVPNProxy\ClaudeVPN.conf` (con `Table = off`) y le quita los permisos heredados: solo Administradores y SYSTEM pueden leerlo, porque contiene la clave privada. Si no logra restringir los permisos, borra la copia y aborta.
4. Instala el túnel como servicio con `wireguard.exe /installtunnelservice`.
5. Agrega una ruta persistente `0.0.0.0/0` por la interfaz `ClaudeVPN` con métrica 9999, para que nunca gane frente a la ruta normal.
6. Crea accesos directos "Claude (VPN)" y "Desinstalar".

El proxy no arranca con Windows. Lo levanta el acceso directo "Claude (VPN)" justo antes de abrir Claude, y se cierra solo cuando terminan todos los procesos que se conectaron a él (con 10 segundos de margen), o a los 2 minutos si nadie se conecta.

La desinstalación revierte todos los pasos: borra la ruta, el servicio del túnel, el proceso, los accesos directos y los archivos.

## Lo que NO hace

- No se conecta a ningún servidor propio, no envía telemetría y no descarga nada. La única conexión de red que inicia `claude_proxy.exe` es la que le pide la aplicación que lo usa.
- No escucha en la red: el puerto 18080 está atado a `127.0.0.1`.
- No modifica el registro, el firewall, el DNS ni la ruta por defecto del sistema.
- No instala drivers ni servicios propios. El único servicio es el del cliente oficial de WireGuard.
- No lee ni envía el contenido del `.conf` a ningún lado.

## Funciones de Windows que usa cada binario

Extraídas de la tabla de importaciones de los `.exe` distribuidos (`objdump -p`). Coinciden con lo que usa el código fuente.

- `claude_proxy.exe`: red (`socket`, `bind`, `listen`, `accept`, `connect`, `send`, `recv`, `setsockopt`, `getaddrinfo`), `GetAdaptersAddresses` para encontrar la interfaz, `CreateThread` para atender conexiones, y `GetExtendedTcpTable` más `OpenProcess` para saber qué proceso local se conectó y esperar a que termine. `OpenProcess` se pide solo con el permiso `SYNCHRONIZE`, que únicamente permite esperar a que el proceso termine: no permite leer su memoria ni controlarlo.
- `prep_conf.exe`: solo lectura y escritura de archivos vía la biblioteca estándar de C (`msvcrt.dll`).
- Ambos: funciones de sincronización, memoria y conversión de texto (`InitializeCriticalSection`, `VirtualProtect`, `MultiByteToWideChar` y similares). Son del código de arranque estándar que MinGW agrega a todo ejecutable. Ninguno importa funciones para crear procesos, cargar otras librerías, tocar el registro o descargar archivos.

## Cómo verificar

1. Extrae el instalador con 7-Zip (clic derecho, "Abrir archivo"). Así ves los archivos que va a copiar sin ejecutarlo.
2. Compara el SHA-256 de cada archivo extraído con `SHA256SUMS.txt`. En PowerShell: `Get-FileHash .\claude_proxy.exe`.
3. `launch-claude.cmd` es texto: compáralo directamente con el de `nsis/`.
4. Para confirmar que los `.exe` vienen de este código fuente, recompílalos con `build.sh` (ver `COMPILAR.md`). La compilación es reproducible: con las mismas versiones de herramientas, el SHA-256 del instalador y de cada `.exe` coincide exactamente con `SHA256SUMS.txt`.

## Consideraciones de seguridad

- El proxy local no pide autenticación. Cualquier proceso del mismo equipo puede usarlo para salir por el túnel. En un equipo de un solo usuario el riesgo es bajo.
- El proxy no restringe destinos: lo que pase por él sale por la VPN y no por la red corporativa. Si la organización depende de filtros de salida, inspección o DLP en la red, ese tráfico no pasa por ellos.
- El instalador no está firmado digitalmente, por eso Windows SmartScreen muestra una advertencia.
