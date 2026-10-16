# Claude VPN Proxy (Windows): Claude Desktop + Edge por un túnel VPN dedicado, sin tocar el resto del sistema

Equivalente para Windows de lo que se armó en Linux con `network namespace` (ver `../claude-proton-netns/`): solo Claude Desktop y Edge salen por un túnel VPN dedicado, todo lo demás en la PC sigue por la conexión normal.

**No está atado a Proton.** Funciona con el `.conf` de WireGuard de cualquier proveedor compatible: Proton, Mullvad, un servidor propio, el que sea. Se llamó "Proton" en las primeras pruebas porque fue el proveedor usado para probarlo, pero el diseño es genérico.

**Probado en Windows real** (dual-boot del autor), no solo bajo Wine. Ver sección "Resultados de la prueba real" más abajo.

## Por qué quedó así

Primero se evaluó **Netch** (soporta WireGuard + ProcessMode). Se descartó: su driver (`netfilter2.sys`) está en la **lista de bloqueo de drivers vulnerables de Microsoft**, y Windows 11 con Core Isolation activado (default) lo bloquea. El proyecto lleva años sin release estable (último: 1.9.7, junio 2022), nunca se adaptó a esa política.

La alternativa sin ningún driver de terceros: el **cliente oficial de WireGuard para Windows** (mantenido, firmado, no bloqueado) soporta `Table = off` en `[Interface]` (confirmado leyendo el código fuente real en `tunnel/addressconfig.go`): el túnel queda arriba, con IP y DNS asignados, pero **sin instalar rutas automáticas ni el firewall de kill-switch**. Así, el resto del sistema queda 100% intacto.

Como el túnel no tiene ninguna ruta por sí solo, hace falta:
1. Una ruta secundaria de métrica altísima (vía `netsh interface ipv4 add route`) para que exista *alguna* ruta hacia internet por esa interfaz (si no, cualquier intento de usarla falla con "red inalcanzable").
2. Algo que fuerce el tráfico de Claude/Edge específicamente por esa interfaz. Como esa ruta tiene métrica altísima, nadie más la usa por accidente.

Eso último se resuelve en espacio de usuario con `IP_UNICAST_IF` (API nativa de Winsock, sin ningún driver), en un proxy HTTP local propio (`claude_proxy.exe`), combinado con que Claude Desktop y Edge **ya saben usar un proxy por sí solos** (Claude Desktop respeta `HTTPS_PROXY`/`HTTP_PROXY` y además el flag `--proxy-server`; Edge/Chromium acepta `--proxy-server`). No hace falta interceptar nada a la fuerza.

**Límite aceptado:** esto solo cubre apps que acepten configurarse con un proxy. Una app que no soporte nada de eso necesitaría volver a algo tipo driver (Proxifier de pago, ya que Netch quedó descartado).

**Otro límite:** la resolución DNS de los hostnames se hace por el resolver normal del sistema, no por la VPN dedicada. Solo el tráfico de datos en sí sale por el túnel.

## Resultados de la prueba real (2026-10-02, Windows 11, dual-boot)

Con el cliente oficial de Proton **desconectado** (para que la prueba no se contamine):

| Chequeo | Resultado |
|---|---|
| Adaptador `ClaudeVPN` | Up, ifIndex=25 |
| Rutas default | `ClaudeVPN` metric=9999, `Ethernet` metric=0 (orden correcto: nadie prefiere la VPN por accidente) |
| IP de salida normal (sin proxy) | IP pública del ISP (omitida) |
| IP de salida vía el proxy | IP del servidor VPN (omitida, **distinta** a la normal: confirma que el proxy sí saca el tráfico por otro lado) |
| `api.anthropic.com` sin proxy | HTTP 403 (confirma el problema original: sin ninguna VPN, no hay acceso) |
| `api.anthropic.com` vía proxy | HTTP 405 (llegó a la API real; 405 es la respuesta normal a un GET en ese endpoint) |
| Bytes recibidos por la interfaz durante la prueba | 25472 (confirma tráfico real pasando por el túnel) |

Esto confirma de punta a punta: el aislamiento de rutas funciona como se diseñó, y el proxy efectivamente saca el tráfico por la VPN dedicada en vez de por la conexión normal.

## Requisito importante: cerrar la app oficial de Proton (u otra VPN GUI) antes de usar esto

Si normalmente usas la app de escritorio de tu proveedor VPN (Proton VPN, por ejemplo) para otras cosas, **ciérrala por completo** (ícono de la bandeja incluido) antes de instalar o de usar los accesos directos "Claude (VPN)"/"Edge (VPN)". Confirmado en pruebas reales: esa app puede usar la misma dirección IP interna que nuestro túnel (`10.2.0.2`, dando "El objeto ya existe" al instalar el servicio), y su propio kill-switch/firewall bloquea tráfico por cualquier interfaz que no sea la suya, lo que hace que nuestro proxy falle con `502 Bad Gateway` sin ninguna pista de por qué. El instalador ahora detecta si hay un proceso `ProtonVPN` corriendo y te avisa antes de continuar.

## Fixes reales encontrados probando en Windows (ya integrados)

1. **Clave privada enmascarada**: si re-descargas un `.conf` ya generado antes en vez de uno nuevo, algunos paneles de VPN (Proton incluido) muestran la clave privada enmascarada. `prep_conf.exe` ahora valida esto (debe ser un valor base64 de 44 caracteres, no empezar con `*`) y aborta con un mensaje claro (exit code 3) en vez de dejar un túnel roto en silencio.
2. **Claude Desktop es de instancia única**: si ya hay una instancia corriendo (por ejemplo, arrancó sola en el logon de Windows), lanzar nuestro acceso directo solo le pasa el foco a esa instancia existente, que NO tiene el proxy configurado. `launch-claude.cmd` ahora detecta esto (`tasklist`) y le pide al usuario cerrarla del todo primero.
3. **Instalación MSIX (Microsoft Store) de Claude Desktop**: ese empaquetado registra un alias de ejecución en una ruta fija (`%LOCALAPPDATA%\Microsoft\WindowsApps\claude-desktop.exe`), independiente de la versión. `launch-claude.cmd` lo prueba primero antes de caer al método de buscar el acceso directo del Menú Inicio (instalación clásica tipo Squirrel).
4. **Reinstalar sin desinstalar antes**: el instalador ahora es idempotente, tumba cualquier túnel/proxy/ruta de una instalación anterior antes de instalar de cero, así no choca si lo corres de nuevo para probar.
5. **Choque con el cliente oficial de Proton**: el instalador ahora detecta si hay un proceso `ProtonVPN` corriendo y te avisa antes de continuar (ver sección de arriba).
6. **Diagnóstico de conexiones fallidas**: `claude_proxy.exe` ahora registra el código de `WSAGetLastError` cuando un `connect()` falla (por ejemplo, por el kill-switch del punto anterior), antes fallaba en silencio con un 502 sin explicación.

## Archivos

| Archivo | Qué hace |
|---|---|
| `src/claude_proxy.c` / `.exe` | Proxy HTTP local (CONNECT + HTTP plano). Resuelve el índice de la interfaz WireGuard por su nombre y fija ahí cada socket saliente con `IP_UNICAST_IF`. |
| `src/prep_conf.c` / `.exe` | Copia el `.conf` inyectando `Table = off` si no está ya (idempotente), y valida que la clave privada no venga enmascarada. |
| `nsis/launch-claude.cmd` | Pone `HTTPS_PROXY`/`HTTP_PROXY` + `--proxy-server`, detecta instancia ya corriendo, detecta instalación MSIX vs. Squirrel, y lanza Claude Desktop sin depender de la carpeta versionada `app-X.Y.Z`. |
| `nsis/launch-edge.cmd` | Lanza `msedge.exe --proxy-server=127.0.0.1:18080`, resolviendo la ruta vía el registro `App Paths` (mecanismo oficial de Windows para esto). |
| `nsis/installer.nsi` | El instalador: pide el `.conf`, instala el túnel como servicio (`wireguard.exe /installtunnelservice`), agrega la ruta, borra el arranque automático del proxy de versiones anteriores (ahora lo levantan los accesos directos y se cierra solo al cerrar las apps), y los accesos directos. Incluye desinstalador (`uninstall.exe`, generado automáticamente por NSIS) y es idempotente (se puede volver a correr sin desinstalar antes). |
| `nsis/ClaudeVPNProxySetup.exe` | El `.exe` ya compilado. |
| `nsis/reparar.ps1` | Script de reparación sin reinstalar todo: útil si solo cambiaste de `.conf` o algo quedó a medias. `powershell -ExecutionPolicy Bypass -File reparar.ps1 -Conf <ruta al .conf nuevo>` (como admin). |
| `nsis/prueba.ps1` | Script de diagnóstico: confirma que el túnel está arriba, que las rutas tienen las métricas correctas, y compara la IP de salida con/sin proxy. Guarda todo en `resultados.txt`. |

## Desinstalar / dejar la PC en blanco para volver a probar

El instalador genera su propio desinstalador en `C:\Program Files\ClaudeVPNProxy\uninstall.exe` (también accesible desde "Agregar o quitar programas" de Windows, o desde el acceso directo "Desinstalar" en el Menú Inicio). Hace, en orden: quita la ruta de métrica 9999, desinstala el servicio del túnel WireGuard, mata el proceso del proxy, borra accesos directos y archivos.

Si prefieres no reinstalar desde cero para solo cambiar de `.conf`, usa `reparar.ps1` en su lugar (ver tabla de arriba).

## Cómo probar desde cero

1. Instalar primero el cliente oficial de WireGuard (wireguard.com/install) si no está.
2. Bajar un `.conf` nuevo (no uno ya usado antes, por el tema de la clave enmascarada) de tu proveedor VPN compatible con WireGuard.
3. Correr `ClaudeVPNProxySetup.exe`, elegir ese `.conf`.
4. Verificar que el túnel quedó arriba: `wg show` o el propio cliente de WireGuard debería mostrar la interfaz `ClaudeVPN` conectada.
5. Verificar que el resto de Windows sigue con tu IP normal: abre cualquier navegador normal (sin el acceso directo) y entra a algo como `ifconfig.co` o `whatismyip.com`.
6. Usar el acceso directo "Claude (VPN)" y, dentro de Claude, confirmar que conecta. Si Claude Desktop ya estaba abierto, ciérralo del todo primero (icono de la bandeja incluido).
7. Lo mismo con "Edge (VPN)".
8. Si algo falla, los `.cmd` y el instalador imprimen mensajes en español explicando qué falló.
