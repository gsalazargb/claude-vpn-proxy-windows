# Reparacion sin reinstalar: actualiza launch-claude.cmd, reinstala el tunel
# con un .conf nuevo y agrega la ruta de metrica 9999. Correr como admin:
#   powershell -ExecutionPolicy Bypass -File reparar.ps1 -Conf <ruta al .conf nuevo>
param([Parameter(Mandatory)][string]$Conf)
$ErrorActionPreference = 'Stop'

$inst = "C:\Program Files\ClaudeVPNProxy"
$wg   = "C:\Program Files\WireGuard\wireguard.exe"
$name = "ClaudeVPN"

# El .conf no debe traer la clave privada enmascarada
$pk = (Select-String -Path $Conf -Pattern '^\s*PrivateKey\s*=\s*(\S+)').Matches.Groups[1].Value
if (-not $pk -or $pk.Length -ne 44 -or $pk.StartsWith('*')) {
    throw "La PrivateKey de '$Conf' no es valida (enmascarada o ausente). Genera un .conf NUEVO en tu proveedor de VPN (Proton u otro) y descargalo en ese momento."
}

Copy-Item "$PSScriptRoot\launch-claude.cmd" "$inst\launch-claude.cmd" -Force

Get-Process claude_proxy -ErrorAction SilentlyContinue | Stop-Process -Force
& $wg /uninstalltunnelservice $name
Start-Sleep 2

& "$inst\prep_conf.exe" $Conf "$inst\$name.conf"
if ($LASTEXITCODE -ne 0) { throw "prep_conf fallo (codigo $LASTEXITCODE)" }

& $wg /installtunnelservice "$inst\$name.conf"
Start-Sleep 4
Get-Service "WireGuardTunnel`$$name" | Format-Table Name, Status

netsh interface ipv4 delete route prefix=0.0.0.0/0 interface="$name" store=persistent 2>$null | Out-Null
netsh interface ipv4 add route prefix=0.0.0.0/0 interface="$name" metric=9999 store=persistent

Write-Host "Listo. Ahora arranca el proxy (sin admin) con el acceso directo de Startup o reiniciando sesion."
