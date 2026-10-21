<#
.SYNOPSIS
    Configura o ambiente de desenvolvimento do firmware RTLS (Zephyr) no Windows.

.DESCRIPTION
    Passos (cada um e pulado se ja estiver feito):
      1. Dependencias via winget: CMake, Ninja, gperf, dtc, Git, 7-Zip.
      2. Python (padrao 3.12) via Python install manager ou winget.
      3. venv em <workspace>\.venv + west.
      4. Workspace west (topologia T2): o workspace e o diretorio PAI deste
         repositorio; baixa o Zephyr e os modulos fixados no west.yml.
      5. Dependencias Python do Zephyr + export do pacote CMake.
      6. Zephyr SDK, apenas a toolchain arm-zephyr-eabi.
      7. Verificacao do SEGGER J-Link.
      8. Build de teste dos tres firmwares (tag, anchor, gateway).

    Documentacao completa: docs\06-configuracao-ambiente.md

.PARAMETER PythonVersion
    Versao do Python do venv (padrao 3.12, a recomendada pelo Zephyr).
.PARAMETER SkipPackages
    Nao instala nada via winget.
.PARAMETER SkipSdk
    Nao instala o Zephyr SDK.
.PARAMETER SkipBuild
    Nao executa o build de teste no final.
.PARAMETER FullClone
    Clona o historico completo do Zephyr (padrao: clone raso, bem menor).
.PARAMETER AcceptAgreements
    Repassa --accept-source-agreements/--accept-package-agreements ao winget.
.PARAMETER Yes
    Responde "sim" as confirmacoes.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\scripts\setup\setup-windows.ps1
#>
[CmdletBinding()]
param(
    [string]$PythonVersion = '3.12',
    [switch]$SkipPackages,
    [switch]$SkipSdk,
    [switch]$SkipBuild,
    [switch]$FullClone,
    [switch]$AcceptAgreements,
    [switch]$Yes
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$RepoName = Split-Path -Leaf $RepoRoot
$Workspace = Split-Path -Parent $RepoRoot
$VenvDir = Join-Path $Workspace '.venv'
$Board = 'decawave_dwm1001_dev'
$Apps = @('tag', 'anchor', 'gateway')

$WingetArgs = @('--exact', '--source', 'winget')
if ($AcceptAgreements) {
    $WingetArgs += @('--accept-source-agreements', '--accept-package-agreements')
}

$Packages = @(
    @{ Id = 'Kitware.CMake'; Cmd = 'cmake' },
    @{ Id = 'Ninja-build.Ninja'; Cmd = 'ninja' },
    @{ Id = 'oss-winget.gperf'; Cmd = 'gperf' },
    @{ Id = 'oss-winget.dtc'; Cmd = 'dtc' },
    @{ Id = 'Git.Git'; Cmd = 'git' },
    @{ Id = '7zip.7zip'; Cmd = '7z' }
)

function Write-Step([string]$Message) { Write-Host "`n==> $Message" -ForegroundColor Cyan }
function Write-Info([string]$Message) { Write-Host "    $Message" }
function Write-Note([string]$Message) { Write-Host "    AVISO: $Message" -ForegroundColor Yellow }

function Test-Tool([string]$Name) {
    return [bool](Get-Command $Name -ErrorAction SilentlyContinue)
}

# Executa um programa nativo e falha se o codigo de saida for diferente de zero.
function Invoke-Native([string]$Exe, [string[]]$Arguments) {
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Falhou (codigo $LASTEXITCODE): $Exe $($Arguments -join ' ')"
    }
}

# Executa um programa nativo ignorando stderr. No PowerShell 5.1, redirecionar
# stderr com ErrorActionPreference=Stop transforma qualquer aviso em erro fatal.
function Invoke-Quiet([string]$Exe, [string[]]$Arguments) {
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = & $Exe @Arguments 2>$null
        return @{ Code = $LASTEXITCODE; Output = $output }
    } finally {
        $ErrorActionPreference = $previous
    }
}

function Update-SessionPath {
    $machine = [Environment]::GetEnvironmentVariable('Path', 'Machine')
    $user = [Environment]::GetEnvironmentVariable('Path', 'User')
    $env:Path = "$machine;$user"
    $sevenZip = Join-Path $env:ProgramFiles '7-Zip'
    if ((Test-Path (Join-Path $sevenZip '7z.exe')) -and -not (Test-Tool '7z')) {
        $env:Path = "$sevenZip;$env:Path"
    }
}

function Confirm-Continue([string]$Question) {
    if ($Yes) { return $true }
    $answer = Read-Host "$Question [s/N]"
    return ($answer -match '^(s|sim|y|yes)$')
}

function Get-ToolVersion([string]$Exe) {
    $result = Invoke-Quiet $Exe @('--version')
    $match = [regex]::Match(($result.Output | Out-String), '\d+\.\d+(\.\d+)?')
    if ($match.Success) { return [version]$match.Value }
    return $null
}

function Get-PythonExe([string]$Version) {
    if (-not (Test-Tool 'py')) { return $null }
    $result = Invoke-Quiet 'py' @("-$Version", '-c', 'import sys; print(sys.executable)')
    if ($result.Code -eq 0 -and $result.Output) {
        return ([string]($result.Output | Select-Object -Last 1)).Trim()
    }
    return $null
}

function Get-JLinkDir {
    try {
        $dir = (Get-ItemProperty -Path 'HKCU:\Software\SEGGER\J-Link' -Name InstallPath -ErrorAction Stop).InstallPath
        if ($dir -and (Test-Path (Join-Path $dir 'JLink.exe'))) { return $dir }
    } catch { }
    $segger = Join-Path $env:ProgramFiles 'SEGGER'
    $candidate = Get-ChildItem -Path $segger -Directory -Filter 'JLink*' -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending | Select-Object -First 1
    if ($candidate -and (Test-Path (Join-Path $candidate.FullName 'JLink.exe'))) { return $candidate.FullName }
    return $null
}

Write-Host "Firmware RTLS DWM1001 - setup do ambiente (Windows)"
Write-Info "Repositorio: $RepoRoot"
Write-Info "Workspace west: $Workspace"

# ---------------------------------------------------------------------------
Write-Step '1/8 Dependencias do sistema (winget)'
Update-SessionPath
if ($SkipPackages) {
    Write-Info 'Pulado (-SkipPackages)'
} else {
    if (-not (Test-Tool 'winget')) {
        throw 'winget nao encontrado. Instale o "App Installer" pela Microsoft Store e rode novamente.'
    }
    foreach ($pkg in $Packages) {
        if (Test-Tool $pkg.Cmd) {
            Write-Info "$($pkg.Id): ok"
            continue
        }
        Write-Info "Instalando $($pkg.Id)..."
        & winget install --id $pkg.Id @WingetArgs
        Update-SessionPath
        if (-not (Test-Tool $pkg.Cmd)) {
            # CMake e Ninja ainda podem vir do pip no passo 4.
            Write-Note "winget nao instalou $($pkg.Id) (codigo $LASTEXITCODE)."
        }
    }
}

if (-not (Test-Tool 'git')) { throw "'git' nao encontrado no PATH." }
if (-not $SkipSdk -and -not (Test-Tool '7z')) {
    throw "'7z' (7-Zip) nao encontrado; necessario para extrair o Zephyr SDK."
}

# ---------------------------------------------------------------------------
Write-Step "2/8 Python $PythonVersion"
$PythonExe = Get-PythonExe $PythonVersion
if (-not $PythonExe) {
    if (Test-Tool 'pymanager') {
        Write-Info "Instalando Python $PythonVersion via Python install manager..."
        Invoke-Native 'pymanager' @('install', $PythonVersion)
    } elseif (-not $SkipPackages) {
        Write-Info "Instalando Python $PythonVersion via winget..."
        & winget install --id "Python.Python.$PythonVersion" @WingetArgs
        Update-SessionPath
    }
    $PythonExe = Get-PythonExe $PythonVersion
    if (-not $PythonExe) {
        throw "Python $PythonVersion nao encontrado (py -$PythonVersion). Instale-o e rode novamente."
    }
}
Write-Info "Python: $PythonExe"

# ---------------------------------------------------------------------------
Write-Step "3/8 Workspace west em $Workspace"
$WestDir = Join-Path $Workspace '.west'
if (Test-Path $WestDir) {
    Write-Info 'Workspace west ja existe'
} else {
    $others = @(Get-ChildItem -LiteralPath $Workspace -Force |
        Where-Object { $_.Name -notin @($RepoName, '.venv', 'zephyr', 'modules') })
    if ($others.Count -gt 0) {
        Write-Note "A pasta '$Workspace' ja contem $($others.Count) outro(s) item(ns)."
        Write-Info "O west vai criar ali: .west\, .venv\, zephyr\ e modules\."
        Write-Info "Recomendado: deixar o repositorio numa pasta dedicada, ex.: $Workspace\rtls-ws\$RepoName"
        if (-not (Confirm-Continue "Usar '$Workspace' como workspace west mesmo assim?")) {
            throw 'Cancelado. Mova o repositorio para uma pasta dedicada e rode o script de la.'
        }
    }
}

# ---------------------------------------------------------------------------
Write-Step "4/8 Ambiente virtual Python ($VenvDir)"
$VenvPython = Join-Path $VenvDir 'Scripts\python.exe'
if (-not (Test-Path $VenvPython)) {
    Invoke-Native $PythonExe @('-m', 'venv', $VenvDir)
}
# Ativacao equivalente ao Activate.ps1, sem depender da ExecutionPolicy.
$env:VIRTUAL_ENV = $VenvDir
$env:Path = "$(Join-Path $VenvDir 'Scripts');$env:Path"
Invoke-Native 'python' @('-m', 'pip', 'install', '--upgrade', '--quiet', 'pip', 'west')
Write-Info "west $((Invoke-Quiet 'west' @('--version')).Output)"

# CMake/Ninja ausentes ou antigos: versoes do pip dentro do venv.
$cmakeVersion = if (Test-Tool 'cmake') { Get-ToolVersion 'cmake' } else { $null }
if (-not $cmakeVersion -or $cmakeVersion -lt [version]'3.20.5') {
    Write-Info "CMake ausente ou antigo ($cmakeVersion); instalando via pip no venv"
    Invoke-Native 'python' @('-m', 'pip', 'install', '--quiet', 'cmake>=3.20.5')
}
if (-not (Test-Tool 'ninja')) {
    Write-Info 'Ninja ausente; instalando via pip no venv'
    Invoke-Native 'python' @('-m', 'pip', 'install', '--quiet', 'ninja')
}
Write-Info "cmake $(Get-ToolVersion 'cmake'), ninja $(Get-ToolVersion 'ninja')"
foreach ($tool in 'gperf', 'dtc') {
    if (Test-Tool $tool) {
        Write-Info "$tool $(Get-ToolVersion $tool)"
    } else {
        Write-Note "'$tool' nao encontrado (dtc e opcional; gperf so e usado com CONFIG_USERSPACE)."
    }
}

# ---------------------------------------------------------------------------
Write-Step '5/8 Zephyr e modulos (west init / update)'
if (-not (Test-Path $WestDir)) {
    Invoke-Native 'west' @('init', '-l', $RepoRoot)
}
Push-Location $RepoRoot
try {
    $updateArgs = @('update')
    if (-not $FullClone) { $updateArgs += @('--narrow', '--fetch-opt=--depth=1') }
    Invoke-Native 'west' $updateArgs
    Invoke-Native 'west' @('zephyr-export')

    Write-Info 'Dependencias Python do Zephyr...'
    $pip = Invoke-Quiet 'west' @('packages', 'pip')
    if ($pip.Code -ne 0) { throw "'west packages pip' falhou (codigo $($pip.Code))." }
    $pipArgs = @((($pip.Output | Out-String).Trim()) -split '\s+' | Where-Object { $_ })
    Invoke-Native 'python' (@('-m', 'pip', 'install', '--quiet') + $pipArgs)
} finally {
    Pop-Location
}

# ---------------------------------------------------------------------------
Write-Step '6/8 Zephyr SDK (toolchain arm-zephyr-eabi)'
if ($SkipSdk) {
    Write-Info 'Pulado (-SkipSdk)'
} else {
    $sdkVersion = (Get-Content (Join-Path $Workspace 'zephyr\SDK_VERSION') -TotalCount 1).Trim()
    $sdkDir = Join-Path $HOME "zephyr-sdk-$sdkVersion"
    $hasArm = (Test-Path (Join-Path $sdkDir 'arm-zephyr-eabi')) -or
              (Test-Path (Join-Path $sdkDir 'gnu\arm-zephyr-eabi'))
    if ($hasArm) {
        Write-Info "SDK $sdkVersion ja instalado em $sdkDir"
    } else {
        Push-Location (Join-Path $Workspace 'zephyr')
        try {
            Invoke-Native 'west' @('sdk', 'install', '-t', 'arm-zephyr-eabi')
        } finally {
            Pop-Location
        }
    }
}

# ---------------------------------------------------------------------------
Write-Step '7/8 SEGGER J-Link (gravacao e debug)'
$jlinkDir = Get-JLinkDir
if ($jlinkDir) {
    Write-Info "J-Link: $jlinkDir"
} else {
    Write-Note 'J-Link Software nao encontrado.'
    Write-Info 'Baixe o "J-Link Software and Documentation Pack" (Windows 64-bit) em'
    Write-Info 'https://www.segger.com/downloads/jlink/ e instale com as opcoes padrao.'
}
$jlinkInPath = Get-Command 'JLink.exe' -ErrorAction SilentlyContinue
if ($jlinkInPath -and ($jlinkInPath.Source -notmatch 'SEGGER')) {
    Write-Note "O 'JLink.exe' do PATH e '$($jlinkInPath.Source)' (provavel jlink do JDK)."
    Write-Info 'O west flash usa o caminho registrado pelo instalador do J-Link, e scripts\env.ps1'
    Write-Info 'coloca o SEGGER na frente do PATH, entao nao e preciso alterar o JDK.'
}

# ---------------------------------------------------------------------------
Write-Step '8/8 Build de teste'
if ($SkipBuild) {
    Write-Info 'Pulado (-SkipBuild)'
} else {
    Push-Location $RepoRoot
    try {
        foreach ($app in $Apps) {
            Write-Info "apps/$app -> build/$app"
            Invoke-Native 'west' @('build', '-b', $Board, "apps/$app", '-d', "build/$app", '-p', 'auto')
        }
    } finally {
        Pop-Location
    }
}

Write-Host "`nAmbiente pronto." -ForegroundColor Green
Write-Host 'Em cada terminal novo do PowerShell, ative com:'
Write-Host '    . .\scripts\env.ps1'
Write-Host 'Gravar a tag (com a placa conectada):'
Write-Host '    west flash -d build\tag'
