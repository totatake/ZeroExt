function Resolve-ZeroExt4VsToolchain {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vsInstallPath = $null

    if (Test-Path $vswhere) {
        $vsInstallPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
    }

    if (-not $vsInstallPath) {
        $candidates = @(
            "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools",
            "C:\Program Files\Microsoft Visual Studio\2022\Enterprise",
            "C:\Program Files\Microsoft Visual Studio\2022\Professional",
            "C:\Program Files\Microsoft Visual Studio\2022\Community",
            "C:\Program Files (x86)\Microsoft Visual Studio\2022\Enterprise",
            "C:\Program Files (x86)\Microsoft Visual Studio\2022\Professional",
            "C:\Program Files (x86)\Microsoft Visual Studio\2022\Community"
        )
        foreach ($cand in $candidates) {
            if (Test-Path (Join-Path $cand "VC\Auxiliary\Build\vcvars64.bat")) {
                $vsInstallPath = $cand
                break
            }
        }
    }

    if (-not $vsInstallPath) {
        throw "Visual Studio C++ x64 build tools (vcvars64.bat) not found."
    }

    $vcvars = Join-Path $vsInstallPath "VC\Auxiliary\Build\vcvars64.bat"
    $cmake  = Join-Path $vsInstallPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    $ctest  = Join-Path $vsInstallPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"

    if (-not (Test-Path $cmake)) {
        $cmd = Get-Command cmake -ErrorAction SilentlyContinue
        if ($cmd) { $cmake = $cmd.Source }
    }
    if (-not (Test-Path $ctest)) {
        $cmd = Get-Command ctest -ErrorAction SilentlyContinue
        if ($cmd) { $ctest = $cmd.Source }
    }

    if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }
    if (-not (Test-Path $cmake))  { throw "cmake.exe not found" }
    if (-not (Test-Path $ctest))  { throw "ctest.exe not found" }

    return @{
        Vcvars = $vcvars
        Cmake  = $cmake
        Ctest  = $ctest
    }
}
