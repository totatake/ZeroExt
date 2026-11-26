param(
    [string]$Configuration = "Release",
    [string]$OutDir = "$PSScriptRoot\..\build\gui",
    [switch]$SkipTests
)

$ErrorActionPreference = "Stop"
$RepoRoot = (Resolve-Path "$PSScriptRoot\..").Path
$SlnPath = Join-Path $RepoRoot "gui\ZeroExt4.sln"
$NugetConfig = Join-Path $RepoRoot "gui\nuget.config"
$GuiProj = Join-Path $RepoRoot "gui\ZeroExt4.Gui\ZeroExt4.Gui.csproj"
$TestProj = Join-Path $RepoRoot "gui\ZeroExt4.Gui.Tests\ZeroExt4.Gui.Tests.csproj"

if (-not (Get-Command dotnet -ErrorAction SilentlyContinue)) {
    throw "dotnet SDK was not found in PATH."
}

# In sandboxed environments %APPDATA%\NuGet\NuGet.Config may be unreadable; redirect dotnet user paths if needed.
$UserNugetCfg = Join-Path $env:APPDATA "NuGet\NuGet.Config"
$UseIsolatedEnv = $false
try {
    if (Test-Path $UserNugetCfg) {
        [void][System.IO.File]::OpenRead($UserNugetCfg).Dispose()
    }
} catch {
    $UseIsolatedEnv = $true
}

$EnvBase = Join-Path $RepoRoot "build\dotnet-env"
if ($UseIsolatedEnv) {
    New-Item -ItemType Directory -Force -Path $EnvBase | Out-Null
    $env:APPDATA = $EnvBase
    $env:LOCALAPPDATA = $EnvBase
    $env:DOTNET_CLI_HOME = $EnvBase
}
$env:DOTNET_SKIP_FIRST_TIME_EXPERIENCE = "1"
$env:DOTNET_CLI_TELEMETRY_OPTOUT = "1"

Write-Host "Building ZeroExt4 GUI solution ($Configuration)..."
dotnet restore $SlnPath --configfile $NugetConfig -m:1
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

dotnet build $SlnPath -c $Configuration --no-restore -m:1
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$GuiBin = Join-Path $RepoRoot "gui\ZeroExt4.Gui\bin\$Configuration\net10.0-windows"
if (-not (Test-Path (Join-Path $GuiBin "ZeroExt4Gui.exe"))) {
    throw "ZeroExt4Gui.exe not found at $GuiBin"
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
Copy-Item -Path (Join-Path $GuiBin "*") -Destination $OutDir -Recurse -Force

# If build\zeroext4.exe exists, copy it beside ZeroExt4Gui.exe for out-of-the-box standalone launch
$CoreExe = Join-Path $RepoRoot "build\zeroext4.exe"
if (Test-Path $CoreExe) {
    Copy-Item -Path $CoreExe -Destination (Join-Path $OutDir "zeroext4.exe") -Force
}

Write-Host "Published GUI output to: $OutDir"

if (-not $SkipTests) {
    $TestExe = Join-Path $RepoRoot "gui\ZeroExt4.Gui.Tests\bin\$Configuration\net10.0-windows\ZeroExt4.Gui.Tests.exe"
    if (-not (Test-Path $TestExe)) {
        throw "ZeroExt4.Gui.Tests.exe not found at $TestExe"
    }
    Write-Host "Running ZeroExt4.Gui.Tests..."
    & $TestExe
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    $PublishedGuiExe = Join-Path $OutDir "ZeroExt4Gui.exe"
    Write-Host "Running ZeroExt4Gui.exe --self-test..."
    $ErrFile = Join-Path $RepoRoot "build\gui_selftest_err.txt"
    if (Test-Path $ErrFile) { Remove-Item $ErrFile -Force }
    $Proc = Start-Process -FilePath $PublishedGuiExe -ArgumentList "--self-test" -NoNewWindow -Wait -PassThru -RedirectStandardError $ErrFile
    $SelfTestExit = $Proc.ExitCode
    if ($SelfTestExit -ne 0 -and (Test-Path $ErrFile)) {
        Get-Content $ErrFile | Write-Error
    }
    if (Test-Path $ErrFile) { Remove-Item $ErrFile -Force }
    if ($SelfTestExit -ne 0) {
        throw "ZeroExt4Gui.exe --self-test failed with exit code $SelfTestExit"
    }
    Write-Host "ZeroExt4Gui.exe --self-test passed (exit=0)."
}
