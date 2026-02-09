# ==============================================================================
# Build OpenCV with CUDA Support - ONE TIME ONLY
# ==============================================================================
# This script builds OpenCV with CUDA and installs it to opencv_cuda_install/
# After running this once, CMakeLists.txt will detect and use the pre-built
# version, avoiding CUDA recompilation on every build.
#
# Usage: .\build_opencv_cuda_once.ps1
# ==============================================================================

$ErrorActionPreference = "Stop"

# Configuration
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$JuceDir = Join-Path $ScriptDir "juce"
$BuildDir = Join-Path $JuceDir "build-opencv-cuda"
$InstallDir = Join-Path $ScriptDir "opencv_cuda_install"

Write-Host "============================================" -ForegroundColor Cyan
Write-Host "  OpenCV CUDA One-Time Builder" -ForegroundColor Cyan
Write-Host "============================================" -ForegroundColor Cyan
Write-Host ""
Write-Host "Build Directory: $BuildDir"
Write-Host "Install Directory: $InstallDir"
Write-Host ""

# Check for Visual Studio environment
if (-not $env:VSINSTALLDIR) {
    Write-Host "Setting up Visual Studio environment..." -ForegroundColor Yellow
    $vcvarsPath = "C:\VS\Studio2022\VC\Auxiliary\Build\vcvars64.bat"
    if (Test-Path $vcvarsPath) {
        cmd /c "`"$vcvarsPath`" && set" | ForEach-Object {
            if ($_ -match "^([^=]+)=(.*)$") {
                [System.Environment]::SetEnvironmentVariable($matches[1], $matches[2], "Process")
            }
        }
    } else {
        Write-Error "Visual Studio vcvars64.bat not found at $vcvarsPath"
        exit 1
    }
}

# Check for CUDA
$cudaPath = $env:CUDA_PATH
if (-not $cudaPath) {
    $cudaPath = "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.6"
}
if (-not (Test-Path $cudaPath)) {
    Write-Error "CUDA not found. Please install CUDA Toolkit."
    exit 1
}
Write-Host "CUDA Path: $cudaPath" -ForegroundColor Green

# Check for Ninja
$ninja = Get-Command ninja -ErrorAction SilentlyContinue
if (-not $ninja) {
    Write-Error "Ninja not found. Please install Ninja build system."
    exit 1
}

# Create build directory
if (-not (Test-Path $BuildDir)) {
    New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null
}

# Run CMake configure with OpenCV CUDA build enabled
Write-Host ""
Write-Host "Step 1/3: Configuring CMake..." -ForegroundColor Yellow
Write-Host ""

$cmakeArgs = @(
    "-B", $BuildDir,
    "-S", $JuceDir,
    "-G", "Ninja",
    "-DCMAKE_BUILD_TYPE=Release",
    "-DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake",
    "-DENABLE_CUDA=ON",
    "-DBUILD_OPENCV_FROM_SOURCE=ON"
)

& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) {
    Write-Error "CMake configure failed!"
    exit 1
}

# Build only OpenCV (opencv_world target)
Write-Host ""
Write-Host "Step 2/3: Building OpenCV with CUDA (this takes 15-30 minutes)..." -ForegroundColor Yellow
Write-Host ""

# Note: Not using --parallel because CUDA compile commands can exceed Windows cmd.exe limits
& cmake --build $BuildDir --target opencv_world --config Release
if ($LASTEXITCODE -ne 0) {
    Write-Error "OpenCV build failed!"
    exit 1
}

# Install OpenCV to standalone directory
Write-Host ""
Write-Host "Step 3/3: Installing OpenCV to $InstallDir..." -ForegroundColor Yellow
Write-Host ""

# Create install directory structure
$includeDst = Join-Path $InstallDir "include"
$libDst = Join-Path $InstallDir "lib"
$binDst = Join-Path $InstallDir "bin"

New-Item -ItemType Directory -Path $includeDst -Force | Out-Null
New-Item -ItemType Directory -Path $libDst -Force | Out-Null
New-Item -ItemType Directory -Path $binDst -Force | Out-Null

# Copy headers from opencv source (FetchContent downloaded)
$opencvSrc = Join-Path $BuildDir "_deps\opencv-src"
$opencvContribSrc = Join-Path $BuildDir "_deps\opencv_contrib-src"
$opencvBuildModules = Join-Path $BuildDir "_deps\opencv-build"

# Copy main OpenCV headers
if (Test-Path "$opencvSrc\include") {
    Copy-Item -Path "$opencvSrc\include\*" -Destination $includeDst -Recurse -Force
}

# Copy module headers
$moduleHeaders = @(
    "core", "imgproc", "imgcodecs", "videoio", "highgui", "video", 
    "calib3d", "features2d", "objdetect", "dnn", "photo", "stitching"
)
foreach ($mod in $moduleHeaders) {
    $modInclude = Join-Path $opencvSrc "modules\$mod\include"
    if (Test-Path $modInclude) {
        Copy-Item -Path "$modInclude\*" -Destination $includeDst -Recurse -Force
    }
}

# Copy CUDA module headers from contrib
$cudaModules = @(
    "cudaarithm", "cudabgsegm", "cudacodec", "cudafeatures2d", 
    "cudafilters", "cudaimgproc", "cudaobjdetect", "cudaoptflow",
    "cudastereo", "cudawarping"
)
foreach ($mod in $cudaModules) {
    $modInclude = Join-Path $opencvContribSrc "modules\$mod\include"
    if (Test-Path $modInclude) {
        Copy-Item -Path "$modInclude\*" -Destination $includeDst -Recurse -Force
    }
}

# Copy generated headers (cvconfig.h, opencv2/opencv_modules.hpp)
$generatedHeaders = Join-Path $opencvBuildModules "opencv2"
if (Test-Path $generatedHeaders) {
    $opencv2Dst = Join-Path $includeDst "opencv2"
    if (-not (Test-Path $opencv2Dst)) {
        New-Item -ItemType Directory -Path $opencv2Dst -Force | Out-Null
    }
    Copy-Item -Path "$generatedHeaders\*.h*" -Destination $opencv2Dst -Force -ErrorAction SilentlyContinue
}

# Copy libraries
$libSrc = Join-Path $opencvBuildModules "lib\Release"
if (Test-Path $libSrc) {
    Copy-Item -Path "$libSrc\*.lib" -Destination $libDst -Force
}

# Copy DLLs
$binSrc = Join-Path $opencvBuildModules "bin\Release"
if (Test-Path $binSrc) {
    Copy-Item -Path "$binSrc\*.dll" -Destination $binDst -Force
}

# Create a marker file with version info
$markerContent = @"
OpenCV CUDA Pre-built Installation
Built: $(Get-Date -Format "yyyy-MM-dd HH:mm:ss")
CUDA: $cudaPath
"@
Set-Content -Path (Join-Path $InstallDir "BUILD_INFO.txt") -Value $markerContent

Write-Host ""
Write-Host "============================================" -ForegroundColor Green
Write-Host "  OpenCV CUDA Build Complete!" -ForegroundColor Green  
Write-Host "============================================" -ForegroundColor Green
Write-Host ""
Write-Host "Pre-built OpenCV installed to: $InstallDir" -ForegroundColor Cyan
Write-Host ""
Write-Host "Now when you run cmake configure, it will detect this" -ForegroundColor Yellow
Write-Host "pre-built version and skip CUDA compilation!" -ForegroundColor Yellow
Write-Host ""
Write-Host "Next steps:" -ForegroundColor White
Write-Host "  1. Delete your current build folder: juce\build-ninja-release" -ForegroundColor Gray
Write-Host "  2. Reconfigure: cmake -B juce\build-ninja-release ..." -ForegroundColor Gray
Write-Host "  3. Build as usual (CUDA will NOT rebuild)" -ForegroundColor Gray
Write-Host ""
