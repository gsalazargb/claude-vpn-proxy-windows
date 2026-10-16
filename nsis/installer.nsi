; Instalador: tunel WireGuard DEDICADO solo a Claude Desktop, sin
; tocar el resto del sistema. Funciona con el .conf de WireGuard de
; cualquier proveedor compatible (Proton, Mullvad, un servidor propio, etc),
; no esta atado a Proton especificamente. Ver README.md del proyecto para
; el diseno completo (por que no se uso Netch/Proxifier, por que Table=off).

!include "MUI2.nsh"
!include "nsDialogs.nsh"
!include "LogicLib.nsh"

!define TUNNEL_NAME    "ClaudeVPN"
!define PROXY_PORT     "18080"
!define WIREGUARD_EXE  "$PROGRAMFILES64\WireGuard\wireguard.exe"

Name "Claude VPN Proxy (tunel dedicado)"
OutFile "ClaudeVPNProxySetup.exe"
InstallDir "$PROGRAMFILES64\ClaudeVPNProxy"
RequestExecutionLevel admin

!insertmacro MUI_PAGE_WELCOME
Page custom ConfPageCreate ConfPageLeave
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "Spanish"

Var ConfPathField
Var ConfPath

; ---------------------------------------------------------------------------
; Pagina custom: elegir el .conf de WireGuard de Proton
; ---------------------------------------------------------------------------
Function ConfPageCreate
  nsDialogs::Create 1018
  Pop $0

  ${NSD_CreateLabel} 0 0 100% 44u "Selecciona un archivo .conf de WireGuard (de Proton, Mullvad, tu propio servidor, o cualquier otro proveedor compatible con WireGuard). Este tunel quedara DEDICADO solo a Claude Desktop: el resto de tu PC sigue por tu conexion normal, sin ninguna VPN."
  Pop $0

  ${NSD_CreateText} 0 40u 70% 12u ""
  Pop $ConfPathField

  ${NSD_CreateBrowseButton} 72% 40u 25% 12u "Elegir..."
  Pop $0
  ${NSD_OnClick} $0 ConfPathBrowse

  nsDialogs::Show
FunctionEnd

Function ConfPathBrowse
  nsDialogs::SelectFileDialog open "" "Archivos WireGuard (*.conf)|*.conf"
  Pop $0
  ${If} $0 != ""
    ${NSD_SetText} $ConfPathField $0
  ${EndIf}
FunctionEnd

Function ConfPageLeave
  ${NSD_GetText} $ConfPathField $ConfPath
  ${If} $ConfPath == ""
    MessageBox MB_OK|MB_ICONEXCLAMATION "Tienes que elegir un archivo .conf para continuar."
    Abort
  ${EndIf}
  IfFileExists "$ConfPath" conf_ok conf_missing
  conf_missing:
    MessageBox MB_OK|MB_ICONEXCLAMATION "Ese archivo no existe."
    Abort
  conf_ok:
FunctionEnd

; ---------------------------------------------------------------------------
; Instalacion
; ---------------------------------------------------------------------------
Section "Instalar"
  ; 0. WireGuard oficial debe estar instalado de antemano (no lo empaquetamos
  ;    nosotros: mejor depender del instalador oficial, siempre al dia,
  ;    firmado por ellos, que nosotros manteniendo una copia embebida).
  IfFileExists "${WIREGUARD_EXE}" wg_found wg_missing
  wg_missing:
    MessageBox MB_OK|MB_ICONSTOP "No se encontro WireGuard para Windows instalado.$\r$\n$\r$\nInstala primero el cliente oficial (wireguard.com/install) y vuelve a correr este instalador."
    Abort
  wg_found:

  ; 0.1 El cliente oficial de Proton (si lo usas normalmente) choca con esto:
  ;     puede usar la misma IP interna (10.2.0.2, 'El objeto ya existe' al
  ;     levantar nuestro tunel) y su propio kill-switch/firewall bloquea
  ;     trafico por cualquier interfaz que no sea la suya (el proxy da 502
  ;     sin pista de por que). Confirmado en pruebas reales.
  nsExec::ExecToStack 'cmd /c tasklist | find /i "ProtonVPN" >nul'
  Pop $0
  ${If} $0 == 0
    MessageBox MB_OKCANCEL|MB_ICONEXCLAMATION "Parece que la app oficial de Proton VPN esta corriendo.$\r$\n$\r$\nSu propio firewall (kill-switch) puede bloquear este tunel, o chocar con la misma direccion IP interna. Cierra la app de Proton POR COMPLETO (icono de la bandeja incluido) y presiona Aceptar para continuar, o Cancelar para salir y cerrarla primero." IDOK proton_ok
    Abort
    proton_ok:
  ${EndIf}

  SetOutPath "$INSTDIR"
  ; Sin guardar la fecha de los archivos fuente dentro del instalador: asi
  ; compilar dos veces el mismo codigo produce el mismo .exe byte a byte.
  SetDateSave off
  File "..\src\claude_proxy.exe"
  File "..\src\prep_conf.exe"
  File "launch-claude.cmd"

  ; 0.1 Idempotencia: si ya habia un tunel/proxy de una instalacion anterior
  ;     (reinstalando sin haber desinstalado, o probando de nuevo tras un
  ;     intento fallido), los tumbamos primero para no chocar.
  nsExec::ExecToLog 'taskkill /f /im claude_proxy.exe'
  Pop $0
  nsExec::ExecToLog '"${WIREGUARD_EXE}" /uninstalltunnelservice "${TUNNEL_NAME}"'
  Pop $0
  Sleep 1000

  ; 1. Preparar el .conf (inyecta Table=off si hace falta) y copiarlo
  nsExec::ExecToLog '"$INSTDIR\prep_conf.exe" "$ConfPath" "$INSTDIR\${TUNNEL_NAME}.conf"'
  Pop $0
  ${If} $0 == 3
    MessageBox MB_OK|MB_ICONSTOP "La clave privada de ese .conf viene enmascarada o invalida.$\r$\n$\r$\nEsto pasa si re-descargas un .conf ya generado antes: muchos paneles de VPN solo muestran la clave privada completa la primera vez que la creas. Genera un perfil nuevo en tu proveedor y vuelve a intentar con ese archivo."
    Abort
  ${ElseIf} $0 != 0
    MessageBox MB_OK|MB_ICONSTOP "El archivo .conf elegido no parece valido (revisa que tenga una seccion [Interface])."
    Abort
  ${EndIf}

  ; 1.1 El .conf lleva la clave privada y Program Files es legible por todos
  ;     los usuarios. Se corta la herencia y se deja acceso solo a
  ;     Administradores (S-1-5-32-544) y SYSTEM (S-1-5-18), que es la cuenta
  ;     del servicio de WireGuard. SIDs en vez de nombres porque los nombres
  ;     cambian con el idioma de Windows.
  nsExec::ExecToLog 'icacls "$INSTDIR\${TUNNEL_NAME}.conf" /inheritance:r /grant:r *S-1-5-32-544:F *S-1-5-18:F'
  Pop $0
  ${If} $0 != 0
    Delete "$INSTDIR\${TUNNEL_NAME}.conf"
    MessageBox MB_OK|MB_ICONSTOP "No se pudieron restringir los permisos del .conf (codigo $0). Se borro la copia para no dejar la clave privada expuesta."
    Abort
  ${EndIf}

  ; 2. Instalar el tunel como servicio de Windows (arranca solo, persiste tras reiniciar)
  nsExec::ExecToLog '"${WIREGUARD_EXE}" /installtunnelservice "$INSTDIR\${TUNNEL_NAME}.conf"'
  Pop $0
  ${If} $0 != 0
    MessageBox MB_OK|MB_ICONSTOP "No se pudo instalar el tunel WireGuard (codigo $0). Revisa que el .conf sea correcto."
    Abort
  ${EndIf}

  ; Un respiro para que la interfaz termine de levantarse antes de rutear
  Sleep 3000

  ; 3. Ruta secundaria de alta metrica: existe en la tabla para que el proxy
  ;    (atado a esta interfaz via IP_UNICAST_IF) pueda alcanzar cualquier
  ;    destino, pero nunca gana frente a tu ruta default normal.
  nsExec::ExecToLog 'netsh interface ipv4 delete route prefix=0.0.0.0/0 interface="${TUNNEL_NAME}" store=persistent'
  Pop $0
  nsExec::ExecToLog 'netsh interface ipv4 add route prefix=0.0.0.0/0 interface="${TUNNEL_NAME}" metric=9999 store=persistent'
  Pop $0

  ; 4. El proxy local ya no arranca con el logon: lo levantan los accesos
  ;    directos al abrir Claude y se cierra solo cuando Claude se
  ;    cierran. Se borra el acceso directo de Inicio de versiones anteriores.
  Delete "$SMSTARTUP\ClaudeVPNProxy.lnk"

  ; 5. Accesos directos para el usuario
  CreateDirectory "$SMPROGRAMS\Claude VPN Proxy"
  CreateShortcut "$SMPROGRAMS\Claude VPN Proxy\Claude (VPN).lnk" "$INSTDIR\launch-claude.cmd" "" "" "" SW_SHOWMINIMIZED
  ; Versiones anteriores incluian un lanzador para Edge; ya no (para el
  ; navegador basta la extension de la VPN). Se limpia si existe.
  Delete "$SMPROGRAMS\Claude VPN Proxy\Edge (VPN).lnk"
  Delete "$INSTDIR\launch-edge.cmd"
  CreateShortcut "$DESKTOP\Claude (VPN).lnk" "$INSTDIR\launch-claude.cmd" "" "" "" SW_SHOWMINIMIZED

  WriteUninstaller "$INSTDIR\uninstall.exe"
  CreateShortcut "$SMPROGRAMS\Claude VPN Proxy\Desinstalar.lnk" "$INSTDIR\uninstall.exe"

  MessageBox MB_OK|MB_ICONINFORMATION "Listo. Usa el acceso directo 'Claude (VPN)' del Menu Inicio o del escritorio. Todo lo demas en tu PC sigue por tu conexion normal, sin tocar nada."
SectionEnd

; ---------------------------------------------------------------------------
; Desinstalacion
; ---------------------------------------------------------------------------
Section "Uninstall"
  nsExec::ExecToLog 'netsh interface ipv4 delete route prefix=0.0.0.0/0 interface="${TUNNEL_NAME}" store=persistent'
  Pop $0
  nsExec::ExecToLog '"${WIREGUARD_EXE}" /uninstalltunnelservice "${TUNNEL_NAME}"'
  Pop $0
  nsExec::ExecToLog 'taskkill /f /im claude_proxy.exe'
  Pop $0

  Delete "$SMSTARTUP\ClaudeVPNProxy.lnk"
  Delete "$DESKTOP\Claude (VPN).lnk"
  Delete "$SMPROGRAMS\Claude VPN Proxy\Claude (VPN).lnk"
  Delete "$SMPROGRAMS\Claude VPN Proxy\Edge (VPN).lnk"
  Delete "$SMPROGRAMS\Claude VPN Proxy\Desinstalar.lnk"
  RMDir "$SMPROGRAMS\Claude VPN Proxy"

  Delete "$INSTDIR\claude_proxy.exe"
  Delete "$INSTDIR\prep_conf.exe"
  Delete "$INSTDIR\launch-claude.cmd"
  Delete "$INSTDIR\launch-edge.cmd"
  Delete "$INSTDIR\${TUNNEL_NAME}.conf"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"
SectionEnd
