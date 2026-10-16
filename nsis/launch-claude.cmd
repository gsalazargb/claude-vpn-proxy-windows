@echo off
setlocal

set HTTPS_PROXY=http://127.0.0.1:18080
set HTTP_PROXY=http://127.0.0.1:18080
set PROXY_FLAG=--proxy-server=127.0.0.1:18080

rem Claude Desktop es de instancia unica: si ya hay una abierta (o arranco
rem sola en el logon), el nuevo lanzamiento solo le pasa el foco a esa
rem instancia, que NO tiene el proxy. Hay que cerrarla del todo antes.
tasklist /fi "imagename eq Claude.exe" 2>nul | find /i "Claude.exe" >nul
if not errorlevel 1 (
    echo Claude Desktop ya esta corriendo sin el proxy.
    echo Cierralo por completo, incluido el icono de la bandeja, y vuelve a usar este acceso directo.
    pause
    exit /b 1
)

rem El proxy no arranca con Windows: lo levantamos aqui si no esta corriendo.
rem Corre sin ventana y se cierra solo cuando se cierran las apps que lo usan.
tasklist /fi "imagename eq claude_proxy.exe" 2>nul | find /i "claude_proxy.exe" >nul
if errorlevel 1 (
    start "" "%~dp0claude_proxy.exe" 18080 ClaudeVPN
    timeout /t 2 /nobreak >nul
)
tasklist /fi "imagename eq claude_proxy.exe" 2>nul | find /i "claude_proxy.exe" >nul
if errorlevel 1 (
    echo No se pudo arrancar el proxy local. Revisa que el tunel ClaudeVPN este activo en WireGuard.
    echo Detalle en: %LOCALAPPDATA%\ClaudeVPNProxy\claude_proxy.log
    pause
    exit /b 1
)

rem 1. Instalacion MSIX (Microsoft Store / WindowsApps): el paquete registra
rem    el alias de ejecucion claude-desktop.exe, con ruta fija e independiente
rem    de la version. Lanzado por CreateProcess hereda el entorno y acepta flags.
set CLAUDE_ALIAS=%LOCALAPPDATA%\Microsoft\WindowsApps\claude-desktop.exe
if exist "%CLAUDE_ALIAS%" (
    start "" "%CLAUDE_ALIAS%" %PROXY_FLAG%
    exit /b 0
)

rem 2. Instalacion clasica (Squirrel, carpetas app-X.Y.Z): leemos el target
rem    del acceso directo que mantiene el instalador de Claude. Se excluyen
rem    nuestros propios accesos directos y cualquier target que no sea un
rem    .exe, para no relanzar este mismo .cmd en bucle.
set CLAUDE_TARGET=
for /f "usebackq delims=" %%T in (`powershell -NoProfile -Command ^
  "$paths = @(\"$env:APPDATA\Microsoft\Windows\Start Menu\Programs\", \"$env:ProgramData\Microsoft\Windows\Start Menu\Programs\");" ^
  "$sh = New-Object -ComObject WScript.Shell;" ^
  "Get-ChildItem -Path $paths -Recurse -Filter 'Claude*.lnk' -ErrorAction SilentlyContinue | ForEach-Object { $sh.CreateShortcut($_.FullName).TargetPath } | Where-Object { $_ -like '*.exe' -and $_ -notlike '*ClaudeVPNProxy*' } | Select-Object -First 1"`) do set CLAUDE_TARGET=%%T

if not defined CLAUDE_TARGET (
    echo No se encontro Claude Desktop: ni el alias claude-desktop.exe ni un acceso directo en el Menu Inicio.
    echo Verifica que Claude Desktop este instalado, o avisa para ajustar la ruta.
    pause
    exit /b 1
)

start "" "%CLAUDE_TARGET%" %PROXY_FLAG%
