param(
    [ValidateSet("all", "asan", "leak", "analyze")]
    [string]$Mode = "all",
    [switch]$KeepBuildDir
)

$ErrorActionPreference = "Stop"
$RepoRoot = (Resolve-Path "$PSScriptRoot\..").Path

$Vcvars = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
$Cmake  = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$Ctest  = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"

if (-not (Test-Path $Vcvars)) { throw "vcvars64.bat not found at $Vcvars" }
if (-not (Test-Path $Cmake))  { throw "cmake.exe not found at $Cmake" }
if (-not (Test-Path $Ctest))  { throw "ctest.exe not found at $Ctest" }

$PythonBin = Join-Path $env:LOCALAPPDATA "Python\bin"
if (Test-Path (Join-Path $PythonBin "python.exe")) {
    $env:PATH = "$PythonBin;$env:PATH"
}

function Invoke-SanitizerRun {
    param(
        [string]$Title,
        [string]$DirName,
        [string]$BuildType,
        [string[]]$CmakeExtraArgs,
        [bool]$RunTests,
        [bool]$CopyAsanRuntime
    )
    $bdir = Join-Path $RepoRoot $DirName
    if (Test-Path $bdir) {
        Remove-Item -Recurse -Force $bdir
    }

    Write-Host ""
    Write-Host "========================================================================" -ForegroundColor Cyan
    Write-Host "  $Title" -ForegroundColor Cyan
    Write-Host "========================================================================" -ForegroundColor Cyan

    $extra = ($CmakeExtraArgs -join " ")
    $cfgBuildCmd = "call `"$Vcvars`" >nul && `"$Cmake`" -S `"$RepoRoot`" -B `"$bdir`" -G Ninja -DCMAKE_BUILD_TYPE=$BuildType $extra && `"$Cmake`" --build `"$bdir`""
    cmd.exe /c $cfgBuildCmd
    if ($LASTEXITCODE -ne 0) {
        throw "$Title build failed (exit=$LASTEXITCODE)"
    }

    if ($CopyAsanRuntime) {
        $asanDlls = Get-ChildItem "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\*\bin\Hostx64\x64\clang_rt.asan_dynamic-x86_64.dll" -ErrorAction SilentlyContinue
        if ($asanDlls) {
            Copy-Item -Force $asanDlls[0].FullName $bdir
        }
    }

    if ($RunTests) {
        $testCmd = "call `"$Vcvars`" >nul && `"$Ctest`" --test-dir `"$bdir`" --output-on-failure"
        cmd.exe /c $testCmd
        if ($LASTEXITCODE -ne 0) {
            throw "$Title tests failed (exit=$LASTEXITCODE)"
        }
    }

    if (-not $KeepBuildDir -and (Test-Path $bdir)) {
        Remove-Item -Recurse -Force $bdir
    }
}

if ($Mode -eq "all" -or $Mode -eq "asan") {
    $env:ASAN_OPTIONS = "detect_leaks=0:halt_on_error=1"
    Invoke-SanitizerRun `
        -Title "1. AddressSanitizer (/fsanitize=address, RelWithDebInfo)" `
        -DirName "build_asan" `
        -BuildType "RelWithDebInfo" `
        -CmakeExtraArgs @("-DZEROEXT_ENABLE_ASAN=ON") `
        -RunTests $true `
        -CopyAsanRuntime $true
}

if ($Mode -eq "all" -or $Mode -eq "leak") {
    Invoke-SanitizerRun `
        -Title "2. MSVC CRT Debug Heap Leak Check (Debug)" `
        -DirName "build_leak" `
        -BuildType "Debug" `
        -CmakeExtraArgs @("-DZEROEXT_ENABLE_LEAK_CHECK=ON") `
        -RunTests $true `
        -CopyAsanRuntime $false
}

if ($Mode -eq "all" -or $Mode -eq "analyze") {
    Invoke-SanitizerRun `
        -Title "3. MSVC Static Code Analysis (/analyze, RelWithDebInfo)" `
        -DirName "build_analyze" `
        -BuildType "RelWithDebInfo" `
        -CmakeExtraArgs @("-DZEROEXT_ENABLE_ANALYZE=ON") `
        -RunTests $false `
        -CopyAsanRuntime $false
}

Write-Host ""
Write-Host "Sanitizer / Static Analysis checks completed successfully." -ForegroundColor Green
