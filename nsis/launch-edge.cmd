@echo off
setlocal

set EDGE_TARGET=
for /f "usebackq tokens=2,*" %%A in (`reg query "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\msedge.exe" /ve 2^>nul`) do set EDGE_TARGET=%%B

if not defined EDGE_TARGET (
    echo No se encontro Microsoft Edge instalado ^(clave App Paths ausente^).
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

start "" "%EDGE_TARGET%" --proxy-server="127.0.0.1:18080"
