# Ativa o ambiente do firmware RTLS na sessao atual do PowerShell.
#
# Uso (na raiz do repositorio):
#     . .\scripts\env.ps1
#
# Se a ExecutionPolicy bloquear scripts, use uma vez:
#     Set-ExecutionPolicy -Scope CurrentUser RemoteSigned
# ou abra um shell ja ativado com:
#     powershell -ExecutionPolicy Bypass -NoExit -File .\scripts\env.ps1

$RtlsRepo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$RtlsWorkspace = Split-Path -Parent $RtlsRepo
$RtlsVenv = Join-Path $RtlsWorkspace '.venv'

if (-not (Test-Path (Join-Path $RtlsVenv 'Scripts\python.exe'))) {
    Write-Warning "venv nao encontrado em $RtlsVenv. Rode antes: scripts\setup\setup-windows.ps1"
    return
}

$env:VIRTUAL_ENV = $RtlsVenv
$env:Path = "$(Join-Path $RtlsVenv 'Scripts');$env:Path"

# J-Link da SEGGER na frente do PATH (evita o jlink.exe do JDK).
try {
    $RtlsJLink = (Get-ItemProperty -Path 'HKCU:\Software\SEGGER\J-Link' -Name InstallPath -ErrorAction Stop).InstallPath
    if ($RtlsJLink) { $env:Path = "$RtlsJLink;$env:Path" }
} catch { }

# 7-Zip (usado por "west sdk install").
$RtlsSevenZip = Join-Path $env:ProgramFiles '7-Zip'
if (Test-Path (Join-Path $RtlsSevenZip '7z.exe')) { $env:Path = "$env:Path;$RtlsSevenZip" }

Set-Location $RtlsRepo
Write-Host "Ambiente RTLS ativo (workspace west: $RtlsWorkspace)" -ForegroundColor Green
Write-Host '  west build -b decawave_dwm1001_dev apps/tag -d build/tag'
Write-Host '  west flash -d build/tag [--dev-id <serial J-Link>]'
Write-Host '  .\scripts\list-boards.ps1   (placas conectadas: COM x serial)'
