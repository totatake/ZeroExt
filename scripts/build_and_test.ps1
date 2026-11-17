param(
    [string]$BuildDir = "$PSScriptRoot\..\build",
    [string]$BuildType = "Release",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$RepoRoot = (Resolve-Path "$PSScriptRoot\..").Path

$Vcvars = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
$Cmake  = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$Ctest  = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"

if (-not (Test-Path $Vcvars)) { throw "vcvars64.bat not found at $Vcvars" }
if (-not (Test-Path $Cmake))  { throw "cmake.exe not found at $Cmake" }
if (-not (Test-Path $Ctest))  { throw "ctest.exe not found at $Ctest" }

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
