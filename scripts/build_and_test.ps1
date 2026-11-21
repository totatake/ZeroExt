param(
    [string]$BuildDir = "$PSScriptRoot\..\build",
    [string]$BuildType = "Release",
    [switch]$Clean,
    [switch]$SkipGui
)

$ErrorActionPreference = "Stop"
$RepoRoot = (Resolve-Path "$PSScriptRoot\..").Path

. (Join-Path $PSScriptRoot "find_vs.ps1")
$Toolchain = Resolve-ZeroExtVsToolchain
$Vcvars = $Toolchain.Vcvars
$Cmake  = $Toolchain.Cmake
$Ctest  = $Toolchain.Ctest

if ($Clean -and (Test-Path $BuildDir)) {
    Remove-Item -Recurse -Force $BuildDir
}

# Ensure real Python comes before WindowsApps alias if present
$PythonBin = Join-Path $env:LOCALAPPDATA "Python\bin"
if (Test-Path (Join-Path $PythonBin "python.exe")) {
    $env:PATH = "$PythonBin;$env:PATH"
}

$Cmd = @"
call "$Vcvars" >nul && "$Cmake" -S "$RepoRoot" -B "$BuildDir" -G Ninja -DCMAKE_BUILD_TYPE=$BuildType && "$Cmake" --build "$BuildDir" && "$Ctest" --test-dir "$BuildDir" --output-on-failure
"@

cmd.exe /c $Cmd
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

if (-not $SkipGui) {
    if (Get-Command dotnet -ErrorAction SilentlyContinue) {
        $GuiScript = Join-Path $PSScriptRoot "build_gui.ps1"
        if (Test-Path $GuiScript) {
            & $GuiScript -Configuration $BuildType -OutDir (Join-Path $BuildDir "gui")
            if ($LASTEXITCODE -ne 0) {
                exit $LASTEXITCODE
            }
        }
    } else {
        Write-Warning "dotnet SDK not found; skipping ZeroExtGui build."
    }
}
