# Prueba del split-tunnel con el cliente de Proton cerrado. No requiere admin.
# Guarda todo en resultados.txt (carpeta del proyecto) para revisarlo despues.
$out = Join-Path (Split-Path $PSScriptRoot) "resultados.txt"
$proxy = "http://127.0.0.1:18080"
$r = @()
function Add($t) { $script:r += $t; Write-Host $t }

Add "=== Prueba $(Get-Date -Format s)"

$pt = Get-NetIPAddress -InterfaceAlias ProTUN -AddressFamily IPv4 -ErrorAction SilentlyContinue
Add ("ProTUN (cliente Proton) con IP: " + $(if ($pt) { "SI $($pt.IPAddress) <- Proton sigue conectado, la prueba no es valida" } else { "no (bien)" }))
Add ("Procesos Proton: " + ((Get-Process ProtonVPN* -ErrorAction SilentlyContinue | % Name) -join ', '))

$a = Get-NetAdapter -Name ClaudeVPN -ErrorAction SilentlyContinue
Add ("Adaptador ClaudeVPN: " + $(if ($a) { "$($a.Status) ifIndex=$($a.ifIndex)" } else { "NO EXISTE" }))
Add ("Rutas default: " + ((Get-NetRoute -DestinationPrefix 0.0.0.0/0 | % { "$($_.InterfaceAlias) m=$($_.RouteMetric)" }) -join ', '))

if (-not (Get-Process claude_proxy -ErrorAction SilentlyContinue)) {
    Start-Process "C:\Program Files\ClaudeVPNProxy\claude_proxy.exe" -ArgumentList '18080','ClaudeVPN' -WindowStyle Minimized
    Start-Sleep 2
    Add "Proxy: no estaba corriendo, se arranco"
} else { Add "Proxy: corriendo" }

$s0 = (Get-NetAdapterStatistics -Name ClaudeVPN -ErrorAction SilentlyContinue).ReceivedBytes

Add ("DNS api.anthropic.com (resolver del sistema): " + ((Resolve-DnsName api.anthropic.com -Type A -ErrorAction SilentlyContinue | ? IPAddress | % IPAddress) -join ', '))

Add ("IP sistema (sin proxy): " + (curl.exe -s -m 15 https://ifconfig.co/json))
Add ("IP via proxy:           " + (curl.exe -s -m 15 -x $proxy https://ifconfig.co/json))
Add ("Anthropic sin proxy: HTTP " + (curl.exe -s -m 15 -o NUL -w "%{http_code} (%{errormsg})" https://api.anthropic.com/v1/messages))
Add ("Anthropic via proxy: HTTP " + (curl.exe -s -m 15 -o NUL -w "%{http_code} (%{errormsg})" -x $proxy https://api.anthropic.com/v1/messages))
Add ("claude.ai via proxy: HTTP " + (curl.exe -s -m 15 -o NUL -w "%{http_code} (%{errormsg})" -x $proxy https://claude.ai))

$s1 = (Get-NetAdapterStatistics -Name ClaudeVPN -ErrorAction SilentlyContinue).ReceivedBytes
Add "Bytes recibidos por ClaudeVPN durante la prueba: $($s1 - $s0)"

$r -join "`r`n" | Out-File $out -Encoding utf8 -Append
Write-Host "`nGuardado en $out"
