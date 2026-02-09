cmake_minimum_required(VERSION 3.22)
project(ColliderAudioEngine VERSION 0.1.0)

if(MSVC)
    set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} /Z7")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /Z7")
endif()

# ==============================================================================
# [OPTIONAL] Enable sccache for faster C/C++/CUDA rebuilds
# ==============================================================================
# Add an option to control sccache usage, default to ON
option(ENABLE_SCCACHE "Enable sccache if found for faster builds" ON)

if(ENABLE_SCCACHE)
    find_program(SCCACHE_EXECUTABLE sccache)
    if(SCCACHE_EXECUTABLE)
        message(STATUS "✓ sccache found, enabling compiler cache: ${SCCACHE_EXECUTABLE}")
        set(CMAKE_C_COMPILER_LAUNCHER ${SCCACHE_EXECUTABLE})
        set(CMAKE_CXX_COMPILER_LAUNCHER ${SCCACHE_EXECUTABLE})
        set(CMAKE_CUDA_COMPILER_LAUNCHER ${SCCACHE_EXECUTABLE})
    else()
        message(WARNING "sccache not found in PATH. Compiler cache is DISABLED. \
                         Install with 'brew install sccache' or 'cargo install sccache' for faster builds.")
    endif()
else()
    message(STATUS "sccache disabled by user (ENABLE_SCCACHE=OFF).")
endif()
# ==============================================================================

# --- TTS Integration (Piper) ---

# 1. Set paths to pre-built Piper and ONNX Runtime
set(PIPER_DIR "${CMAKE_SOURCE_DIR}/../vendor/piper/piper" CACHE PATH "Path to Piper TTS")
set(ONNXRUNTIME_DIR "${CMAKE_SOURCE_DIR}/../vendor/onnxruntime" CACHE PATH "Path to ONNX Runtime")

if(NOT EXISTS "${PIPER_DIR}")
    message(WARNING "Piper TTS not found at ${PIPER_DIR}. TTS features will be limited.")
endif()

if(NOT EXISTS "${ONNXRUNTIME_DIR}")
    message(WARNING "ONNX Runtime not found at ${ONNXRUNTIME_DIR}. TTS features will be limited.")
endif()

# 2. Add SoundTouch library
set(SOUNDTOUCH_SOURCE_DIR "${CMAKE_SOURCE_DIR}/../soundtouch/source")
add_library(soundtouch STATIC
    ${SOUNDTOUCH_SOURCE_DIR}/SoundTouch/AAFilter.cpp
    ${SOUNDTOUCH_SOURCE_DIR}/SoundTouch/BPMDetect.cpp
    ${SOUNDTOUCH_SOURCE_DIR}/SoundTouch/FIFOSampleBuffer.cpp
    ${SOUNDTOUCH_SOURCE_DIR}/SoundTouch/FIRFilter.cpp
    ${SOUNDTOUCH_SOURCE_DIR}/SoundTouch/InterpolateCubic.cpp
    ${SOUNDTOUCH_SOURCE_DIR}/SoundTouch/InterpolateLinear.cpp
    ${SOUNDTOUCH_SOURCE_DIR}/SoundTouch/InterpolateShannon.cpp
    ${SOUNDTOUCH_SOURCE_DIR}/SoundTouch/PeakFinder.cpp
    ${SOUNDTOUCH_SOURCE_DIR}/SoundTouch/RateTransposer.cpp
    ${SOUNDTOUCH_SOURCE_DIR}/SoundTouch/SoundTouch.cpp
    ${SOUNDTOUCH_SOURCE_DIR}/SoundTouch/TDStretch.cpp
    ${SOUNDTOUCH_SOURCE_DIR}/SoundTouch/cpu_detect_x86.cpp
    ${SOUNDTOUCH_SOURCE_DIR}/SoundTouch/mmx_optimized.cpp
    ${SOUNDTOUCH_SOURCE_DIR}/SoundTouch/sse_optimized.cpp
)
target_include_directories(soundtouch PUBLIC 
    ${CMAKE_SOURCE_DIR}/../soundtouch/include
    ${CMAKE_SOURCE_DIR}/../soundtouch/source/SoundTouch
)

# ==============================================================================
# STK (Synthesis Toolkit) - Physical Modeling Synthesis
# ==============================================================================
set(STK_SOURCE_DIR "${CMAKE_SOURCE_DIR}/../vendor/stk" CACHE PATH "Path to STK source")

# Force recheck by unsetting STK_FOUND first (in case it was cached from a previous run)
unset(STK_FOUND CACHE)

# Check if STK header exists
if(NOT EXISTS "${STK_SOURCE_DIR}/include/Stk.h")
    message(WARNING "STK not found at ${STK_SOURCE_DIR}/include/Stk.h. STK modules will be disabled.")
    set(STK_FOUND FALSE CACHE BOOL "STK library found")
else()
    message(STATUS "✓ STK found at ${STK_SOURCE_DIR}")
    set(STK_FOUND TRUE CACHE BOOL "STK library found")
    
    # STK requires RAWWAVES path for some instruments
    # Set to vendor/stk/rawwaves/ if it exists, or disable rawwaves-dependent instruments
    set(STK_RAWWAVES_PATH "${STK_SOURCE_DIR}/rawwaves" CACHE PATH "Path to STK rawwaves")
    
    # Log STK paths that will be copied (BEFORE compilation)
    message(STATUS "  STK Source Directory: ${STK_SOURCE_DIR}")
    if(EXISTS "${STK_SOURCE_DIR}/rawwaves")
        message(STATUS "  STK rawwaves Source: ${STK_SOURCE_DIR}/rawwaves (will be copied to output/rawwaves)")
    else()
        message(WARNING "  STK rawwaves not found at ${STK_SOURCE_DIR}/rawwaves")
    endif()
    
    # Create STK library target
    add_library(stk STATIC
        # Core STK
        ${STK_SOURCE_DIR}/src/Stk.cpp
        # String Instruments
        ${STK_SOURCE_DIR}/src/Plucked.cpp
        ${STK_SOURCE_DIR}/src/Bowed.cpp
        ${STK_SOURCE_DIR}/src/Sitar.cpp
        ${STK_SOURCE_DIR}/src/Mandolin.cpp
        # Wind Instruments
        ${STK_SOURCE_DIR}/src/Flute.cpp
        ${STK_SOURCE_DIR}/src/Clarinet.cpp
        ${STK_SOURCE_DIR}/src/Saxofony.cpp
        ${STK_SOURCE_DIR}/src/Brass.cpp
        # Percussion Instruments
        ${STK_SOURCE_DIR}/src/ModalBar.cpp
        ${STK_SOURCE_DIR}/src/Modal.cpp
        ${STK_SOURCE_DIR}/src/BandedWG.cpp
        ${STK_SOURCE_DIR}/src/Shakers.cpp
        # Utility classes required by instruments
        ${STK_SOURCE_DIR}/src/DelayA.cpp
        ${STK_SOURCE_DIR}/src/DelayL.cpp
        ${STK_SOURCE_DIR}/src/OneZero.cpp
        ${STK_SOURCE_DIR}/src/OnePole.cpp
        ${STK_SOURCE_DIR}/src/Noise.cpp
        ${STK_SOURCE_DIR}/src/BiQuad.cpp
        ${STK_SOURCE_DIR}/src/SineWave.cpp
        ${STK_SOURCE_DIR}/src/ADSR.cpp
        ${STK_SOURCE_DIR}/src/Envelope.cpp
        ${STK_SOURCE_DIR}/src/PoleZero.cpp
        # File I/O classes required by ModalBar and other instruments
        ${STK_SOURCE_DIR}/src/FileRead.cpp
        ${STK_SOURCE_DIR}/src/FileLoop.cpp
        ${STK_SOURCE_DIR}/src/FileWvIn.cpp
    )
    
    target_include_directories(stk PUBLIC
        ${STK_SOURCE_DIR}/include
    )
    
    # STK may need platform-specific defines
    if(WIN32)
        target_compile_definitions(stk PRIVATE
            __WINDOWS_DS__
            WIN32
            _USE_MATH_DEFINES  # Required for M_SQRT1_2, M_PI, etc. on Windows
        )
    endif()
    
    # Link against system audio libraries if needed
    if(UNIX AND NOT APPLE)
        target_link_libraries(stk PRIVATE asound)
    endif()
endif()

# ==============================================================================
# Essentia - Music Information Retrieval and Audio Analysis
# ==============================================================================
option(ENABLE_ESSENTIA "Enable Essentia audio analysis library" ON)

if(NOT ENABLE_ESSENTIA)
    message(STATUS "Essentia disabled (set ENABLE_ESSENTIA=ON to enable)")
    set(ESSENTIA_FOUND FALSE CACHE BOOL "Essentia library found" FORCE)
else()

# Try multiple possible Essentia directory names
set(ESSENTIA_POSSIBLE_NAMES
    "essentia-2.1_beta5"
    "essentia-master"
    "essentia"
)

set(ESSENTIA_DIR "" CACHE PATH "Path to Essentia installation")
set(ESSENTIA_FOUND FALSE)

foreach(ESSENTIA_NAME ${ESSENTIA_POSSIBLE_NAMES})
    set(ESSENTIA_TRY_PATH "${CMAKE_SOURCE_DIR}/../vendor/${ESSENTIA_NAME}")
    if(EXISTS "${ESSENTIA_TRY_PATH}/src/essentia/essentia.h")
        set(ESSENTIA_DIR "${ESSENTIA_TRY_PATH}" CACHE PATH "Path to Essentia installation" FORCE)
        set(ESSENTIA_FOUND TRUE)
        message(STATUS "✓ Essentia found at ${ESSENTIA_DIR}")
        break()
    endif()
endforeach()

if(NOT ESSENTIA_FOUND)
    message(WARNING "Essentia not found in any of: ${ESSENTIA_POSSIBLE_NAMES}. Essentia modules will be disabled.")
    unset(ESSENTIA_FOUND CACHE)
    set(ESSENTIA_FOUND FALSE CACHE BOOL "Essentia library found" FORCE)
else()
    # ESSENTIA_FOUND and ESSENTIA_DIR already set in the loop above
    unset(ESSENTIA_FOUND CACHE)
    set(ESSENTIA_FOUND TRUE CACHE BOOL "Essentia library found" FORCE)
    
    # --------------------------------------------------------------------------
    # Essentia Source Integration (Full build with Streaming Framework)
    # --------------------------------------------------------------------------
    
    # Gather Source Files - Core
    file(GLOB ESSENTIA_CORE_SRCS "${ESSENTIA_DIR}/src/essentia/*.cpp")
    file(GLOB ESSENTIA_UTILS_SRCS "${ESSENTIA_DIR}/src/essentia/utils/*.cpp")
    
    # Gather Streaming Framework Sources (required by most algorithms)
    file(GLOB ESSENTIA_STREAMING_SRCS "${ESSENTIA_DIR}/src/essentia/streaming/*.cpp")
    file(GLOB ESSENTIA_STREAMING_ALGO_SRCS "${ESSENTIA_DIR}/src/essentia/streaming/algorithms/*.cpp")
    
    # Gather Scheduler/Network Sources
    file(GLOB ESSENTIA_SCHEDULER_SRCS "${ESSENTIA_DIR}/src/essentia/scheduler/*.cpp")
    
    # Gather 3rdparty spline Sources
    file(GLOB ESSENTIA_SPLINE_SRCS "${ESSENTIA_DIR}/src/3rdparty/spline/*.cpp")
    
    # Gather Algorithm Sources
    file(GLOB ESSENTIA_ALGO_STD_SRCS "${ESSENTIA_DIR}/src/algorithms/standard/*.cpp")
    file(GLOB ESSENTIA_ALGO_RHYTHM_SRCS "${ESSENTIA_DIR}/src/algorithms/rhythm/*.cpp")
    
    # KissFFT Sources
    set(KISS_FFT_DIR "${ESSENTIA_DIR}/src/3rdparty/kiss_fft130")
    set(KISS_FFT_SRCS 
        "${KISS_FFT_DIR}/kiss_fft.c"
        "${KISS_FFT_DIR}/tools/kiss_fftr.c"
    )
    
    # Filter out unwanted FFT implementations (FFTW, Accelerate - we use KissFFT)
    list(FILTER ESSENTIA_ALGO_STD_SRCS EXCLUDE REGEX ".*fftw.*")
    list(FILTER ESSENTIA_ALGO_STD_SRCS EXCLUDE REGEX ".*ffta.*")
    list(FILTER ESSENTIA_ALGO_STD_SRCS EXCLUDE REGEX ".*ifftw.*")
    list(FILTER ESSENTIA_ALGO_STD_SRCS EXCLUDE REGEX ".*iffta.*")
    
    # Filter out algorithms with external dependencies
    list(FILTER ESSENTIA_ALGO_STD_SRCS EXCLUDE REGEX ".*resample.*")  # libsamplerate
    list(FILTER ESSENTIA_ALGO_STD_SRCS EXCLUDE REGEX ".*tensorfl.*")  # TensorFlow
    list(FILTER ESSENTIA_ALGO_STD_SRCS EXCLUDE REGEX ".*gaia.*")      # Gaia
    
    # Filter out YAML-dependent files (requires libyaml)
    list(FILTER ESSENTIA_UTILS_SRCS EXCLUDE REGEX ".*yaml.*")
    list(FILTER ESSENTIA_UTILS_SRCS EXCLUDE REGEX ".*jsonconvert.*")
    
    # Filter out FFmpeg-dependent audio context (we'll use JUCE for audio I/O)
    list(FILTER ESSENTIA_UTILS_SRCS EXCLUDE REGEX ".*audiocontext.*")
    
    # Filter out test files if any
    list(FILTER ESSENTIA_CORE_SRCS EXCLUDE REGEX ".*test.*")
    
    # Create Static Library
    add_library(essentia_lib STATIC
        ${ESSENTIA_CORE_SRCS}
        ${ESSENTIA_UTILS_SRCS}
        ${ESSENTIA_STREAMING_SRCS}
        ${ESSENTIA_STREAMING_ALGO_SRCS}
        ${ESSENTIA_SCHEDULER_SRCS}
        ${ESSENTIA_SPLINE_SRCS}
        ${ESSENTIA_ALGO_STD_SRCS}
        ${ESSENTIA_ALGO_RHYTHM_SRCS}
        ${KISS_FFT_SRCS}
    )
    
    target_include_directories(essentia_lib PUBLIC
        "${ESSENTIA_DIR}/src"
        "${ESSENTIA_DIR}/src/essentia"                     # For "algorithm.h", "types.h", etc.
        "${ESSENTIA_DIR}/src/essentia/streaming"           # For "streamingalgorithmwrapper.h"
        "${ESSENTIA_DIR}/src/essentia/streaming/algorithms" # For "vectoroutput.h", "poolstorage.h"
        "${ESSENTIA_DIR}/src/essentia/scheduler"           # For "network.h"
        "${ESSENTIA_DIR}/src/essentia/utils"               # For "bpfutil.h"
        "${ESSENTIA_DIR}/src/3rdparty"                     # For "kiss_fft130/kiss_fft.h"
        "${ESSENTIA_DIR}/src/3rdparty/spline"              # For "splineutil.h"
        "${KISS_FFT_DIR}"
        "${KISS_FFT_DIR}/tools"
    )
    
    # Defines for Essentia
    target_compile_definitions(essentia_lib PUBLIC
        __WINDOWS_DS__
        WIN32
        _USE_MATH_DEFINES
        NOMINMAX               # Prevent Windows min/max macros
        kiss_fft_scalar=float  # Essentia uses float by default
        HAVE_KISSFFT           # Use KissFFT backend
    )
    
    # Suppress some warnings for third-party code
    if(MSVC)
        target_compile_options(essentia_lib PRIVATE /W0)
    endif()
    
    # Set ESSENTIA_LIBRARY variable for linking
    set(ESSENTIA_LIBRARY essentia_lib)
    
    message(STATUS "  Essentia configured for source build (KissFFT + Streaming)")
    
    # Log Copy Commands
    if(EXISTS "${ESSENTIA_DIR}/models")
        message(STATUS "  Essentia models Source: ${ESSENTIA_DIR}/models (will be copied to output/essentia/models)")
    endif()
    if(EXISTS "${ESSENTIA_DIR}/data")
        message(STATUS "  Essentia data Source: ${ESSENTIA_DIR}/data (will be copied to output/essentia/data)")
    endif()
endif()

endif() # ENABLE_ESSENTIA

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# ------------------------------------------------------------------------------
# ImNodes Zoom Integration Toggle (fork-based)
# ------------------------------------------------------------------------------
# If PRESET_CREATOR_IMNODES_ZOOM is ON and both IMNODES_FORK_URL and IMNODES_FORK_TAG
# are provided, we'll fetch imnodes from the fork at the specified commit/tag.
# Otherwise, we fall back to upstream imnodes at the pinned upstream tag.
option(PRESET_CREATOR_IMNODES_ZOOM "Use fork of imnodes with zoom feature" OFF)
set(IMNODES_FORK_URL "" CACHE STRING "Fork URL for imnodes (with zoom commit)")
set(IMNODES_FORK_TAG "" CACHE STRING "Fork commit/tag for imnodes (with zoom)")

# ==============================================================================
# Third-Party Dependencies (Fetch Only)
# ==============================================================================

FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 7.0.9
)
set(JUCE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(JUCE_BUILD_EXTRAS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(JUCE)

FetchContent_Declare(imgui_fc
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    # Use docking branch to ensure PlatformIO/viewport APIs are present
    GIT_TAG docking
)
FetchContent_MakeAvailable(imgui_fc)

# imnodes uses find_package(imgui) in its CMake; build as sources instead
if(PRESET_CREATOR_IMNODES_ZOOM AND IMNODES_FORK_URL AND IMNODES_FORK_TAG)
    message(STATUS "Using forked imnodes for zoom: ${IMNODES_FORK_URL} @ ${IMNODES_FORK_TAG}")
    FetchContent_Declare(imnodes_fc
        GIT_REPOSITORY ${IMNODES_FORK_URL}
        GIT_TAG ${IMNODES_FORK_TAG}
    )
else()
    message(STATUS "Using upstream imnodes (no zoom fork)")
    FetchContent_Declare(imnodes_fc
        GIT_REPOSITORY https://github.com/Nelarius/imnodes.git
        GIT_TAG b2ec254ce576ac3d42dfb7aef61deadbff8e7211
    )
endif()
FetchContent_GetProperties(imnodes_fc)
if(NOT imnodes_fc_POPULATED)
  FetchContent_Populate(imnodes_fc)
endif()

# imgui_juce backend (dedicated JUCE bridge for ImGui)
FetchContent_Declare(imgui_juce_fc
    GIT_REPOSITORY https://github.com/Krasjet/imgui_juce.git
    GIT_TAG master
)
FetchContent_MakeAvailable(imgui_juce_fc)

# --------------------------------------------------------------
# Box2D (2D physics engine for physics-based audio module)
# --------------------------------------------------------------
FetchContent_Declare(box2d_fc
    GIT_REPOSITORY https://github.com/erincatto/box2d.git
    GIT_TAG v2.4.1
)
set(BOX2D_BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(BOX2D_BUILD_TESTBED OFF CACHE BOOL "" FORCE)
set(BOX2D_BUILD_DOCS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(box2d_fc)

# --------------------------------------------------------------
# GLM (OpenGL Mathematics library for 3D vector/matrix math)
# --------------------------------------------------------------
FetchContent_Declare(
  glm
  GIT_REPOSITORY https://github.com/g-truc/glm.git
  GIT_TAG        1.0.1 # Latest stable version
)
FetchContent_MakeAvailable(glm)

# --------------------------------------------------------------
# nlohmann/json (required by tinygltf)
# --------------------------------------------------------------
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.11.3 # A recent stable version
)
FetchContent_MakeAvailable(nlohmann_json)

# --------------------------------------------------------------
# tinygltf (glTF 2.0 file loader for animation system)
# --------------------------------------------------------------
FetchContent_Declare(
  tinygltf
  GIT_REPOSITORY https://github.com/syoyo/tinygltf.git
  GIT_TAG        v2.8.20 # A recent stable version
)
FetchContent_MakeAvailable(tinygltf)

# --------------------------------------------------------------
# ufbx (FBX file loader for animation system)
# --------------------------------------------------------------
FetchContent_Declare(
  ufbx
  GIT_REPOSITORY https://github.com/ufbx/ufbx.git
  GIT_TAG        v0.10.0 # A recent stable version of ufbx
)
FetchContent_MakeAvailable(ufbx)

# Get the path to the downloaded ufbx source code
FetchContent_GetProperties(ufbx SOURCE_DIR ufbx_SOURCE_DIR)

# Create a dedicated static library target for ufbx (single-header library needs separate compilation unit)
add_library(ufbx_static STATIC ${ufbx_SOURCE_DIR}/ufbx.c)
target_include_directories(ufbx_static PUBLIC ${ufbx_SOURCE_DIR})

# ==============================================================================
# CUDA & cuDNN Configuration (MUST be done before OpenCV)
# ==============================================================================
option(ENABLE_CUDA "Enable CUDA support" ON)

if(ENABLE_CUDA)
    # Find the CUDA Toolkit. This will use the default installation path or environment
    # variables. This is essential for OpenCV's configuration to detect CUDA correctly.
    find_package(CUDAToolkit QUIET)
    
    if(NOT CUDAToolkit_FOUND)
        message(WARNING "CUDA Toolkit not found. Disabling CUDA support.")
        set(ENABLE_CUDA OFF)
    endif()
endif()

if(ENABLE_CUDA)

# Find CUDA runtime library for direct linking if CUDAToolkit::cudart target is not available
if(MSVC)
    find_library(CUDA_CUDART_LIBRARY
        NAMES cudart_static.lib cudart.lib
        PATHS "${CUDAToolkit_LIBRARY_DIR}"
        NO_DEFAULT_PATH
    )
else()
    find_library(CUDA_CUDART_LIBRARY
        NAMES cudart
        PATHS "${CUDAToolkit_LIBRARY_DIR}"
        NO_DEFAULT_PATH
    )
endif()

# --- NPP (NVIDIA Performance Primitives) Configuration ---
# Explicitly set CUDA and NPP paths for OpenCV to find them
set(CUDA_TOOLKIT_ROOT_DIR "${CUDAToolkit_TARGET_DIR}" CACHE PATH "CUDA Toolkit root directory")
set(CMAKE_CUDA_TOOLKIT_INCLUDE_DIRECTORIES "${CUDAToolkit_INCLUDE_DIRS}" CACHE PATH "CUDA Toolkit include directories")

# Set NPP include and library directories explicitly
# NPP headers are in the CUDA Toolkit include directory
set(CUDA_INCLUDE_DIRS "${CUDAToolkit_INCLUDE_DIRS}" CACHE PATH "CUDA include directories")
set(CUDA_NPP_LIBRARY_ROOT_DIR "${CUDAToolkit_LIBRARY_DIR}" CACHE PATH "NPP library root directory")

# Provide hints for cuDNN path.
# First check if cuDNN is integrated into CUDA Toolkit (recommended approach)
# Then fall back to standalone cuDNN installation
set(CUDNN_FOUND FALSE)

# CRITICAL FIX: CUDAToolkit_INCLUDE_DIRS is a LIST, extract the first element
list(GET CUDAToolkit_INCLUDE_DIRS 0 CUDA_MAIN_INCLUDE_DIR)

# Check in CUDA Toolkit directory first
if(EXISTS "${CUDA_MAIN_INCLUDE_DIR}/cudnn.h")
    set(CUDNN_INCLUDE_DIR "${CUDA_MAIN_INCLUDE_DIR}" CACHE PATH "Path to cuDNN include directory")
    set(CUDNN_LIBRARY "${CUDAToolkit_LIBRARY_DIR}/cudnn.lib" CACHE FILEPATH "Path to cuDNN library")
    set(CUDNN_FOUND TRUE)
    message(STATUS "Found cuDNN integrated into CUDA Toolkit at ${CUDA_MAIN_INCLUDE_DIR}")
# Fall back to standalone cuDNN installation
elseif(EXISTS "C:/Program Files/NVIDIA/CUDNN/v9.14/include/cudnn.h")
    set(CUDNN_INCLUDE_DIR "C:/Program Files/NVIDIA/CUDNN/v9.14/include" CACHE PATH "Path to cuDNN include directory")
    set(CUDNN_LIBRARY "C:/Program Files/NVIDIA/CUDNN/v9.14/lib/x64/cudnn.lib" CACHE FILEPATH "Path to cuDNN library")
    set(CUDNN_FOUND TRUE)
    message(STATUS "Found standalone cuDNN at C:/Program Files/NVIDIA/CUDNN/v9.14/")
endif()

    # Configure OpenCV based on cuDNN availability
    if(CUDNN_FOUND)
        set(WITH_CUDNN ON CACHE BOOL "" FORCE)
        set(OPENCV_DNN_CUDA ON CACHE BOOL "" FORCE)
    else()
        message(WARNING "cuDNN not found. OpenCV will build without cuDNN support.")
        message(WARNING "Disabling OPENCV_DNN_CUDA since cuDNN is required for CUDA DNN backend.")
        set(WITH_CUDNN OFF CACHE BOOL "" FORCE)
        set(OPENCV_DNN_CUDA OFF CACHE BOOL "" FORCE)
    endif()
else()
    # CUDA Disabled
    set(CUDNN_FOUND FALSE)
    set(WITH_CUDNN OFF)
    set(OPENCV_DNN_CUDA OFF)
endif()

# ==============================================================================
# OpenCV (Computer Vision Library - with CUDA Support)
# ==============================================================================
option(ENABLE_OPENCV "Enable OpenCV support" ON)

if(ENABLE_OPENCV)
# NEW ISOLATION STRATEGY:
# 1. Try to find pre-built OpenCV (from standalone builder)
# 2. Fall back to building from source if not found
# 
# This prevents OpenCV CUDA from rebuilding when CMakeLists.txt changes!
# 
# To build OpenCV once: .\build_opencv_cuda_once.ps1
# ==============================================================================

# Add CUDA include directories for all builds
include_directories(SYSTEM ${CUDAToolkit_INCLUDE_DIRS})

# Check for standalone pre-built OpenCV
# Try multiple possible locations and structures
set(OPENCV_PREBUILT FALSE)
set(OPENCV_HAS_RELEASE FALSE)
set(OPENCV_HAS_DEBUG FALSE)

# Possible install prefixes (in order of preference)
set(OPENCV_POSSIBLE_PATHS
    "${CMAKE_SOURCE_DIR}/opencv_cuda_install"           # Inside juce folder
    "${CMAKE_SOURCE_DIR}/../opencv_cuda_install"        # At repo root
)

foreach(PREFIX_PATH ${OPENCV_POSSIBLE_PATHS})
    if(NOT OPENCV_PREBUILT)
        # Check different lib directory structures OpenCV uses
        set(LIB_SEARCH_PATHS
            "${PREFIX_PATH}/x64/vc17/staticlib"    # Static libs (MSVC 2022)
            "${PREFIX_PATH}/x64/vc17/lib"          # Dynamic libs (MSVC 2022)
            "${PREFIX_PATH}/lib"                   # Standard layout
        )
        
        foreach(LIB_PATH ${LIB_SEARCH_PATHS})
            if(EXISTS "${LIB_PATH}/opencv_world4130.lib")
                set(OPENCV_INSTALL_PREFIX "${PREFIX_PATH}")
                set(OPENCV_LIB_DIR "${LIB_PATH}")
                set(OPENCV_HAS_RELEASE TRUE)
                break()
            endif()
        endforeach()
        
        # Also check for debug libs
        foreach(LIB_PATH ${LIB_SEARCH_PATHS})
            if(EXISTS "${LIB_PATH}/opencv_world4130d.lib")
                set(OPENCV_HAS_DEBUG TRUE)
                break()
            endif()
        endforeach()
        
        if(OPENCV_HAS_RELEASE OR OPENCV_HAS_DEBUG)
            set(OPENCV_PREBUILT TRUE)
        endif()
    endif()
endforeach()

if(OPENCV_HAS_RELEASE OR OPENCV_HAS_DEBUG)
    # Found standalone OpenCV - use find_package() to get everything
    set(OPENCV_PREBUILT TRUE)
    
    message(STATUS "")
    message(STATUS "========================================")
    message(STATUS "  Found Pre-Built OpenCV with CUDA!")
    message(STATUS "========================================")
    message(STATUS "Location: ${OPENCV_INSTALL_PREFIX}")
    message(STATUS "")
    if(OPENCV_HAS_RELEASE)
        message(STATUS "✓ Release configuration available")
    endif()
    if(OPENCV_HAS_DEBUG)
        message(STATUS "✓ Debug configuration available")
    endif()
    message(STATUS "")
    message(STATUS "✓ Using cached build (no rebuild needed!)")
    message(STATUS "  You can modify CMakeLists.txt without rebuilding OpenCV!")
    message(STATUS "")
    
    # Use find_package to get ALL transitive dependencies automatically
    # Try multiple possible config locations
    if(EXISTS "${OPENCV_INSTALL_PREFIX}/OpenCVConfig.cmake")
        set(OpenCV_DIR "${OPENCV_INSTALL_PREFIX}")
    elseif(EXISTS "${OPENCV_INSTALL_PREFIX}/x64/vc17/staticlib/OpenCVConfig.cmake")
        set(OpenCV_DIR "${OPENCV_INSTALL_PREFIX}/x64/vc17/staticlib")
    elseif(EXISTS "${OPENCV_INSTALL_PREFIX}/lib/cmake/OpenCV/OpenCVConfig.cmake")
        set(OpenCV_DIR "${OPENCV_INSTALL_PREFIX}/lib/cmake/OpenCV")
    endif()
    message(STATUS "  - OpenCV_DIR: ${OpenCV_DIR}")
    find_package(OpenCV 4.13 REQUIRED)
    
    if(NOT OpenCV_FOUND)
        message(FATAL_ERROR "OpenCV installation found but CMake config is broken!")
    endif()
    
    message(STATUS "✓ OpenCV CMake config loaded successfully")
    message(STATUS "  - Include dirs: ${OpenCV_INCLUDE_DIRS}")
    message(STATUS "  - Libraries: ${OpenCV_LIBS}")
    message(STATUS "  - Version: ${OpenCV_VERSION}")
    
    # Set paths for compatibility with rest of CMakeLists
    set(opencv_SOURCE_DIR "${OPENCV_INSTALL_PREFIX}/include")
    set(OPENCV_BUILD_DIR "${OPENCV_INSTALL_PREFIX}/include")
    set(opencv_contrib_SOURCE_DIR "${OPENCV_INSTALL_PREFIX}/include")
    # Pre-built OpenCV has haarcascades in /etc/ not /data/
    set(OPENCV_DATA_DIR "${OPENCV_INSTALL_PREFIX}/etc")
    
else()
    message(STATUS "")
    message(STATUS "========================================")
    message(STATUS "  Pre-Built OpenCV NOT Found")
    message(STATUS "========================================")
    message(STATUS "Will build from source (takes 30-45 minutes)")
    message(STATUS "")
    message(STATUS "To avoid future rebuilds, run:")
    message(STATUS "  .\\build_opencv_cuda_once.ps1")
    message(STATUS "")
    
    # Add /FS flag for MSVC BEFORE declaring OpenCV
    if(MSVC)
        set(SAVED_CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
        set(SAVED_CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
        set(CMAKE_CXX_FLAGS_INIT "${CMAKE_CXX_FLAGS_INIT} /FS" CACHE STRING "" FORCE)
        set(CMAKE_C_FLAGS_INIT "${CMAKE_C_FLAGS_INIT} /FS" CACHE STRING "" FORCE)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /FS")
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /FS")
    endif()
    
    # Fetch OpenCV from source
    FetchContent_Declare(
      opencv
      GIT_REPOSITORY https://github.com/opencv/opencv.git
      GIT_TAG        4.x
    )
    
    FetchContent_Declare(
      opencv_contrib
      GIT_REPOSITORY https://github.com/opencv/opencv_contrib.git
      GIT_TAG        4.x
    )
    
    # Populate opencv_contrib first
    FetchContent_GetProperties(opencv_contrib)
    if(NOT opencv_contrib_POPULATED)
        FetchContent_Populate(opencv_contrib)
    endif()
    FetchContent_GetProperties(opencv_contrib SOURCE_DIR opencv_contrib_SOURCE_DIR)
    
    set(OPENCV_EXTRA_MODULES_PATH "${opencv_contrib_SOURCE_DIR}/modules" CACHE PATH "" FORCE)
    
    # OpenCV Build Configuration
    set(BUILD_SHARED_LIBS OFF)
    set(BUILD_opencv_world ON)
    set(BUILD_TESTS OFF)
    set(BUILD_PERF_TESTS OFF)
    set(BUILD_EXAMPLES OFF)
    set(BUILD_opencv_apps OFF)
    set(ENABLE_PRECOMPILED_HEADERS OFF)
    set(BUILD_WITH_STATIC_CRT OFF)
    set(WITH_IPP OFF)
    
    # CUDA Configuration
    set(WITH_CUDA ON CACHE BOOL "" FORCE)
    set(WITH_NVIDIA_NPP ON CACHE BOOL "" FORCE)
    set(WITH_NVCUVID ON CACHE BOOL "" FORCE)
    set(WITH_NVCUVENC ON CACHE BOOL "" FORCE)
    
    set(CUDA_TOOLKIT_ROOT_DIR "${CUDAToolkit_TARGET_DIR}" CACHE PATH "" FORCE)
    set(CUDA_INCLUDE_DIRS "${CUDAToolkit_INCLUDE_DIRS}" CACHE PATH "" FORCE)
    
    # CUDA Architecture
    set(CUDA_ARCH_BIN "8.6;8.9;12.0")
    set(CUDA_ARCH_PTX "12.0")
    
    # Required modules
    set(BUILD_opencv_core ON)
    set(BUILD_opencv_dnn ON)
    set(BUILD_opencv_features2d ON)
    set(BUILD_opencv_highgui ON)
    set(BUILD_opencv_imgcodecs ON)
    set(BUILD_opencv_imgproc ON)
    set(BUILD_opencv_objdetect ON)
    set(BUILD_opencv_tracking ON)
    set(BUILD_opencv_video ON)
    set(BUILD_opencv_videoio ON)
    set(BUILD_opencv_calib3d ON)
    set(OPENCV_DNN_CAFFE ON)
    
    # Build OpenCV
    FetchContent_MakeAvailable(opencv)
    
    # Get build directory
    FetchContent_GetProperties(opencv)
    if(NOT opencv_POPULATED)
        FetchContent_Populate(opencv)
    endif()
    set(OPENCV_BUILD_DIR "${CMAKE_BINARY_DIR}/_deps/opencv-build")
    
    # Generate opencv_modules.hpp if missing
    set(OPENCV_MODULES_HPP "${OPENCV_BUILD_DIR}/opencv2/opencv_modules.hpp")
    if(NOT EXISTS "${OPENCV_MODULES_HPP}")
        message(STATUS "Generating opencv_modules.hpp...")
        file(MAKE_DIRECTORY "${OPENCV_BUILD_DIR}/opencv2")
        file(WRITE "${OPENCV_MODULES_HPP}"
"/*
 * Auto-generated by CMake
 */

#ifndef OPENCV_MODULES_HPP
#define OPENCV_MODULES_HPP

#define HAVE_OPENCV_CALIB3D
#define HAVE_OPENCV_CORE
#define HAVE_OPENCV_DNN
#define HAVE_OPENCV_FEATURES2D
#define HAVE_OPENCV_FLANN
#define HAVE_OPENCV_GAPI
#define HAVE_OPENCV_HIGHGUI
#define HAVE_OPENCV_IMGCODECS
#define HAVE_OPENCV_IMGPROC
#define HAVE_OPENCV_ML
#define HAVE_OPENCV_OBJDETECT
#define HAVE_OPENCV_PHOTO
#define HAVE_OPENCV_STITCHING
#define HAVE_OPENCV_VIDEO
#define HAVE_OPENCV_VIDEOIO
#define HAVE_OPENCV_WORLD

#endif
")
    endif()
    
    # Restore flags
    if(MSVC)
        set(CMAKE_CXX_FLAGS "${SAVED_CMAKE_CXX_FLAGS} /FS")
        set(CMAKE_C_FLAGS "${SAVED_CMAKE_C_FLAGS} /FS")
    endif()
    
    # Source-built OpenCV has haarcascades in /data/
    set(OPENCV_DATA_DIR "${opencv_SOURCE_DIR}/data")
endif()

# ==============================================================================
# Post-Configuration (Common to both pre-built and source builds)
# ==============================================================================
# Set OpenCV link libraries based on whether we're using pre-built or source-built

if(OPENCV_PREBUILT)
    # Pre-built: Our pre-built uses opencv_world (single combined library)
    # We need to link opencv_world directly, not the individual modules from OpenCV_LIBS
    # because we built with BUILD_opencv_world=ON
    find_library(OPENCV_WORLD_LIB 
        NAMES opencv_world4130 opencv_world
        PATHS "${OPENCV_LIB_DIR}" "${OPENCV_INSTALL_PREFIX}/lib"
        NO_DEFAULT_PATH
    )
    if(OPENCV_WORLD_LIB)
        set(OPENCV_LINK_LIBS "${OPENCV_WORLD_LIB}")
        message(STATUS "Using pre-built OpenCV: ${OPENCV_WORLD_LIB}")
    else()
        # Fallback to OpenCV_LIBS if world lib not found (shouldn't happen)
        set(OPENCV_LINK_LIBS ${OpenCV_LIBS})
        message(STATUS "Using pre-built OpenCV modules: ${OpenCV_LIBS}")
    endif()
    
    # Pre-built OpenCV includes 3rdparty libraries in the same directory
    # These MUST be linked for static opencv_world to resolve all symbols
    set(OPENCV_3RDPARTY_LIBS "")
    set(OPENCV_3RDPARTY_NAMES
        zlib
        libpng
        libjpeg-turbo
        libtiff
        libwebp
        libopenjp2
        IlmImf           # OpenEXR
        libprotobuf      # For DNN/Caffe
        ade              # For DNN graph
        ittnotify        # Intel threading
    )
    
    foreach(LIB_NAME ${OPENCV_3RDPARTY_NAMES})
        find_library(OPENCV_3RDPARTY_${LIB_NAME}
            NAMES ${LIB_NAME}
            PATHS "${OPENCV_LIB_DIR}"
            NO_DEFAULT_PATH
        )
        if(OPENCV_3RDPARTY_${LIB_NAME})
            list(APPEND OPENCV_3RDPARTY_LIBS "${OPENCV_3RDPARTY_${LIB_NAME}}")
            message(STATUS "  Found 3rdparty: ${LIB_NAME}")
        endif()
    endforeach()
    
    if(OPENCV_3RDPARTY_LIBS)
        message(STATUS "Pre-built OpenCV 3rdparty libs: ${OPENCV_3RDPARTY_LIBS}")
    endif()
else()
    # Source-built: Use the opencv_world target directly
    # OpenCV's opencv_world static library requires all 3rdparty dependencies to be linked explicitly
    set(OPENCV_LINK_LIBS opencv_world)
    message(STATUS "Using source-built OpenCV: opencv_world target")
    
    # Collect OpenCV's 3rdparty library targets that opencv_world depends on
    # These are built by OpenCV's CMake and need to be linked for static builds
    set(OPENCV_3RDPARTY_LIBS "")
    
    # Image codecs
    if(TARGET libpng)
        list(APPEND OPENCV_3RDPARTY_LIBS libpng)
    endif()
    if(TARGET libjpeg-turbo)
        list(APPEND OPENCV_3RDPARTY_LIBS libjpeg-turbo)
    endif()
    if(TARGET libtiff)
        list(APPEND OPENCV_3RDPARTY_LIBS libtiff)
    endif()
    if(TARGET libwebp)
        list(APPEND OPENCV_3RDPARTY_LIBS libwebp)
    endif()
    if(TARGET libopenjp2)
        list(APPEND OPENCV_3RDPARTY_LIBS libopenjp2)
    endif()
    if(TARGET IlmImf)
        list(APPEND OPENCV_3RDPARTY_LIBS IlmImf)
    endif()
    
    # Compression
    if(TARGET zlib)
        list(APPEND OPENCV_3RDPARTY_LIBS zlib)
    endif()
    
    # DNN dependencies
    if(TARGET libprotobuf)
        list(APPEND OPENCV_3RDPARTY_LIBS libprotobuf)
    endif()
    if(TARGET ade)
        list(APPEND OPENCV_3RDPARTY_LIBS ade)
    endif()
    
    # Intel performance library
    if(TARGET ittnotify)
        list(APPEND OPENCV_3RDPARTY_LIBS ittnotify)
    endif()
    
    if(OPENCV_3RDPARTY_LIBS)
        message(STATUS "OpenCV 3rdparty libs to link: ${OPENCV_3RDPARTY_LIBS}")
    endif()
endif()

# ==============================================================================
# CUDA Libraries for OpenCV DNN CUDA Backend
# ==============================================================================
# When OpenCV is built with CUDA support, we need cuBLAS and cuDNN for DNN operations
set(OPENCV_CUDA_LIBS "")

if(ENABLE_CUDA AND CUDAToolkit_FOUND)
    # cuBLAS - required for DNN CUDA backend
    if(TARGET CUDA::cublas)
        list(APPEND OPENCV_CUDA_LIBS CUDA::cublas)
        message(STATUS "✓ cuBLAS found for OpenCV DNN")
    elseif(CUDA_cublas_LIBRARY)
        list(APPEND OPENCV_CUDA_LIBS ${CUDA_cublas_LIBRARY})
        message(STATUS "✓ cuBLAS found at: ${CUDA_cublas_LIBRARY}")
    else()
        find_library(CUBLAS_LIB NAMES cublas HINTS "${CUDAToolkit_LIBRARY_DIR}" "${CUDA_TOOLKIT_ROOT_DIR}/lib/x64")
        if(CUBLAS_LIB)
            list(APPEND OPENCV_CUDA_LIBS ${CUBLAS_LIB})
            message(STATUS "✓ cuBLAS found at: ${CUBLAS_LIB}")
        endif()
    endif()
    
    # cuDNN - required for DNN CUDA backend
    if(CUDNN_LIBRARY)
        list(APPEND OPENCV_CUDA_LIBS ${CUDNN_LIBRARY})
        message(STATUS "✓ cuDNN for OpenCV: ${CUDNN_LIBRARY}")
    elseif(CUDNN_FOUND)
        find_library(CUDNN_LIB NAMES cudnn cudnn8 HINTS "${CUDNN_INCLUDE_DIR}/../lib/x64" "${CUDAToolkit_LIBRARY_DIR}")
        if(CUDNN_LIB)
            list(APPEND OPENCV_CUDA_LIBS ${CUDNN_LIB})
            message(STATUS "✓ cuDNN found at: ${CUDNN_LIB}")
        endif()
    endif()
    
    # CUDA runtime (usually already linked, but ensure it's there)
    if(TARGET CUDA::cudart)
        list(APPEND OPENCV_CUDA_LIBS CUDA::cudart)
    endif()
    
    if(OPENCV_CUDA_LIBS)
        message(STATUS "OpenCV CUDA libs to link: ${OPENCV_CUDA_LIBS}")
    endif()
endif()

# Explicitly add opencv_contrib cudacodec include (after source dirs are populated)
if(EXISTS "${opencv_contrib_SOURCE_DIR}/modules/cudacodec/include")
    include_directories(SYSTEM "${opencv_contrib_SOURCE_DIR}/modules/cudacodec/include")
    message(STATUS "✓ Added cudacodec include path from: ${opencv_contrib_SOURCE_DIR}/modules/cudacodec/include")
endif()

else()
    message(STATUS "OpenCV disabled by user (ENABLE_OPENCV=OFF).")
    set(OPENCV_LINK_LIBS "")
endif()

# ==============================================================================
# Define CUDA Support Preprocessor Macro for Application Code
# ==============================================================================
# This allows C++ code to check #if WITH_CUDA_SUPPORT at compile time
# Only define if both CUDA and cuDNN are available (required for full DNN CUDA support)
if(ENABLE_CUDA AND WITH_CUDA AND CUDNN_FOUND)
    message(STATUS "✓ Enabling WITH_CUDA_SUPPORT preprocessor definition")
    message(STATUS "  - CUDA Version: ${CUDAToolkit_VERSION}")
    message(STATUS "  - cuDNN Found: ${CUDNN_INCLUDE_DIR}")
    # Will be added to targets later via target_compile_definitions
    set(CUDA_SUPPORT_ENABLED TRUE)
else()
    message(STATUS "CUDA support disabled - WITH_CUDA_SUPPORT not defined")
    if(NOT WITH_CUDA)
        message(STATUS "  Reason: WITH_CUDA is OFF")
    elseif(NOT CUDNN_FOUND)
        message(STATUS "  Reason: cuDNN not found")
    endif()
    set(CUDA_SUPPORT_ENABLED FALSE)
endif()

# ==============================================================================
# ASIO SDK Configuration (Windows-only low-latency audio)
# ==============================================================================
set(ASIO_SDK_DIR "${CMAKE_SOURCE_DIR}/../vendor/ASIOSDK" CACHE PATH "Path to ASIO SDK")

if(WIN32)
    if(EXISTS "${ASIO_SDK_DIR}/common/asio.h")
        set(ASIO_SDK_FOUND TRUE)
        message(STATUS "✓ ASIO SDK found at ${ASIO_SDK_DIR}")
        message(STATUS "  - ASIO low-latency audio support will be enabled")
        
        # Store ASIO SDK paths as global variables for later use
        set(ASIO_SDK_INCLUDE_DIRS
            "${ASIO_SDK_DIR}/common"
            "${ASIO_SDK_DIR}/host"
            CACHE INTERNAL "ASIO SDK include directories"
        )
    else()
        set(ASIO_SDK_FOUND FALSE)
        message(WARNING 
            "ASIO SDK not found at ${ASIO_SDK_DIR}\n"
            "Low-latency ASIO audio support will NOT be available.\n"
            "Download from https://www.steinberg.net/asiosdk\n"
            "Extract to: ${ASIO_SDK_DIR}"
        )
    endif()
else()
    # ASIO is Windows-only (macOS uses CoreAudio, Linux uses ALSA/JACK)
    set(ASIO_SDK_FOUND FALSE)
    message(STATUS "ASIO not applicable on this platform (Windows-only)")
endif()

# ==============================================================================
# yt-dlp (YouTube Downloader - external executable)
# ==============================================================================
set(YTDLP_DIR "${CMAKE_SOURCE_DIR}/../vendor/yt-dlp" CACHE PATH "Path to yt-dlp")
set(YTDLP_FOUND FALSE)

if(EXISTS "${YTDLP_DIR}/yt-dlp.exe")
    message(STATUS "✓ yt-dlp found at ${YTDLP_DIR}")
    set(YTDLP_FOUND TRUE)
    set(YTDLP_EXECUTABLE "${YTDLP_DIR}/yt-dlp.exe")
else()
    message(WARNING "yt-dlp not found at ${YTDLP_DIR}/yt-dlp.exe")
    message(STATUS "  YouTube Source module will be compiled but may fail at runtime if yt-dlp.exe is missing.")
endif()

# ==============================================================================
# FFmpeg (Pre-built vendor binaries - SIMPLIFIED!)
# ==============================================================================
# Windows: Download from https://github.com/BtbN/FFmpeg-Builds/releases
# Extract to vendor/ffmpeg/ (see FFMPEG_SETUP_GUIDE.md)
set(FFMPEG_DIR "${CMAKE_SOURCE_DIR}/../vendor/ffmpeg" CACHE PATH "Path to FFmpeg")
set(FFMPEG_FOUND FALSE)

if(WIN32)
    # Windows: Use pre-built vendor binaries
    if(EXISTS "${FFMPEG_DIR}/include/libavformat/avformat.h")
        message(STATUS "✓ FFmpeg found at ${FFMPEG_DIR}")
        
        set(FFMPEG_INCLUDE_DIR "${FFMPEG_DIR}/include")
        set(FFMPEG_LIB_DIR "${FFMPEG_DIR}/lib")
        set(FFMPEG_BIN_DIR "${FFMPEG_DIR}/bin")
        
        # Find all required libraries
        find_library(AVFORMAT_LIBRARY avformat PATHS "${FFMPEG_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)
        find_library(AVCODEC_LIBRARY avcodec PATHS "${FFMPEG_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)
        find_library(AVUTIL_LIBRARY avutil PATHS "${FFMPEG_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)
        find_library(SWRESAMPLE_LIBRARY swresample PATHS "${FFMPEG_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)
        find_library(SWSCALE_LIBRARY swscale PATHS "${FFMPEG_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)
        
        set(FFMPEG_LIBRARIES 
            ${AVFORMAT_LIBRARY}
            ${AVCODEC_LIBRARY}
            ${AVUTIL_LIBRARY}
            ${SWRESAMPLE_LIBRARY}
            ${SWSCALE_LIBRARY}
        )
        
        message(STATUS "  - Include: ${FFMPEG_INCLUDE_DIR}")
        message(STATUS "  - Libraries: ${FFMPEG_LIBRARIES}")
        set(FFMPEG_FOUND TRUE)
    else()
        message(FATAL_ERROR 
            "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
            "❌ FFmpeg NOT FOUND!\n"
            "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
            "Expected location: ${FFMPEG_DIR}\n"
            "\n"
            "📦 Quick Setup (5 minutes):\n"
            "  1. Download: https://github.com/BtbN/FFmpeg-Builds/releases\n"
            "     Get: ffmpeg-master-latest-win64-gpl-shared.zip\n"
            "  2. Extract to: ${FFMPEG_DIR}\n"
            "  3. Folder structure should be:\n"
            "     vendor/ffmpeg/\n"
            "       ├── bin/     (DLL files)\n"
            "       ├── include/ (header files)\n"
            "       └── lib/     (.lib files)\n"
            "\n"
            "📖 See FFMPEG_SETUP_GUIDE.md for detailed instructions\n"
            "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
        )
    endif()
else()
    # Linux/Mac: Use system packages
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(FFMPEG
            libavformat>=57.0
            libavcodec>=57.0
            libavutil>=55.0
            libswresample>=3.0
            libswscale>=5.0
        )
        if(FFMPEG_FOUND)
            set(FFMPEG_INCLUDE_DIR ${FFMPEG_INCLUDE_DIRS})
            set(FFMPEG_LIBRARIES ${FFMPEG_LIBRARIES})
            message(STATUS "✓ FFmpeg found via pkg-config")
            message(STATUS "  - Include: ${FFMPEG_INCLUDE_DIR}")
        endif()
    endif()
    
    # Fallback: try finding libraries directly
    if(NOT FFMPEG_FOUND)
        find_path(FFMPEG_INCLUDE_DIR NAMES libavformat/avformat.h
            PATHS /usr/include /usr/local/include /opt/homebrew/include
        )
        find_library(AVFORMAT_LIBRARY avformat)
        find_library(AVCODEC_LIBRARY avcodec)
        find_library(AVUTIL_LIBRARY avutil)
        find_library(SWRESAMPLE_LIBRARY swresample)
        find_library(SWSCALE_LIBRARY swscale)
        
        if(FFMPEG_INCLUDE_DIR AND AVFORMAT_LIBRARY)
            set(FFMPEG_LIBRARIES 
                ${AVFORMAT_LIBRARY}
                ${AVCODEC_LIBRARY}
                ${AVUTIL_LIBRARY}
                ${SWRESAMPLE_LIBRARY}
                ${SWSCALE_LIBRARY}
            )
            set(FFMPEG_FOUND TRUE)
            message(STATUS "✓ FFmpeg found")
        else()
            message(FATAL_ERROR
                "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
                "❌ FFmpeg NOT FOUND!\n"
                "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
                "📦 Install FFmpeg:\n"
                "  Linux:  sudo apt-get install libavformat-dev libavcodec-dev libavutil-dev libswresample-dev libswscale-dev\n"
                "  macOS:  brew install ffmpeg\n"
                "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
            )
        endif()
    endif()
endif()

# --------------------------------------------------------------
# Rubber Band (optional, real-time timestretcher/pitch-shifter)
# --------------------------------------------------------------

# ==============================================================================
# NDI SDK Configuration (Network Device Interface for video streaming)
# ==============================================================================
# Download from: https://ndi.video/for-developers/ndi-sdk/
# Extract to: vendor/ndi/ (should contain Include/ and Lib/ subdirectories)
set(NDI_SDK_DIR "${CMAKE_SOURCE_DIR}/../vendor/ndi" CACHE PATH "Path to NDI SDK")
set(NDI_FOUND FALSE)

if(ENABLE_OPENCV)
    # Check for NDI SDK header
    if(EXISTS "${NDI_SDK_DIR}/Include/Processing.NDI.Lib.h")
        set(NDI_FOUND TRUE)
        message(STATUS "✓ NDI SDK found at ${NDI_SDK_DIR}")
        
        set(NDI_INCLUDE_DIR "${NDI_SDK_DIR}/Include")
        
        # Find NDI library (Windows)
        if(WIN32)
            # NDI uses runtime DLL loading, we only need the import library
            find_library(NDI_LIBRARY
                NAMES Processing.NDI.Lib.x64
                PATHS "${NDI_SDK_DIR}/Lib/x64"
                NO_DEFAULT_PATH
            )
            
            if(NDI_LIBRARY)
                message(STATUS "  - Include: ${NDI_INCLUDE_DIR}")
                message(STATUS "  - Library: ${NDI_LIBRARY}")
            else()
                message(WARNING "NDI import library not found. Trying header-only mode with runtime loading.")
                # NDI supports pure runtime loading - we can still use it
            endif()
        else()
            # Linux/macOS - NDI uses runtime loading
            message(STATUS "  - Include: ${NDI_INCLUDE_DIR}")
            message(STATUS "  - Using runtime DLL loading")
        endif()
    else()
        message(STATUS "NDI SDK not found at ${NDI_SDK_DIR}. NDI modules will be disabled.")
        message(STATUS "  Download from: https://ndi.video/for-developers/ndi-sdk/")
    endif()
else()
    message(STATUS "NDI disabled (requires OpenCV)")
endif()

# ==============================================================================
# librtlsdr (SDR Receiver)
# ==============================================================================
set(LIBRTLSDR_FOUND FALSE)
find_package(librtlsdr CONFIG QUIET)
if(librtlsdr_FOUND)
    message(STATUS "✓ librtlsdr found via CMake config")
    set(LIBRTLSDR_FOUND TRUE)
    set(LIBRTLSDR_LIBRARIES librtlsdr::librtlsdr)
else()
    # Fallback to pkg-config
     if(NOT WIN32)
        find_package(PkgConfig QUIET)
        if(PKG_CONFIG_FOUND)
            pkg_check_modules(LIBRTLSDR_PKG librtlsdr)
            if(LIBRTLSDR_PKG_FOUND)
                set(LIBRTLSDR_FOUND TRUE)
                set(LIBRTLSDR_INCLUDE_DIRS ${LIBRTLSDR_PKG_INCLUDE_DIRS})
                set(LIBRTLSDR_LIBRARIES ${LIBRTLSDR_PKG_LIBRARIES})
                message(STATUS "✓ librtlsdr found via pkg-config")
            endif()
        endif()
    endif()
endif()

if(NOT LIBRTLSDR_FOUND)
    message(STATUS "librtlsdr not found. SDR Receiver module will be disabled.")
    message(STATUS "  Install via: vcpkg install librtlsdr:x64-windows")
endif()

# ==============================================================================
# librtlsdr (SDR Receiver)
# ==============================================================================
set(LIBRTLSDR_FOUND FALSE)
find_package(rtlsdr CONFIG QUIET)
if(rtlsdr_FOUND)
    message(STATUS "✓ rtlsdr found via CMake config")
    set(LIBRTLSDR_FOUND TRUE)
    set(LIBRTLSDR_LIBRARIES rtlsdr::rtlsdr)
else()
    # Fallback to pkg-config
     if(NOT WIN32)
        find_package(PkgConfig QUIET)
        if(PKG_CONFIG_FOUND)
            pkg_check_modules(LIBRTLSDR_PKG librtlsdr)
            if(LIBRTLSDR_PKG_FOUND)
                set(LIBRTLSDR_FOUND TRUE)
                set(LIBRTLSDR_INCLUDE_DIRS ${LIBRTLSDR_PKG_INCLUDE_DIRS})
                set(LIBRTLSDR_LIBRARIES ${LIBRTLSDR_PKG_LIBRARIES})
                message(STATUS "✓ librtlsdr found via pkg-config")
            endif()
        endif()
    endif()
endif()

if(NOT LIBRTLSDR_FOUND)
    message(STATUS "librtlsdr not found. SDR Receiver module will be disabled.")
    message(STATUS "  Install via: vcpkg install librtlsdr:x64-windows")
endif()

# ==============================================================================
# SRT (Secure Reliable Transport) - Low-latency streaming protocol
# ==============================================================================
# Install via vcpkg: vcpkg install libsrt:x64-windows
set(SRT_FOUND FALSE)

# Try to find SRT via CMake config (vcpkg provides this)
find_package(srt QUIET CONFIG)
if(srt_FOUND)
    message(STATUS "✓ SRT found via CMake config")
    set(SRT_FOUND TRUE)
    set(SRT_LIBRARIES srt::srt)
else()
    # Fallback: Try pkg-config on Linux/Mac
    if(NOT WIN32)
        find_package(PkgConfig QUIET)
        if(PKG_CONFIG_FOUND)
            pkg_check_modules(SRT_PKG srt)
            if(SRT_PKG_FOUND)
                set(SRT_FOUND TRUE)
                set(SRT_INCLUDE_DIRS ${SRT_PKG_INCLUDE_DIRS})
                set(SRT_LIBRARIES ${SRT_PKG_LIBRARIES})
                message(STATUS "✓ SRT found via pkg-config")
            endif()
        endif()
    endif()
    
    # Fallback: Manual search (vcpkg default paths)
    if(NOT SRT_FOUND)
        find_path(SRT_INCLUDE_DIR NAMES srt/srt.h
            PATHS 
                "${CMAKE_SOURCE_DIR}/../vendor/srt/include"
                "$ENV{VCPKG_ROOT}/installed/x64-windows/include"
                "C:/vcpkg/installed/x64-windows/include"
                "/usr/include"
                "/usr/local/include"
        )
        find_library(SRT_LIBRARY NAMES srt
            PATHS
                "${CMAKE_SOURCE_DIR}/../vendor/srt/lib"
                "$ENV{VCPKG_ROOT}/installed/x64-windows/lib"
                "C:/vcpkg/installed/x64-windows/lib"
                "/usr/lib"
                "/usr/local/lib"
        )
        
        if(SRT_INCLUDE_DIR AND SRT_LIBRARY)
            set(SRT_FOUND TRUE)
            set(SRT_INCLUDE_DIRS ${SRT_INCLUDE_DIR})
            set(SRT_LIBRARIES ${SRT_LIBRARY})
            message(STATUS "✓ SRT found manually")
            message(STATUS "  - Include: ${SRT_INCLUDE_DIR}")
            message(STATUS "  - Library: ${SRT_LIBRARY}")
            
            # Find SRT DLL for runtime copy
            find_file(SRT_DLL srt.dll
                PATHS 
                    "$ENV{VCPKG_ROOT}/installed/x64-windows/bin"
                    "C:/vcpkg/installed/x64-windows/bin"
                NO_DEFAULT_PATH
            )
        endif()
    endif()
endif()

if(NOT SRT_FOUND)
    message(STATUS "SRT not found. SRT Receiver module will be disabled.")
    message(STATUS "  Install via: vcpkg install libsrt:x64-windows")
endif()

# ==============================================================================
# C++/WinRT for Bluetooth Audio and System Audio (Windows only)
# ==============================================================================
set(CPPWINRT_AVAILABLE FALSE)

if(WIN32)
    # Check for Windows SDK with C++/WinRT support (10.0.19041.0+)
    # AudioPlaybackConnection requires Windows 10 2004 (build 19041)
    # We check for the WinRT header availability
    include(CheckIncludeFileCXX)
    check_include_file_cxx("winrt/Windows.Media.Audio.h" HAS_WINRT_HEADERS)
    
    if(HAS_WINRT_HEADERS)
        set(CPPWINRT_AVAILABLE TRUE)
        message(STATUS "✓ C++/WinRT available (Windows SDK)")
    else()
        # Try to detect SDK version from CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION
        if(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION)
            if(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION VERSION_GREATER_EQUAL "10.0.19041.0")
                set(CPPWINRT_AVAILABLE TRUE)
                message(STATUS "✓ C++/WinRT available (Windows SDK ${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION})")
            else()
                message(STATUS "C++/WinRT requires Windows SDK 10.0.19041.0 or later")
                message(STATUS "  Current: ${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")
            endif()
        else()
            # Assume available on modern Windows builds
            set(CPPWINRT_AVAILABLE TRUE)
            message(STATUS "✓ C++/WinRT assumed available (Windows)")
        endif()
    endif()
endif()

# ==============================================================================

set(USE_RUBBERBAND ON CACHE BOOL "Enable Rubber Band time/pitch processing")
if (USE_RUBBERBAND)
  FetchContent_Declare(rubberband_fc
    GIT_REPOSITORY https://github.com/breakfastquay/rubberband.git
    GIT_TAG v3.3.0
  )
  FetchContent_MakeAvailable(rubberband_fc)
  # Build from single-file amalgamation to avoid external link issues
  # Use official amalgamated single-file build (works cross-platform)
  add_library(rubberband_single STATIC
    ${rubberband_fc_SOURCE_DIR}/single/RubberBandSingle.cpp
  )
  target_include_directories(rubberband_single PUBLIC
    ${rubberband_fc_SOURCE_DIR}
    ${rubberband_fc_SOURCE_DIR}/single
  )
  target_compile_definitions(rubberband_single PUBLIC
    NOMINMAX
    _USE_MATH_DEFINES
    RUBBERBAND_USE_R3=1
    RUBBERBAND_BUILD_FFT=KISSFFT
    RUBBERBAND_BUILD_RESAMPLER=NONE
  )
  set(RUBBERBAND_TARGET rubberband_single)
  set(RUBBERBAND_INCLUDE_DIR "${rubberband_fc_SOURCE_DIR}")
endif()

# ==============================================================================
# Main Engine App Target (Unchanged)
# ==============================================================================
juce_add_gui_app(ColliderApp
    PRODUCT_NAME "Collider Audio Engine"
    VERSION "0.1.0"
    COMPANY_NAME "Collider"
)

target_sources(ColliderApp PRIVATE
    Source/main.cpp
    Source/app/MainApplication.cpp
    Source/app/MainApplication.h
    Source/ui/MainComponent.cpp
    Source/ui/MainComponent.h
    Source/ui/TestHarnessComponent.cpp
    Source/ui/TestHarnessComponent.h
    Source/ui/VisualiserComponent.cpp
    Source/ui/VisualiserComponent.h
    Source/ui/DebugInfo.h
    Source/audio/AudioEngine.h
    Source/audio/AudioEngine.cpp
    Source/audio/MidiDeviceManager.h
    Source/audio/MidiDeviceManager.cpp
    Source/audio/OscDeviceManager.h
    Source/audio/OscDeviceManager.cpp
    Source/audio/voices/SampleVoiceProcessor.h
    Source/audio/voices/SampleVoiceProcessor.cpp
    Source/audio/voices/SynthVoiceProcessor.h
    Source/audio/voices/SynthVoiceProcessor.cpp
    Source/audio/voices/NoiseVoiceProcessor.h
    Source/audio/voices/NoiseVoiceProcessor.cpp
    Source/audio/voices/ModularVoice.h
    Source/audio/graph/VoiceProcessor.h
    Source/audio/graph/VoiceProcessor.cpp
    Source/audio/graph/ModularSynthProcessor.h
    Source/audio/graph/ModularSynthProcessor.cpp
    Source/audio/fx/FXChain.h
    Source/audio/fx/GainProcessor.h
    Source/audio/fx/GainProcessor.cpp
    Source/audio/assets/SampleBank.h
    Source/audio/assets/SampleBank.cpp
    Source/audio/modules/ModuleProcessor.h
    Source/audio/modules/ModuleProcessor.cpp
    Source/audio/modules/AudioInputModuleProcessor.h
    Source/audio/modules/AudioInputModuleProcessor.cpp
    Source/audio/modules/VCOModuleProcessor.h
    Source/audio/modules/VCOModuleProcessor.cpp
    Source/audio/modules/stk/StkWrapper.h
    Source/audio/modules/stk/StkWrapper.cpp
    Source/audio/modules/stk/StkStringModuleProcessor.h
    Source/audio/modules/stk/StkStringModuleProcessor.cpp
    Source/audio/modules/stk/StkWindModuleProcessor.h
    Source/audio/modules/stk/StkWindModuleProcessor.cpp
    Source/audio/modules/stk/StkPercussionModuleProcessor.h
    Source/audio/modules/stk/StkPercussionModuleProcessor.cpp
    Source/audio/modules/stk/StkPluckedModuleProcessor.h
    Source/audio/modules/stk/StkPluckedModuleProcessor.cpp
    Source/audio/modules/essentia/EssentiaWrapper.h
    Source/audio/modules/essentia/EssentiaWrapper.cpp
    Source/audio/modules/essentia/EssentiaOnsetDetectorModuleProcessor.h
    Source/audio/modules/essentia/EssentiaOnsetDetectorModuleProcessor.cpp
    Source/audio/modules/essentia/EssentiaPitchTrackerModuleProcessor.h
    Source/audio/modules/essentia/EssentiaPitchTrackerModuleProcessor.cpp
    Source/audio/modules/VCFModuleProcessor.h
    Source/audio/modules/VCFModuleProcessor.cpp
    Source/audio/modules/VCAModuleProcessor.h
    Source/audio/modules/VCAModuleProcessor.cpp
    Source/audio/modules/NoiseModuleProcessor.h
    Source/audio/modules/NoiseModuleProcessor.cpp
    Source/audio/modules/LFOModuleProcessor.h
    Source/audio/modules/LFOModuleProcessor.cpp
    Source/audio/modules/ADSRModuleProcessor.h
    Source/audio/modules/ADSRModuleProcessor.cpp
    Source/audio/modules/MixerModuleProcessor.h
    Source/audio/modules/MixerModuleProcessor.cpp
    Source/audio/modules/CVMixerModuleProcessor.h
    Source/audio/modules/CVMixerModuleProcessor.cpp
    Source/audio/modules/DelayModuleProcessor.h
    Source/audio/modules/DelayModuleProcessor.cpp
    Source/audio/modules/ReverbModuleProcessor.h
    Source/audio/modules/ReverbModuleProcessor.cpp
    Source/audio/modules/SAndHModuleProcessor.h
    Source/audio/modules/SAndHModuleProcessor.cpp
    Source/audio/modules/AttenuverterModuleProcessor.h
    Source/audio/modules/AttenuverterModuleProcessor.cpp
    Source/audio/modules/ScopeModuleProcessor.h
    Source/audio/modules/ScopeModuleProcessor.cpp
    Source/audio/modules/StepSequencerModuleProcessor.h
    Source/audio/modules/StepSequencerModuleProcessor.cpp
    Source/audio/modules/MultiSequencerModuleProcessor.h
    Source/audio/modules/MultiSequencerModuleProcessor.cpp
    Source/audio/modules/ChordArpModuleProcessor.h
    Source/audio/modules/ChordArpModuleProcessor.cpp
    Source/audio/modules/LagProcessorModuleProcessor.h
    Source/audio/modules/LagProcessorModuleProcessor.cpp
    Source/audio/modules/DeCrackleModuleProcessor.h
    Source/audio/modules/DeCrackleModuleProcessor.cpp
    Source/audio/modules/GraphicEQModuleProcessor.h
    Source/audio/modules/GraphicEQModuleProcessor.cpp
    Source/audio/modules/FrequencyGraphModuleProcessor.h
    Source/audio/modules/FrequencyGraphModuleProcessor.cpp
    Source/audio/modules/ChorusModuleProcessor.h
    Source/audio/modules/ChorusModuleProcessor.cpp
    Source/audio/modules/PhaserModuleProcessor.h
    Source/audio/modules/PhaserModuleProcessor.cpp
    Source/audio/modules/CompressorModuleProcessor.h
    Source/audio/modules/CompressorModuleProcessor.cpp
    Source/audio/modules/RecordModuleProcessor.h
    Source/audio/modules/RecordModuleProcessor.cpp
    Source/audio/modules/CommentModuleProcessor.h
    Source/audio/modules/CommentModuleProcessor.cpp
    Source/audio/modules/LimiterModuleProcessor.h
    Source/audio/modules/LimiterModuleProcessor.cpp
    Source/audio/modules/GateModuleProcessor.h
    Source/audio/modules/GateModuleProcessor.cpp
    Source/audio/modules/DriveModuleProcessor.h
    Source/audio/modules/DriveModuleProcessor.cpp
    Source/audio/modules/BitCrusherModuleProcessor.h
    Source/audio/modules/BitCrusherModuleProcessor.cpp
    Source/audio/modules/PanVolModuleProcessor.h
    Source/audio/modules/PanVolModuleProcessor.cpp
    Source/audio/modules/MathModuleProcessor.h
    Source/audio/modules/MathModuleProcessor.cpp
    Source/audio/modules/MapRangeModuleProcessor.h
    Source/audio/modules/MapRangeModuleProcessor.cpp
    Source/audio/modules/ComparatorModuleProcessor.h
    Source/audio/modules/ComparatorModuleProcessor.cpp
    Source/audio/modules/RandomModuleProcessor.h
    Source/audio/modules/RandomModuleProcessor.cpp
    Source/audio/modules/RateModuleProcessor.h
    Source/audio/modules/RateModuleProcessor.cpp
    Source/audio/modules/QuantizerModuleProcessor.h
    Source/audio/modules/QuantizerModuleProcessor.cpp
    Source/audio/modules/SequentialSwitchModuleProcessor.h
    Source/audio/modules/SequentialSwitchModuleProcessor.cpp
    Source/audio/modules/LogicModuleProcessor.h
    Source/audio/modules/LogicModuleProcessor.cpp
    Source/audio/modules/ClockDividerModuleProcessor.h
    Source/audio/modules/ClockDividerModuleProcessor.cpp
    Source/audio/modules/WaveshaperModuleProcessor.h
    Source/audio/modules/WaveshaperModuleProcessor.cpp
    Source/audio/modules/MultiBandShaperModuleProcessor.h
    Source/audio/modules/MultiBandShaperModuleProcessor.cpp
    Source/audio/modules/GranulatorModuleProcessor.h
    Source/audio/modules/GranulatorModuleProcessor.cpp
    Source/audio/modules/SpatialGranulatorModuleProcessor.h
    Source/audio/modules/SpatialGranulatorModuleProcessor.cpp
    Source/audio/modules/HarmonicShaperModuleProcessor.h
    Source/audio/modules/HarmonicShaperModuleProcessor.cpp
    Source/audio/modules/ValueModuleProcessor.h
    Source/audio/modules/ValueModuleProcessor.cpp
    Source/audio/modules/TimePitchModuleProcessor.h
    Source/audio/modules/TimePitchModuleProcessor.cpp
    Source/audio/modules/MIDIPlayerModuleProcessor.h
    Source/audio/modules/MIDIPlayerModuleProcessor.cpp
        Source/audio/modules/TrackMixerModuleProcessor.h
        Source/audio/modules/TrackMixerModuleProcessor.cpp
    Source/audio/modules/PolyVCOModuleProcessor.h
    Source/audio/modules/PolyVCOModuleProcessor.cpp
        Source/audio/modules/DebugModuleProcessor.h
        Source/audio/modules/DebugModuleProcessor.cpp
        Source/audio/modules/CommentModuleProcessor.h
        Source/audio/modules/CommentModuleProcessor.cpp
        Source/audio/modules/RerouteModuleProcessor.h
        Source/audio/modules/RerouteModuleProcessor.cpp
        Source/audio/modules/InputDebugModuleProcessor.h
        Source/audio/modules/InputDebugModuleProcessor.cpp
    Source/audio/modules/TTSPerformerModuleProcessor.h
    Source/audio/modules/TTSPerformerModuleProcessor.cpp
    Source/audio/modules/TimingData.h
    Source/audio/modules/SampleLoaderModuleProcessor.h
    Source/audio/modules/SampleLoaderModuleProcessor.cpp
    Source/audio/modules/SampleSfxModuleProcessor.h
    Source/audio/modules/SampleSfxModuleProcessor.cpp
    Source/audio/modules/FunctionGeneratorModuleProcessor.h
    Source/audio/modules/FunctionGeneratorModuleProcessor.cpp
    Source/audio/modules/BestPracticeNodeProcessor.h
    Source/audio/modules/BestPracticeNodeProcessor.cpp
    Source/audio/modules/TimelineModuleProcessor.h
    Source/audio/modules/TimelineModuleProcessor.cpp
    Source/audio/modules/VocalTractFilterModuleProcessor.h
    Source/audio/modules/VocalTractFilterModuleProcessor.cpp
    Source/audio/modules/VstHostModuleProcessor.h
    Source/audio/modules/VstHostModuleProcessor.cpp
    Source/audio/modules/ShapingOscillatorModuleProcessor.h
    Source/audio/modules/ShapingOscillatorModuleProcessor.cpp
    Source/audio/modules/InletModuleProcessor.h
    Source/audio/modules/InletModuleProcessor.cpp
    Source/audio/modules/OutletModuleProcessor.h
    Source/audio/modules/OutletModuleProcessor.cpp
    Source/audio/modules/MetaModuleProcessor.h
    Source/audio/modules/MetaModuleProcessor.cpp
    Source/audio/modules/SnapshotSequencerModuleProcessor.h
    Source/audio/modules/SnapshotSequencerModuleProcessor.cpp
    Source/audio/modules/MIDICVModuleProcessor.h
    Source/audio/modules/MIDICVModuleProcessor.cpp
        Source/audio/modules/OSCCVModuleProcessor.h
        Source/audio/modules/OSCCVModuleProcessor.cpp
        Source/audio/modules/CVOSCSenderModuleProcessor.h
        Source/audio/modules/CVOSCSenderModuleProcessor.cpp
    Source/audio/modules/MIDIFadersModuleProcessor.h
    Source/audio/modules/MIDIFadersModuleProcessor.cpp
    Source/audio/modules/MIDIKnobsModuleProcessor.h
    Source/audio/modules/MIDIKnobsModuleProcessor.cpp
    Source/audio/modules/MIDIButtonsModuleProcessor.h
    Source/audio/modules/MIDIButtonsModuleProcessor.cpp
    Source/audio/modules/MIDIJogWheelModuleProcessor.h
    Source/audio/modules/MIDIJogWheelModuleProcessor.cpp
    Source/audio/modules/MIDIPadModuleProcessor.h
    Source/audio/modules/MIDIPadModuleProcessor.cpp
    Source/audio/modules/MidiLoggerModuleProcessor.h
    Source/audio/modules/MidiLoggerModuleProcessor.cpp
    Source/audio/modules/TempoClockModuleProcessor.h
    Source/audio/modules/TempoClockModuleProcessor.cpp
    Source/audio/modules/PhysicsModuleProcessor.h
    Source/audio/modules/PhysicsModuleProcessor.cpp
    Source/audio/modules/StrokeSequencerModuleProcessor.h
    Source/audio/modules/StrokeSequencerModuleProcessor.cpp
    Source/audio/modules/TapTempo.h
    Source/audio/modules/TapTempo.cpp
    Source/audio/modules/BPMMonitorModuleProcessor.h
    Source/audio/modules/BPMMonitorModuleProcessor.cpp
    Source/audio/modules/AnimationModuleProcessor.h
    Source/audio/modules/AnimationModuleProcessor.cpp
    Source/audio/modules/AutomationLaneModuleProcessor.h
    Source/audio/modules/AutomationLaneModuleProcessor.cpp
    Source/audio/modules/AutomatoModuleProcessor.h
    Source/audio/modules/AutomatoModuleProcessor.cpp
    Source/video/VideoFrameManager.h
    Source/video/CameraEnumerator.h
    Source/audio/modules/WebcamLoaderModule.h
    Source/audio/modules/WebcamLoaderModule.cpp
    Source/audio/modules/VideoFileLoaderModule.h
    Source/audio/modules/VideoFileLoaderModule.cpp
    Source/audio/modules/YouTubeSourceModule.h
    Source/audio/modules/YouTubeSourceModule.cpp
    Source/audio/modules/FFmpegAudioReader.h
    Source/audio/modules/FFmpegAudioReader.cpp
    Source/audio/modules/TimeStretcherAudioSource.h
    Source/audio/modules/TimeStretcherAudioSource.cpp
    Source/audio/modules/VideoFXModule.h
    Source/audio/modules/VideoFXModule.cpp
    Source/audio/modules/ChromakeyModuleProcessor.h
    Source/audio/modules/ChromakeyModuleProcessor.cpp
    Source/audio/modules/VideoCompositorModule.h
    Source/audio/modules/VideoCompositorModule.cpp
    Source/audio/modules/VideoDrawImpactModuleProcessor.h
    Source/audio/modules/VideoDrawImpactModuleProcessor.cpp
    Source/audio/modules/VideoViewerModuleProcessor.h
    Source/audio/modules/VideoViewerModuleProcessor.cpp
    Source/audio/modules/MovementDetectorModule.h
    Source/audio/modules/MovementDetectorModule.cpp
    Source/audio/modules/HumanDetectorModule.h
    Source/audio/modules/HumanDetectorModule.cpp
    Source/audio/modules/PoseEstimatorModule.h
    Source/audio/modules/PoseEstimatorModule.cpp
    Source/audio/modules/HandTrackerModule.h
    Source/audio/modules/HandTrackerModule.cpp
    Source/audio/modules/FaceTrackerModule.h
    Source/audio/modules/FaceTrackerModule.cpp
    Source/audio/modules/ObjectDetectorModule.h
    Source/audio/modules/ObjectDetectorModule.cpp
    Source/audio/modules/ColorTrackerModule.h
    Source/audio/modules/ColorTrackerModule.cpp
    Source/audio/modules/ContourDetectorModule.h
    Source/audio/modules/ContourDetectorModule.cpp
    Source/audio/modules/CropVideoModule.h
    Source/audio/modules/CropVideoModule.cpp
    Source/audio/modules/NdiReceiverModule.h
    Source/audio/modules/NdiReceiverModule.cpp
    # Wireless/Streaming Audio Receivers
    Source/audio/modules/SdrReceiverModule.h
    Source/audio/modules/SdrReceiverModule.cpp
    Source/audio/modules/InternetRadioReceiverModule.h
    Source/audio/modules/InternetRadioReceiverModule.cpp
    Source/audio/modules/SystemAudioCaptureModule.h
    Source/audio/modules/SystemAudioCaptureModule.cpp
    Source/audio/modules/SrtReceiverModule.h
    Source/audio/modules/SrtReceiverModule.cpp
    Source/audio/modules/RtmpReceiverModule.h
    Source/audio/modules/RtmpReceiverModule.cpp
    Source/audio/modules/BluetoothAudioReceiverModule.h
    Source/audio/modules/BluetoothAudioReceiverModule.cpp
    Source/animation/AnimationData.h
    Source/animation/RawAnimationData.h
    Source/animation/RawAnimationData.cpp
    Source/animation/AnimationBinder.h
    Source/animation/AnimationBinder.cpp
    Source/animation/Animator.h
    Source/animation/Animator.cpp
    Source/animation/AnimationFileLoader.h
    Source/animation/AnimationFileLoader.cpp
    Source/animation/GltfLoader.h
    Source/animation/GltfLoader.cpp
    Source/animation/FbxLoader.h
    Source/animation/FbxLoader.cpp
    Source/animation/AnimationRenderer.h
    Source/animation/AnimationRenderer.cpp
    Source/ipc/IpcServer.cpp
    Source/ipc/IpcServer.h
    Source/ipc/OscClient.h
    Source/ipc/CommandBus.cpp
    Source/ipc/CommandBus.h
    Source/audio/utils/VoiceDeletionUtils.h
    Source/audio/dsp/TimePitchProcessor.h
    Source/audio/voices/VoiceDownloadHelper.h
    Source/audio/voices/VoiceDownloadHelper.cpp
    Source/audio/voices/VoiceDownloadThread.h
    Source/audio/voices/VoiceDownloadThread.cpp
    Source/utils/RtLogger.h
    Source/utils/RtLogger.cpp
)

target_compile_definitions(ColliderApp PRIVATE
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:ColliderApp,PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:ColliderApp,VERSION>"
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
    JUCE_PLUGINHOST_VST3=1
    $<$<BOOL:${USE_RUBBERBAND}>:USE_RUBBERBAND=1>
    $<$<NOT:$<BOOL:${USE_RUBBERBAND}>>:USE_RUBBERBAND=0>
    $<$<BOOL:${CUDA_SUPPORT_ENABLED}>:WITH_CUDA_SUPPORT=1>
    # Enable ASIO if SDK is found
    $<$<BOOL:${ASIO_SDK_FOUND}>:JUCE_ASIO=1>
    # Enable STK if found
    $<$<BOOL:${STK_FOUND}>:STK_FOUND=1>
    # Enable Essentia if found
    $<$<BOOL:${ESSENTIA_FOUND}>:ESSENTIA_FOUND=1>
)

target_link_libraries(ColliderApp PRIVATE
    juce::juce_gui_extra
    juce::juce_opengl
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_audio_formats
    juce::juce_dsp
    juce::juce_osc
    soundtouch
    $<$<BOOL:${STK_FOUND}>:stk>
    $<$<BOOL:${ESSENTIA_FOUND}>:${ESSENTIA_LIBRARY}>
    box2d
    glm::glm
    tinygltf
    ufbx_static
    ${OPENCV_LINK_LIBS}
    ${OPENCV_3RDPARTY_LIBS}  # OpenCV's 3rdparty deps (zlib, png, jpeg, etc.)
    ${OPENCV_CUDA_LIBS}       # cuBLAS, cuDNN for DNN CUDA backend
    $<$<BOOL:${USE_RUBBERBAND}>:${RUBBERBAND_TARGET}>
    $<$<BOOL:${FFMPEG_FOUND}>:${AVFORMAT_LIBRARY}>
    $<$<BOOL:${FFMPEG_FOUND}>:${AVCODEC_LIBRARY}>
    $<$<BOOL:${FFMPEG_FOUND}>:${AVUTIL_LIBRARY}>
    $<$<BOOL:${FFMPEG_FOUND}>:${SWRESAMPLE_LIBRARY}>
    $<$<BOOL:${FFMPEG_FOUND}>:${SWSCALE_LIBRARY}>
    # Piper TTS libraries (pre-built)
    ${ONNXRUNTIME_DIR}/lib/onnxruntime.lib
)

# Link CUDA runtime for OpenCV CUDA symbols (legacy fallback, OPENCV_CUDA_LIBS above preferred)
if(NOT OPENCV_CUDA_LIBS)
    if(TARGET CUDAToolkit::cudart)
        target_link_libraries(ColliderApp PRIVATE CUDAToolkit::cudart)
    elseif(CUDA_CUDART_LIBRARY)
        target_link_libraries(ColliderApp PRIVATE ${CUDA_CUDART_LIBRARY})
    endif()
endif()

# Link Windows libraries for native camera enumeration (DirectShow)
if(WIN32)
    target_link_libraries(ColliderApp PRIVATE ole32 strmiids)
endif()

# Link SRT if found (for SRT Receiver module)
if(SRT_FOUND)
    target_link_libraries(ColliderApp PRIVATE ${SRT_LIBRARIES})
    if(SRT_INCLUDE_DIRS)
        target_include_directories(ColliderApp PRIVATE ${SRT_INCLUDE_DIRS})
    endif()
    target_compile_definitions(ColliderApp PRIVATE HAS_SRT=1)
endif()

# Link librtlsdr if found
if(LIBRTLSDR_FOUND)
    target_link_libraries(ColliderApp PRIVATE ${LIBRTLSDR_LIBRARIES})
    if(LIBRTLSDR_INCLUDE_DIRS)
        target_include_directories(ColliderApp PRIVATE ${LIBRTLSDR_INCLUDE_DIRS})
    endif()
    target_compile_definitions(ColliderApp PRIVATE HAS_LIBRTLSDR=1)
endif()

# Link WinRT for Bluetooth Audio and System Audio Capture (Windows only)
if(WIN32 AND CPPWINRT_AVAILABLE)
    target_link_libraries(ColliderApp PRIVATE WindowsApp.lib)
    target_compile_definitions(ColliderApp PRIVATE HAS_WINRT_AUDIO=1)
endif()

# Add include directories for ColliderApp
target_include_directories(ColliderApp PRIVATE
    ${PIPER_DIR}
    ${ONNXRUNTIME_DIR}/include
    ${CMAKE_SOURCE_DIR}/../vendor/openvino_toolkit_windows_2025.3.0.19807.44526285f24_x86_64/samples/cpp/thirdparty/nlohmann_json/single_include
    ${CMAKE_SOURCE_DIR}/../soundtouch/include
    ${CMAKE_SOURCE_DIR}/../soundtouch/source/SoundTouch
    $<$<BOOL:${STK_FOUND}>:${STK_SOURCE_DIR}/include>
    $<$<BOOL:${ESSENTIA_FOUND}>:${ESSENTIA_DIR}/src>
    ${ufbx_SOURCE_DIR}
    $<$<BOOL:${FFMPEG_FOUND}>:${FFMPEG_INCLUDE_DIR}>
    # OpenCV include directories - source and generated headers
    ${opencv_SOURCE_DIR}/include
    ${opencv_SOURCE_DIR}/modules/core/include
    ${opencv_SOURCE_DIR}/modules/imgproc/include
    ${opencv_SOURCE_DIR}/modules/imgcodecs/include
    ${opencv_SOURCE_DIR}/modules/videoio/include
    ${opencv_SOURCE_DIR}/modules/highgui/include
    ${opencv_SOURCE_DIR}/modules/video/include
    ${opencv_SOURCE_DIR}/modules/objdetect/include
    ${opencv_SOURCE_DIR}/modules/features2d/include
    ${opencv_SOURCE_DIR}/modules/calib3d/include
    ${opencv_SOURCE_DIR}/modules/dnn/include              # For DNN headers (dnn.hpp)
    ${OPENCV_BUILD_DIR}
    ${OPENCV_BUILD_DIR}/modules/world
    # OpenCV CUDA modules (from opencv_contrib source)
    $<$<BOOL:${CUDA_SUPPORT_ENABLED}>:${opencv_contrib_SOURCE_DIR}/modules/cudaimgproc/include>
    $<$<BOOL:${CUDA_SUPPORT_ENABLED}>:${opencv_contrib_SOURCE_DIR}/modules/cudawarping/include>
    $<$<BOOL:${CUDA_SUPPORT_ENABLED}>:${opencv_contrib_SOURCE_DIR}/modules/cudaarithm/include>
    $<$<BOOL:${CUDA_SUPPORT_ENABLED}>:${opencv_contrib_SOURCE_DIR}/modules/cudafilters/include>
    $<$<BOOL:${CUDA_SUPPORT_ENABLED}>:${opencv_contrib_SOURCE_DIR}/modules/cudafeatures2d/include>
    $<$<BOOL:${CUDA_SUPPORT_ENABLED}>:${opencv_contrib_SOURCE_DIR}/modules/cudaobjdetect/include>
    $<$<BOOL:${CUDA_SUPPORT_ENABLED}>:${opencv_contrib_SOURCE_DIR}/modules/cudaoptflow/include>
    $<$<BOOL:${USE_RUBBERBAND}>:${RUBBERBAND_INCLUDE_DIR}>
    # ASIO SDK headers (Windows only, conditional)
    $<$<BOOL:${ASIO_SDK_FOUND}>:${ASIO_SDK_DIR}/common>
    $<$<BOOL:${ASIO_SDK_FOUND}>:${ASIO_SDK_DIR}/host>
)

# ==============================================================================
# Preset Creator App Target (Final Corrected Version)
# ==============================================================================

juce_add_gui_app(PresetCreatorApp
    PRODUCT_NAME "Pikon Raditsz"
    VERSION "0.1.0"
    COMPANY_NAME "Collider"
    ICON_BIG "${CMAKE_SOURCE_DIR}/../icons/pikon_raditsz_icon.png"
    ICON_SMALL "${CMAKE_SOURCE_DIR}/../icons/pikon_raditsz_icon.png"
)

target_sources(PresetCreatorApp PRIVATE
    # Your application sources
    Source/preset_creator/PresetCreatorMain.cpp
    Source/preset_creator/PresetCreatorApplication.h
    Source/preset_creator/PinDatabase.h
    Source/preset_creator/PinDatabase.cpp
    Source/preset_creator/NotificationManager.h
    Source/preset_creator/NotificationManager.cpp
    Source/preset_creator/PresetValidator.h
    Source/preset_creator/PresetValidator.cpp
    Source/preset_creator/PresetAutoHealer.h
    Source/preset_creator/PresetAutoHealer.cpp
    Source/preset_creator/SavePresetJob.h
    Source/preset_creator/SavePresetJob.cpp
    Source/preset_creator/ShortcutManager.h
    Source/preset_creator/ShortcutManager.cpp
    Source/preset_creator/ImGuiNodeEditorComponent.h
    Source/preset_creator/ImGuiNodeEditorComponent.cpp
    Source/preset_creator/PatchGenerator.h
    Source/preset_creator/PatchGenerator.cpp
    Source/preset_creator/PresetCreatorComponent.h
    Source/preset_creator/PresetCreatorComponent.cpp
    Source/preset_creator/ControllerPresetManager.h
    Source/preset_creator/ControllerPresetManager.cpp
    Source/preset_creator/VstManager.h
    Source/preset_creator/VstManager.cpp
    Source/preset_creator/SplashScreenComponent.h
    Source/preset_creator/SplashScreenComponent.cpp
    
    # Version and utilities
    Source/utils/VersionInfo.h
    
    # Auto-updater system
    Source/updater/UpdaterTypes.h
    Source/updater/UpdaterTypes.cpp
    Source/updater/HashVerifier.h
    Source/updater/HashVerifier.cpp
    Source/updater/VersionManager.h
    Source/updater/VersionManager.cpp
    Source/updater/UpdateChecker.h
    Source/updater/UpdateChecker.cpp
    Source/updater/FileDownloader.h
    Source/updater/FileDownloader.cpp
    Source/updater/UpdateApplier.h
    Source/updater/UpdateApplier.cpp
    Source/updater/UpdateManager.h
    Source/updater/UpdateManager.cpp
    Source/updater/ui/UpdateAvailableDialog.h
    Source/updater/ui/UpdateAvailableDialog.cpp
    Source/updater/ui/DownloadProgressDialog.h
    Source/updater/ui/DownloadProgressDialog.cpp
    Source/updater/ui/UpdateDownloadDialog.h
    Source/updater/ui/UpdateDownloadDialog.cpp
    
    # Theme system
    Source/preset_creator/theme/Theme.h
    Source/preset_creator/theme/ThemeManager.h
    Source/preset_creator/theme/ThemeManager.cpp
    Source/preset_creator/theme/ThemeEditorComponent.h
    Source/preset_creator/theme/ThemeEditorComponent.cpp
    Source/preset_creator/HelpManagerComponent.h
    Source/preset_creator/HelpManagerComponent.cpp
    Source/preset_creator/VoiceDownloadDialog.h
    Source/preset_creator/VoiceDownloadDialog.cpp

    # Add ImGui, imnodes, and the backend DIRECTLY as source files
    ${imgui_fc_SOURCE_DIR}/imgui.cpp
    ${imgui_fc_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_fc_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_fc_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_fc_SOURCE_DIR}/backends/imgui_impl_opengl2.cpp
    ${imnodes_fc_SOURCE_DIR}/imnodes.cpp

    # Your other reused engine modules
    Source/audio/MidiDeviceManager.h
    Source/audio/MidiDeviceManager.cpp
    Source/audio/OscDeviceManager.h
    Source/audio/OscDeviceManager.cpp
    Source/audio/graph/ModularSynthProcessor.h
    Source/audio/graph/ModularSynthProcessor.cpp
    Source/audio/modules/ModuleProcessor.h
    Source/audio/modules/ModuleProcessor.cpp
    Source/audio/modules/AudioInputModuleProcessor.h
    Source/audio/modules/AudioInputModuleProcessor.cpp
    Source/audio/modules/VCOModuleProcessor.h
    Source/audio/modules/VCOModuleProcessor.cpp
    Source/audio/modules/stk/StkWrapper.h
    Source/audio/modules/stk/StkWrapper.cpp
    Source/audio/modules/stk/StkStringModuleProcessor.h
    Source/audio/modules/stk/StkStringModuleProcessor.cpp
    Source/audio/modules/stk/StkWindModuleProcessor.h
    Source/audio/modules/stk/StkWindModuleProcessor.cpp
    Source/audio/modules/stk/StkPercussionModuleProcessor.h
    Source/audio/modules/stk/StkPercussionModuleProcessor.cpp
    Source/audio/modules/stk/StkPluckedModuleProcessor.h
    Source/audio/modules/stk/StkPluckedModuleProcessor.cpp
    Source/audio/modules/essentia/EssentiaWrapper.h
    Source/audio/modules/essentia/EssentiaWrapper.cpp
    Source/audio/modules/essentia/EssentiaOnsetDetectorModuleProcessor.h
    Source/audio/modules/essentia/EssentiaOnsetDetectorModuleProcessor.cpp
    Source/audio/modules/essentia/EssentiaPitchTrackerModuleProcessor.h
    Source/audio/modules/essentia/EssentiaPitchTrackerModuleProcessor.cpp
    Source/audio/modules/VCFModuleProcessor.h
    Source/audio/modules/VCFModuleProcessor.cpp
    Source/audio/modules/VCAModuleProcessor.h
    Source/audio/modules/VCAModuleProcessor.cpp
    Source/audio/modules/NoiseModuleProcessor.h
    Source/audio/modules/NoiseModuleProcessor.cpp
    Source/audio/modules/LFOModuleProcessor.h
    Source/audio/modules/LFOModuleProcessor.cpp
    Source/audio/modules/ADSRModuleProcessor.h
    Source/audio/modules/ADSRModuleProcessor.cpp
    Source/audio/modules/MixerModuleProcessor.h
    Source/audio/modules/MixerModuleProcessor.cpp
    Source/audio/modules/CVMixerModuleProcessor.h
    Source/audio/modules/CVMixerModuleProcessor.cpp
    Source/audio/modules/DelayModuleProcessor.h
    Source/audio/modules/DelayModuleProcessor.cpp
    Source/audio/modules/ReverbModuleProcessor.h
    Source/audio/modules/ReverbModuleProcessor.cpp
    Source/audio/modules/SAndHModuleProcessor.h
    Source/audio/modules/SAndHModuleProcessor.cpp
    Source/audio/modules/AttenuverterModuleProcessor.h
    Source/audio/modules/AttenuverterModuleProcessor.cpp
    Source/audio/modules/ScopeModuleProcessor.h
    Source/audio/modules/ScopeModuleProcessor.cpp
    Source/audio/modules/StepSequencerModuleProcessor.h
    Source/audio/modules/StepSequencerModuleProcessor.cpp
    Source/audio/modules/MathModuleProcessor.h
    Source/audio/modules/MathModuleProcessor.cpp
    Source/audio/modules/MapRangeModuleProcessor.h
    Source/audio/modules/MapRangeModuleProcessor.cpp
    Source/audio/modules/ComparatorModuleProcessor.h
    Source/audio/modules/ComparatorModuleProcessor.cpp
    Source/audio/modules/RandomModuleProcessor.h
    Source/audio/modules/RandomModuleProcessor.cpp
    Source/audio/modules/RateModuleProcessor.h
    Source/audio/modules/RateModuleProcessor.cpp
    Source/audio/modules/QuantizerModuleProcessor.h
    Source/audio/modules/QuantizerModuleProcessor.cpp
    Source/audio/modules/SequentialSwitchModuleProcessor.h
    Source/audio/modules/SequentialSwitchModuleProcessor.cpp
    Source/audio/modules/LogicModuleProcessor.h
    Source/audio/modules/LogicModuleProcessor.cpp
    Source/audio/modules/ClockDividerModuleProcessor.h
    Source/audio/modules/ClockDividerModuleProcessor.cpp
    Source/audio/modules/WaveshaperModuleProcessor.h
    Source/audio/modules/WaveshaperModuleProcessor.cpp
    Source/audio/modules/MultiBandShaperModuleProcessor.h
    Source/audio/modules/MultiBandShaperModuleProcessor.cpp
    Source/audio/modules/GranulatorModuleProcessor.h
    Source/audio/modules/GranulatorModuleProcessor.cpp
    Source/audio/modules/SpatialGranulatorModuleProcessor.h
    Source/audio/modules/SpatialGranulatorModuleProcessor.cpp
    Source/audio/modules/HarmonicShaperModuleProcessor.h
    Source/audio/modules/HarmonicShaperModuleProcessor.cpp
    Source/audio/modules/ValueModuleProcessor.h
    Source/audio/modules/ValueModuleProcessor.cpp
        Source/audio/modules/DebugModuleProcessor.h
        Source/audio/modules/DebugModuleProcessor.cpp
        Source/audio/modules/CommentModuleProcessor.h
        Source/audio/modules/CommentModuleProcessor.cpp
        Source/audio/modules/InputDebugModuleProcessor.h
        Source/audio/modules/InputDebugModuleProcessor.cpp
    Source/audio/modules/TimePitchModuleProcessor.h
    Source/audio/modules/TimePitchModuleProcessor.cpp
    Source/audio/modules/MIDIPlayerModuleProcessor.h
    Source/audio/modules/MIDIPlayerModuleProcessor.cpp
        Source/audio/modules/TrackMixerModuleProcessor.h
        Source/audio/modules/TrackMixerModuleProcessor.cpp
    Source/audio/modules/PolyVCOModuleProcessor.h
    Source/audio/modules/PolyVCOModuleProcessor.cpp
    Source/audio/modules/RerouteModuleProcessor.h
    Source/audio/modules/RerouteModuleProcessor.cpp
    Source/audio/modules/TTSPerformerModuleProcessor.h
    Source/audio/modules/TTSPerformerModuleProcessor.cpp
    Source/audio/modules/TimingData.h
    Source/audio/modules/SampleLoaderModuleProcessor.h
    Source/audio/modules/SampleLoaderModuleProcessor.cpp
    Source/audio/modules/SampleSfxModuleProcessor.h
    Source/audio/modules/SampleSfxModuleProcessor.cpp
    Source/audio/modules/FunctionGeneratorModuleProcessor.h
    Source/audio/modules/FunctionGeneratorModuleProcessor.cpp
    Source/audio/modules/BestPracticeNodeProcessor.h
    Source/audio/modules/BestPracticeNodeProcessor.cpp
    Source/audio/modules/TimelineModuleProcessor.h
    Source/audio/modules/TimelineModuleProcessor.cpp
    Source/audio/modules/ShapingOscillatorModuleProcessor.h
    Source/audio/modules/ShapingOscillatorModuleProcessor.cpp
    Source/audio/voices/SampleVoiceProcessor.h
    Source/audio/voices/SampleVoiceProcessor.cpp
    Source/audio/voices/VoiceDownloadHelper.h
    Source/audio/voices/VoiceDownloadHelper.cpp
    Source/audio/voices/VoiceDownloadThread.h
    Source/audio/voices/VoiceDownloadThread.cpp
    Source/audio/graph/VoiceProcessor.h
    Source/audio/graph/VoiceProcessor.cpp
    Source/audio/assets/SampleBank.h
    Source/audio/assets/SampleBank.cpp
    Source/utils/RtLogger.h
    Source/utils/RtLogger.cpp
    Source/audio/modules/VocalTractFilterModuleProcessor.h
    Source/audio/modules/VocalTractFilterModuleProcessor.cpp
    Source/audio/modules/VstHostModuleProcessor.h
    Source/audio/modules/VstHostModuleProcessor.cpp
    Source/audio/modules/MultiSequencerModuleProcessor.h
    Source/audio/modules/MultiSequencerModuleProcessor.cpp
    Source/audio/modules/ChordArpModuleProcessor.h
    Source/audio/modules/ChordArpModuleProcessor.cpp
    Source/audio/modules/LagProcessorModuleProcessor.h
    Source/audio/modules/LagProcessorModuleProcessor.cpp
    Source/audio/modules/DeCrackleModuleProcessor.h
    Source/audio/modules/DeCrackleModuleProcessor.cpp
    Source/audio/modules/GraphicEQModuleProcessor.h
    Source/audio/modules/GraphicEQModuleProcessor.cpp
    Source/audio/modules/FrequencyGraphModuleProcessor.h
    Source/audio/modules/FrequencyGraphModuleProcessor.cpp
    Source/audio/modules/ChorusModuleProcessor.h
    Source/audio/modules/ChorusModuleProcessor.cpp
    Source/audio/modules/PhaserModuleProcessor.h
    Source/audio/modules/PhaserModuleProcessor.cpp
    Source/audio/modules/CompressorModuleProcessor.h
    Source/audio/modules/CompressorModuleProcessor.cpp
    Source/audio/modules/RecordModuleProcessor.h
    Source/audio/modules/RecordModuleProcessor.cpp
    Source/audio/modules/CommentModuleProcessor.h
    Source/audio/modules/CommentModuleProcessor.cpp
    Source/audio/modules/LimiterModuleProcessor.h
    Source/audio/modules/LimiterModuleProcessor.cpp
    Source/audio/modules/GateModuleProcessor.h
    Source/audio/modules/GateModuleProcessor.cpp
    Source/audio/modules/DriveModuleProcessor.h
    Source/audio/modules/DriveModuleProcessor.cpp
    Source/audio/modules/BitCrusherModuleProcessor.h
    Source/audio/modules/BitCrusherModuleProcessor.cpp
    Source/audio/modules/PanVolModuleProcessor.h
    Source/audio/modules/PanVolModuleProcessor.cpp
    Source/audio/modules/InletModuleProcessor.h
    Source/audio/modules/InletModuleProcessor.cpp
    Source/audio/modules/OutletModuleProcessor.h
    Source/audio/modules/OutletModuleProcessor.cpp
    Source/audio/modules/MetaModuleProcessor.h
    Source/audio/modules/MetaModuleProcessor.cpp
    Source/audio/modules/SnapshotSequencerModuleProcessor.h
    Source/audio/modules/SnapshotSequencerModuleProcessor.cpp
    Source/audio/modules/MIDICVModuleProcessor.h
    Source/audio/modules/MIDICVModuleProcessor.cpp
        Source/audio/modules/OSCCVModuleProcessor.h
        Source/audio/modules/OSCCVModuleProcessor.cpp
        Source/audio/modules/CVOSCSenderModuleProcessor.h
        Source/audio/modules/CVOSCSenderModuleProcessor.cpp
    Source/audio/modules/MIDIFadersModuleProcessor.h
    Source/audio/modules/MIDIFadersModuleProcessor.cpp
    Source/audio/modules/MIDIKnobsModuleProcessor.h
    Source/audio/modules/MIDIKnobsModuleProcessor.cpp
    Source/audio/modules/MIDIButtonsModuleProcessor.h
    Source/audio/modules/MIDIButtonsModuleProcessor.cpp
    Source/audio/modules/MIDIJogWheelModuleProcessor.h
    Source/audio/modules/MIDIJogWheelModuleProcessor.cpp
    Source/audio/modules/MIDIPadModuleProcessor.h
    Source/audio/modules/MIDIPadModuleProcessor.cpp
    Source/audio/modules/MidiLoggerModuleProcessor.h
    Source/audio/modules/MidiLoggerModuleProcessor.cpp
    Source/audio/modules/TempoClockModuleProcessor.h
    Source/audio/modules/TempoClockModuleProcessor.cpp
    Source/audio/modules/PhysicsModuleProcessor.h
    Source/audio/modules/PhysicsModuleProcessor.cpp
    Source/audio/modules/StrokeSequencerModuleProcessor.h
    Source/audio/modules/StrokeSequencerModuleProcessor.cpp
    Source/audio/modules/TapTempo.h
    Source/audio/modules/TapTempo.cpp
    Source/audio/modules/BPMMonitorModuleProcessor.h
    Source/audio/modules/BPMMonitorModuleProcessor.cpp
    Source/audio/modules/AnimationModuleProcessor.h
    Source/audio/modules/AnimationModuleProcessor.cpp
    Source/audio/modules/AutomationLaneModuleProcessor.h
    Source/audio/modules/AutomationLaneModuleProcessor.cpp
    Source/audio/modules/AutomatoModuleProcessor.h
    Source/audio/modules/AutomatoModuleProcessor.cpp
    Source/video/VideoFrameManager.h
    Source/video/CameraEnumerator.h
    Source/audio/modules/WebcamLoaderModule.h
    Source/audio/modules/WebcamLoaderModule.cpp
    Source/audio/modules/VideoFileLoaderModule.h
    Source/audio/modules/VideoFileLoaderModule.cpp
    Source/audio/modules/YouTubeSourceModule.h
    Source/audio/modules/YouTubeSourceModule.cpp
    Source/audio/modules/FFmpegAudioReader.h
    Source/audio/modules/FFmpegAudioReader.cpp
    Source/audio/modules/TimeStretcherAudioSource.h
    Source/audio/modules/TimeStretcherAudioSource.cpp
    Source/audio/modules/VideoFXModule.h
    Source/audio/modules/VideoFXModule.cpp
    Source/audio/modules/ChromakeyModuleProcessor.h
    Source/audio/modules/ChromakeyModuleProcessor.cpp
    Source/audio/modules/VideoCompositorModule.h
    Source/audio/modules/VideoCompositorModule.cpp
    Source/audio/modules/VideoDrawImpactModuleProcessor.h
    Source/audio/modules/VideoDrawImpactModuleProcessor.cpp
    Source/audio/modules/VideoViewerModuleProcessor.h
    Source/audio/modules/VideoViewerModuleProcessor.cpp
    Source/audio/modules/MovementDetectorModule.h
    Source/audio/modules/MovementDetectorModule.cpp
    Source/audio/modules/HumanDetectorModule.h
    Source/audio/modules/HumanDetectorModule.cpp
    Source/audio/modules/PoseEstimatorModule.h
    Source/audio/modules/PoseEstimatorModule.cpp
    Source/audio/modules/HandTrackerModule.h
    Source/audio/modules/HandTrackerModule.cpp
    Source/audio/modules/FaceTrackerModule.h
    Source/audio/modules/FaceTrackerModule.cpp
    Source/audio/modules/ObjectDetectorModule.h
    Source/audio/modules/ObjectDetectorModule.cpp
    Source/audio/modules/ColorTrackerModule.h
    Source/audio/modules/ColorTrackerModule.cpp
    Source/audio/modules/ContourDetectorModule.h
    Source/audio/modules/ContourDetectorModule.cpp
    Source/audio/modules/CropVideoModule.h
    Source/audio/modules/CropVideoModule.cpp
    Source/audio/modules/NdiReceiverModule.h
    Source/audio/modules/NdiReceiverModule.cpp
    # Wireless/Streaming Audio Receivers
    Source/audio/modules/SdrReceiverModule.h
    Source/audio/modules/SdrReceiverModule.cpp
    Source/audio/modules/InternetRadioReceiverModule.h
    Source/audio/modules/InternetRadioReceiverModule.cpp
    Source/audio/modules/SystemAudioCaptureModule.h
    Source/audio/modules/SystemAudioCaptureModule.cpp
    Source/audio/modules/SrtReceiverModule.h
    Source/audio/modules/SrtReceiverModule.cpp
    Source/audio/modules/RtmpReceiverModule.h
    Source/audio/modules/RtmpReceiverModule.cpp
    Source/audio/modules/BluetoothAudioReceiverModule.h
    Source/audio/modules/BluetoothAudioReceiverModule.cpp
    Source/animation/AnimationData.h
    Source/animation/RawAnimationData.h
    Source/animation/RawAnimationData.cpp
    Source/animation/AnimationBinder.h
    Source/animation/AnimationBinder.cpp
    Source/animation/Animator.h
    Source/animation/Animator.cpp
    Source/animation/AnimationFileLoader.h
    Source/animation/AnimationFileLoader.cpp
    Source/animation/GltfLoader.h
    Source/animation/GltfLoader.cpp
    Source/animation/FbxLoader.h
    Source/animation/FbxLoader.cpp
    Source/animation/AnimationRenderer.h
    Source/animation/AnimationRenderer.cpp
    Source/preset_creator/VoiceDownloadDialog.h
    Source/preset_creator/VoiceDownloadDialog.cpp
    Source/audio/voices/VoiceDownloadHelper.h
    Source/audio/voices/VoiceDownloadHelper.cpp
    Source/audio/voices/VoiceDownloadThread.h
    Source/audio/voices/VoiceDownloadThread.cpp
)

# Copy splash screen image to executable directory
add_custom_command(
    TARGET PresetCreatorApp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:PresetCreatorApp>/icons"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/../icons/splash clean.png"
            "$<TARGET_FILE_DIR:PresetCreatorApp>/icons/splash.png"
    COMMENT "Copying splash screen image to executable directory"
)

# This target now needs to know where to find all the headers
target_include_directories(PresetCreatorApp PRIVATE
    ${imgui_fc_SOURCE_DIR}
    ${imgui_fc_SOURCE_DIR}/backends
    ${imnodes_fc_SOURCE_DIR}
    ${imgui_juce_fc_SOURCE_DIR}
    ${PIPER_DIR}
    ${ONNXRUNTIME_DIR}/include
    ${CMAKE_SOURCE_DIR}/../vendor/openvino_toolkit_windows_2025.3.0.19807.44526285f24_x86_64/samples/cpp/thirdparty/nlohmann_json/single_include
    ${CMAKE_SOURCE_DIR}/../soundtouch/include
    ${CMAKE_SOURCE_DIR}/../soundtouch/source/SoundTouch
    ${ufbx_SOURCE_DIR}
    $<$<BOOL:${FFMPEG_FOUND}>:${FFMPEG_INCLUDE_DIR}>
    # OpenCV include directories - source and generated headers
    ${opencv_SOURCE_DIR}/include
    ${opencv_SOURCE_DIR}/modules/core/include
    ${opencv_SOURCE_DIR}/modules/imgproc/include
    ${opencv_SOURCE_DIR}/modules/imgcodecs/include
    ${opencv_SOURCE_DIR}/modules/videoio/include
    ${opencv_SOURCE_DIR}/modules/highgui/include
    ${opencv_SOURCE_DIR}/modules/video/include
    ${opencv_SOURCE_DIR}/modules/objdetect/include
    ${opencv_SOURCE_DIR}/modules/features2d/include
    ${opencv_SOURCE_DIR}/modules/calib3d/include
    ${opencv_SOURCE_DIR}/modules/dnn/include              # For DNN headers (dnn.hpp)
    ${OPENCV_BUILD_DIR}
    ${OPENCV_BUILD_DIR}/modules/world
    # OpenCV CUDA modules (from opencv_contrib source)
    $<$<BOOL:${CUDA_SUPPORT_ENABLED}>:${opencv_contrib_SOURCE_DIR}/modules/cudaimgproc/include>
    $<$<BOOL:${CUDA_SUPPORT_ENABLED}>:${opencv_contrib_SOURCE_DIR}/modules/cudawarping/include>
    $<$<BOOL:${CUDA_SUPPORT_ENABLED}>:${opencv_contrib_SOURCE_DIR}/modules/cudaarithm/include>
    $<$<BOOL:${CUDA_SUPPORT_ENABLED}>:${opencv_contrib_SOURCE_DIR}/modules/cudafilters/include>
    $<$<BOOL:${CUDA_SUPPORT_ENABLED}>:${opencv_contrib_SOURCE_DIR}/modules/cudafeatures2d/include>
    $<$<BOOL:${CUDA_SUPPORT_ENABLED}>:${opencv_contrib_SOURCE_DIR}/modules/cudaobjdetect/include>
    $<$<BOOL:${CUDA_SUPPORT_ENABLED}>:${opencv_contrib_SOURCE_DIR}/modules/cudaoptflow/include>
    $<$<BOOL:${USE_RUBBERBAND}>:${RUBBERBAND_INCLUDE_DIR}>
    # ASIO SDK headers (Windows only, conditional)
    $<$<BOOL:${ASIO_SDK_FOUND}>:${ASIO_SDK_DIR}/common>
    $<$<BOOL:${ASIO_SDK_FOUND}>:${ASIO_SDK_DIR}/host>
    # STK and Essentia headers
    $<$<BOOL:${STK_FOUND}>:${STK_SOURCE_DIR}/include>
    $<$<BOOL:${ESSENTIA_FOUND}>:${ESSENTIA_DIR}/src>
    # NDI SDK headers
    $<$<BOOL:${NDI_FOUND}>:${NDI_INCLUDE_DIR}>
)

# This target also needs the compile definitions
target_compile_definitions(PresetCreatorApp PRIVATE
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:PresetCreatorApp,PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:PresetCreatorApp,VERSION>"
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
    JUCE_PLUGINHOST_VST3=1
    IMGUI_IMPL_JUCE_BEZEL=0
    IMGUI_ENABLE_VIEWPORTS
    IMGUI_DEFINE_MATH_OPERATORS
    IMNODES_NAMESPACE=ImNodes
    IMNODES_STATIC_DEFINE
    PRESET_CREATOR_UI=1
    # Prevent winsock.h/winsock2.h conflict on Windows
    $<$<PLATFORM_ID:Windows>:WIN32_LEAN_AND_MEAN>
    $<$<BOOL:${USE_RUBBERBAND}>:USE_RUBBERBAND=1>
    $<$<NOT:$<BOOL:${USE_RUBBERBAND}>>:USE_RUBBERBAND=0>
    $<$<BOOL:${CUDA_SUPPORT_ENABLED}>:WITH_CUDA_SUPPORT=1>
    # Enable ASIO if SDK is found
    $<$<BOOL:${ASIO_SDK_FOUND}>:JUCE_ASIO=1>
    # Enable STK if found
    $<$<BOOL:${STK_FOUND}>:STK_FOUND=1>
    # Enable Essentia if found
    $<$<BOOL:${ESSENTIA_FOUND}>:ESSENTIA_FOUND=1>
    # Enable NDI if found
    $<$<BOOL:${NDI_FOUND}>:NDI_FOUND=1>
)

target_link_libraries(PresetCreatorApp PRIVATE
    juce::juce_gui_extra
    juce::juce_opengl
    juce::juce_audio_devices
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_audio_formats
    juce::juce_dsp
    juce::juce_osc
    juce::juce_cryptography
    imgui_impl_juce
    soundtouch
    $<$<BOOL:${STK_FOUND}>:stk>
    $<$<BOOL:${ESSENTIA_FOUND}>:${ESSENTIA_LIBRARY}>
    box2d
    glm::glm
    tinygltf
    ufbx_static
    ${OPENCV_LINK_LIBS}       # opencv_world for source-built, or specific libs for pre-built
    ${OPENCV_3RDPARTY_LIBS}   # OpenCV's 3rdparty deps (zlib, png, jpeg, protobuf, etc.)
    ${OPENCV_CUDA_LIBS}       # cuBLAS, cuDNN for DNN CUDA backend
    $<$<BOOL:${USE_RUBBERBAND}>:${RUBBERBAND_TARGET}>
    $<$<BOOL:${FFMPEG_FOUND}>:${AVFORMAT_LIBRARY}>
    $<$<BOOL:${FFMPEG_FOUND}>:${AVCODEC_LIBRARY}>
    $<$<BOOL:${FFMPEG_FOUND}>:${AVUTIL_LIBRARY}>
    $<$<BOOL:${FFMPEG_FOUND}>:${SWRESAMPLE_LIBRARY}>
    # Piper TTS libraries (pre-built)
    ${ONNXRUNTIME_DIR}/lib/onnxruntime.lib
)

# Link CUDA runtime for OpenCV CUDA symbols (legacy fallback, OPENCV_CUDA_LIBS above preferred)
if(NOT OPENCV_CUDA_LIBS)
    if(TARGET CUDAToolkit::cudart)
        target_link_libraries(PresetCreatorApp PRIVATE CUDAToolkit::cudart)
    elseif(CUDA_CUDART_LIBRARY)
        target_link_libraries(PresetCreatorApp PRIVATE ${CUDA_CUDART_LIBRARY})
    endif()
endif()

# Link Windows libraries for native camera enumeration (DirectShow)
if(WIN32)
    target_link_libraries(PresetCreatorApp PRIVATE ole32 strmiids)
endif()

# Link NDI library if found
if(NDI_FOUND AND NDI_LIBRARY)
    target_link_libraries(PresetCreatorApp PRIVATE ${NDI_LIBRARY})
endif()

# Link SRT if found (for SRT Receiver module)
if(SRT_FOUND)
    target_link_libraries(PresetCreatorApp PRIVATE ${SRT_LIBRARIES})
    if(SRT_INCLUDE_DIRS)
        target_include_directories(PresetCreatorApp PRIVATE ${SRT_INCLUDE_DIRS})
    endif()
    target_compile_definitions(PresetCreatorApp PRIVATE HAS_SRT=1)
endif()

# Link WinRT for Bluetooth Audio and System Audio Capture (Windows only)
if(WIN32 AND CPPWINRT_AVAILABLE)
    target_link_libraries(PresetCreatorApp PRIVATE WindowsApp.lib)
    target_compile_definitions(PresetCreatorApp PRIVATE HAS_WINRT_AUDIO=1)
endif()

# Link librtlsdr if found
if(LIBRTLSDR_FOUND)
    target_link_libraries(PresetCreatorApp PRIVATE ${LIBRTLSDR_LIBRARIES})
    if(LIBRTLSDR_INCLUDE_DIRS)
        target_include_directories(PresetCreatorApp PRIVATE ${LIBRTLSDR_INCLUDE_DIRS})
    endif()
    target_compile_definitions(PresetCreatorApp PRIVATE HAS_LIBRTLSDR=1)
endif()

# ==============================================================================
# FORCE EMBEDDED DEBUG INFO (/Z7) FOR ALL TARGETS TO FIX NINJA PDB LOCKING
# ==============================================================================
if(MSVC)
    target_compile_options(soundtouch PRIVATE "$<$<CONFIG:Debug>:/Z7>")
    target_compile_options(box2d PRIVATE "$<$<CONFIG:Debug>:/Z7>")
    target_compile_options(ufbx_static PRIVATE "$<$<CONFIG:Debug>:/Z7>")
    target_compile_options(rubberband_single PRIVATE "$<$<CONFIG:Debug>:/Z7>")
    target_compile_options(ColliderApp PRIVATE "$<$<CONFIG:Debug>:/Z7>")
    target_compile_options(PresetCreatorApp PRIVATE "$<$<CONFIG:Debug>:/Z7>")
endif()

# Add compile definition to signal zoom mode for app code if enabled
if(PRESET_CREATOR_IMNODES_ZOOM)
    target_compile_definitions(PresetCreatorApp PRIVATE IMNODES_ZOOM_ENABLED=1)
endif()

# ============================================================================
# PikonUpdater - Standalone update installer
# ============================================================================
add_executable(PikonUpdater
    Source/updater/PikonUpdater/PikonUpdater.cpp
)

target_link_libraries(PikonUpdater
    PRIVATE
    juce::juce_core
    juce::juce_cryptography
)

# Prevent winsock.h/winsock2.h conflict on Windows
if(WIN32)
    target_compile_definitions(PikonUpdater PRIVATE WIN32_LEAN_AND_MEAN)
endif()

# Console app (not GUI)
set_target_properties(PikonUpdater PROPERTIES
    WIN32_EXECUTABLE FALSE
)

# Copy to output directory next to main app (same as PresetCreatorApp)
set_target_properties(PikonUpdater PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/PresetCreatorApp_artefacts/$<CONFIG>"
)

if(WIN32)
    # Copy Piper executable and DLLs to output directories
    add_custom_command(
        TARGET ColliderApp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${PIPER_DIR}/piper.exe"
                "$<TARGET_FILE_DIR:ColliderApp>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${PIPER_DIR}/piper_phonemize.dll"
                "$<TARGET_FILE_DIR:ColliderApp>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${PIPER_DIR}/espeak-ng.dll"
                "$<TARGET_FILE_DIR:ColliderApp>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${ONNXRUNTIME_DIR}/lib/onnxruntime.dll"
                "$<TARGET_FILE_DIR:ColliderApp>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${ONNXRUNTIME_DIR}/lib/onnxruntime_providers_shared.dll"
                "$<TARGET_FILE_DIR:ColliderApp>"
        # OpenCV FFmpeg DLL copying is handled separately below (if not pre-built static)
    )
    
    # Copy OpenCV FFmpeg video backend DLL (only for source-built OpenCV, pre-built is static)
    if(NOT OPENCV_PREBUILT)
        add_custom_command(
            TARGET ColliderApp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_BINARY_DIR}/bin/opencv_videoio_ffmpeg4140_64.dll"
                    "$<TARGET_FILE_DIR:ColliderApp>"
            COMMENT "Copying OpenCV FFmpeg DLL to ColliderApp (source-built)"
        )
    endif()
    
    # Copy SRT DLL if found (for SRT Receiver module)
    if(SRT_FOUND AND SRT_DLL)
        add_custom_command(
            TARGET ColliderApp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${SRT_DLL}"
                    "$<TARGET_FILE_DIR:ColliderApp>"
            COMMENT "Copying SRT DLL to ColliderApp"
        )
        add_custom_command(
            TARGET PresetCreatorApp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${SRT_DLL}"
                    "$<TARGET_FILE_DIR:PresetCreatorApp>"
            COMMENT "Copying SRT DLL to PresetCreatorApp"
        )
    endif()
    
    # Copy FFmpeg DLLs separately (if FFmpeg is found)
    # We need to copy each DLL individually to avoid generator expression issues
    if(FFMPEG_FOUND AND EXISTS "${FFMPEG_BIN_DIR}")
        file(GLOB FFMPEG_DLLS "${FFMPEG_BIN_DIR}/*.dll")
        if(FFMPEG_DLLS)
            foreach(DLL ${FFMPEG_DLLS})
                get_filename_component(DLL_NAME ${DLL} NAME)
                add_custom_command(
                    TARGET ColliderApp POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            "${DLL}"
                            "$<TARGET_FILE_DIR:ColliderApp>"
                    COMMENT "Copying ${DLL_NAME} to ColliderApp"
                )
                add_custom_command(
                    TARGET PresetCreatorApp POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            "${DLL}"
                            "$<TARGET_FILE_DIR:PresetCreatorApp>"
                    COMMENT "Copying ${DLL_NAME} to PresetCreatorApp"
                )
            endforeach()
        endif()
    endif()
    
    # Copy CUDA DLLs from vendor folder to PresetCreatorApp
    if(EXISTS "${CMAKE_SOURCE_DIR}/../vendor/cuda")
        file(GLOB CUDA_VENDOR_DLLS "${CMAKE_SOURCE_DIR}/../vendor/cuda/*.dll")
        if(CUDA_VENDOR_DLLS)
            foreach(DLL ${CUDA_VENDOR_DLLS})
                get_filename_component(DLL_NAME ${DLL} NAME)
                add_custom_command(
                    TARGET PresetCreatorApp POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            "${DLL}"
                            "$<TARGET_FILE_DIR:PresetCreatorApp>"
                    COMMENT "Copying ${DLL_NAME} to PresetCreatorApp"
                )
            endforeach()
        endif()
    endif()
    
    # Continue with other files for ColliderApp
    add_custom_command(
        TARGET ColliderApp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${PIPER_DIR}/espeak-ng-data"
                "$<TARGET_FILE_DIR:ColliderApp>/espeak-ng-data"
        COMMENT "Copying espeak-ng-data to ColliderApp output directory"
    )
    
    # ==============================================================================
    # Copy STK runtime assets (following Piper pattern)
    # ==============================================================================
    if(STK_FOUND)
        # Log STK source paths before copying
        message(STATUS "STK Source Path: ${STK_SOURCE_DIR}")
        message(STATUS "STK rawwaves Source: ${STK_SOURCE_DIR}/rawwaves")
        
        # Copy STK rawwaves directory (required for some STK instruments)
        if(EXISTS "${STK_SOURCE_DIR}/rawwaves")
            add_custom_command(
                TARGET ColliderApp POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E echo "Copying STK rawwaves from: ${STK_SOURCE_DIR}/rawwaves"
                COMMAND ${CMAKE_COMMAND} -E echo "Copying STK rawwaves to: $<TARGET_FILE_DIR:ColliderApp>/rawwaves"
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                        "${STK_SOURCE_DIR}/rawwaves"
                        "$<TARGET_FILE_DIR:ColliderApp>/rawwaves"
                COMMENT "Copying STK rawwaves to ColliderApp output directory"
            )

            add_custom_command(
                TARGET PresetCreatorApp POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E echo "Copying STK rawwaves from: ${STK_SOURCE_DIR}/rawwaves"
                COMMAND ${CMAKE_COMMAND} -E echo "Copying STK rawwaves to: $<TARGET_FILE_DIR:PresetCreatorApp>/rawwaves"
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                        "${STK_SOURCE_DIR}/rawwaves"
                        "$<TARGET_FILE_DIR:PresetCreatorApp>/rawwaves"
                COMMENT "Copying STK rawwaves to PresetCreatorApp output directory"
            )

        else()
            message(WARNING "STK rawwaves directory not found at ${STK_SOURCE_DIR}/rawwaves")
        endif()
    endif()

    if(YTDLP_FOUND)
        add_custom_command(
            TARGET ColliderApp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${YTDLP_EXECUTABLE}"
                    "$<TARGET_FILE_DIR:ColliderApp>"
            COMMENT "Copying yt-dlp.exe to ColliderApp output directory"
        )
        add_custom_command(
            TARGET PresetCreatorApp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${YTDLP_EXECUTABLE}"
                    "$<TARGET_FILE_DIR:PresetCreatorApp>"
            COMMENT "Copying yt-dlp.exe to PresetCreatorApp output directory"
        )
    endif()
    
    # ==============================================================================
    # Copy Essentia runtime assets (following Piper pattern)
    # ==============================================================================
    if(ESSENTIA_FOUND)
        # Log Essentia source paths before copying
        message(STATUS "Essentia Source Path: ${ESSENTIA_DIR}")
        
        # Essentia may need models or data files at runtime
        # Check for common Essentia runtime directories
        if(EXISTS "${ESSENTIA_DIR}/models")
            add_custom_command(
                TARGET ColliderApp POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E echo "Copying Essentia models from: ${ESSENTIA_DIR}/models"
                COMMAND ${CMAKE_COMMAND} -E echo "Copying Essentia models to: $<TARGET_FILE_DIR:ColliderApp>/essentia/models"
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                        "${ESSENTIA_DIR}/models"
                        "$<TARGET_FILE_DIR:ColliderApp>/essentia/models"
                COMMENT "Copying Essentia models to ColliderApp output directory"
            )
            add_custom_command(
                TARGET PresetCreatorApp POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E echo "Copying Essentia models from: ${ESSENTIA_DIR}/models"
                COMMAND ${CMAKE_COMMAND} -E echo "Copying Essentia models to: $<TARGET_FILE_DIR:PresetCreatorApp>/essentia/models"
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                        "${ESSENTIA_DIR}/models"
                        "$<TARGET_FILE_DIR:PresetCreatorApp>/essentia/models"
                COMMENT "Copying Essentia models to PresetCreatorApp output directory"
            )
        endif()
        
        # Copy Essentia data files if they exist
        if(EXISTS "${ESSENTIA_DIR}/data")
            add_custom_command(
                TARGET ColliderApp POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E echo "Copying Essentia data from: ${ESSENTIA_DIR}/data"
                COMMAND ${CMAKE_COMMAND} -E echo "Copying Essentia data to: $<TARGET_FILE_DIR:ColliderApp>/essentia/data"
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                        "${ESSENTIA_DIR}/data"
                        "$<TARGET_FILE_DIR:ColliderApp>/essentia/data"
                COMMENT "Copying Essentia data to ColliderApp output directory"
            )
            add_custom_command(
                TARGET PresetCreatorApp POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E echo "Copying Essentia data from: ${ESSENTIA_DIR}/data"
                COMMAND ${CMAKE_COMMAND} -E echo "Copying Essentia data to: $<TARGET_FILE_DIR:PresetCreatorApp>/essentia/data"
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                        "${ESSENTIA_DIR}/data"
                        "$<TARGET_FILE_DIR:PresetCreatorApp>/essentia/data"
                COMMENT "Copying Essentia data to PresetCreatorApp output directory"
            )
        endif()
    endif()
    add_custom_command(
        TARGET ColliderApp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${OPENCV_DATA_DIR}/haarcascades/haarcascade_frontalface_default.xml"
                "$<TARGET_FILE_DIR:ColliderApp>"
        COMMENT "Copying OpenCV haarcascade data to ColliderApp output directory"
    )
    add_custom_command(
        TARGET PresetCreatorApp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${PIPER_DIR}/piper.exe"
                "$<TARGET_FILE_DIR:PresetCreatorApp>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${PIPER_DIR}/piper_phonemize.dll"
                "$<TARGET_FILE_DIR:PresetCreatorApp>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${PIPER_DIR}/espeak-ng.dll"
                "$<TARGET_FILE_DIR:PresetCreatorApp>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${ONNXRUNTIME_DIR}/lib/onnxruntime.dll"
                "$<TARGET_FILE_DIR:PresetCreatorApp>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${ONNXRUNTIME_DIR}/lib/onnxruntime_providers_shared.dll"
                "$<TARGET_FILE_DIR:PresetCreatorApp>"
        # OpenCV FFmpeg DLL copying is handled separately below (if not pre-built static)
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${PIPER_DIR}/espeak-ng-data"
                "$<TARGET_FILE_DIR:PresetCreatorApp>/espeak-ng-data"
        # NEW (Corrected): Copy the entire assets folder
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_SOURCE_DIR}/assets"
                "$<TARGET_FILE_DIR:PresetCreatorApp>/assets"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${OPENCV_DATA_DIR}/haarcascades/haarcascade_frontalface_default.xml"
                "$<TARGET_FILE_DIR:PresetCreatorApp>"
        COMMENT "Copying Piper TTS, OpenPose, FFmpeg, and other runtime assets to PresetCreatorApp output directory"
    )
    
    # Copy OpenCV FFmpeg video backend DLL (only for source-built OpenCV, pre-built is static)
    if(NOT OPENCV_PREBUILT)
        add_custom_command(
            TARGET PresetCreatorApp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_BINARY_DIR}/bin/opencv_videoio_ffmpeg4140_64.dll"
                    "$<TARGET_FILE_DIR:PresetCreatorApp>"
            COMMENT "Copying OpenCV FFmpeg DLL to PresetCreatorApp (source-built)"
        )
    endif()

    add_custom_command(
        TARGET PresetCreatorApp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:PresetCreatorApp>/themes"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_SOURCE_DIR}/Source/preset_creator/theme/presets"
                "$<TARGET_FILE_DIR:PresetCreatorApp>/themes"
        COMMENT "Copying theme presets to PresetCreatorApp/themes"
    )

    add_custom_command(
        TARGET PresetCreatorApp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_SOURCE_DIR}/assets/fonts"
                "$<TARGET_FILE_DIR:PresetCreatorApp>/fonts"
        COMMENT "Copying fonts to PresetCreatorApp/fonts"
    )

    add_custom_command(
        TARGET PresetCreatorApp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:PresetCreatorApp>/USER_MANUAL"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_SOURCE_DIR}/../USER_MANUAL"
                "$<TARGET_FILE_DIR:PresetCreatorApp>/USER_MANUAL"
        COMMENT "Copying USER_MANUAL documentation files to PresetCreatorApp/USER_MANUAL"
    )

    add_custom_command(
        TARGET ColliderApp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:ColliderApp>/themes"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_SOURCE_DIR}/Source/preset_creator/theme/presets"
                "$<TARGET_FILE_DIR:ColliderApp>/themes"
        COMMENT "Copying theme presets to ColliderApp/themes"
    )
endif()

# ==============================================================================
# Pikon Raditsz Audio App Target (No-OpenCV / No-CUDA)
# ==============================================================================

juce_add_gui_app(PikonRaditszAudioApp
    PRODUCT_NAME "Pikon Raditsz Audio"
    VERSION "0.1.0"
    COMPANY_NAME "Collider"
    ICON_BIG "${CMAKE_SOURCE_DIR}/../icons/pikon_raditsz_icon-audio.png"
    ICON_SMALL "${CMAKE_SOURCE_DIR}/../icons/pikon_raditsz_icon-audio.png"
)

target_sources(PikonRaditszAudioApp PRIVATE
    # Core Application Sources (Same as PresetCreatorApp)
    Source/preset_creator/PresetCreatorMain.cpp
    Source/preset_creator/PresetCreatorApplication.h
    Source/preset_creator/PinDatabase.h
    Source/preset_creator/PinDatabase.cpp
    Source/preset_creator/NotificationManager.h
    Source/preset_creator/NotificationManager.cpp
    Source/preset_creator/PresetValidator.h
    Source/preset_creator/PresetValidator.cpp
    Source/preset_creator/PresetAutoHealer.h
    Source/preset_creator/PresetAutoHealer.cpp
    Source/preset_creator/SavePresetJob.h
    Source/preset_creator/SavePresetJob.cpp
    Source/preset_creator/ShortcutManager.h
    Source/preset_creator/ShortcutManager.cpp
    Source/preset_creator/ImGuiNodeEditorComponent.h
    Source/preset_creator/ImGuiNodeEditorComponent.cpp
    Source/preset_creator/PatchGenerator.h
    Source/preset_creator/PatchGenerator.cpp
    Source/preset_creator/PresetCreatorComponent.h
    Source/preset_creator/PresetCreatorComponent.cpp
    Source/preset_creator/ControllerPresetManager.h
    Source/preset_creator/ControllerPresetManager.cpp
    Source/preset_creator/VstManager.h
    Source/preset_creator/VstManager.cpp
    Source/preset_creator/SplashScreenComponent.h
    Source/preset_creator/SplashScreenComponent.cpp
    
    # Version and utilities
    Source/utils/VersionInfo.h
    
    # Auto-updater system
    Source/updater/UpdaterTypes.h
    Source/updater/UpdaterTypes.cpp
    Source/updater/HashVerifier.h
    Source/updater/HashVerifier.cpp
    Source/updater/VersionManager.h
    Source/updater/VersionManager.cpp
    Source/updater/UpdateChecker.h
    Source/updater/UpdateChecker.cpp
    Source/updater/FileDownloader.h
    Source/updater/FileDownloader.cpp
    Source/updater/UpdateApplier.h
    Source/updater/UpdateApplier.cpp
    Source/updater/UpdateManager.h
    Source/updater/UpdateManager.cpp
    Source/updater/ui/UpdateAvailableDialog.h
    Source/updater/ui/UpdateAvailableDialog.cpp
    Source/updater/ui/DownloadProgressDialog.h
    Source/updater/ui/DownloadProgressDialog.cpp
    Source/updater/ui/UpdateDownloadDialog.h
    Source/updater/ui/UpdateDownloadDialog.cpp
    
    # Theme system
    Source/preset_creator/theme/Theme.h
    Source/preset_creator/theme/ThemeManager.h
    Source/preset_creator/theme/ThemeManager.cpp
    Source/preset_creator/theme/ThemeEditorComponent.h
    Source/preset_creator/theme/ThemeEditorComponent.cpp
    Source/preset_creator/HelpManagerComponent.h
    Source/preset_creator/HelpManagerComponent.cpp
    Source/preset_creator/VoiceDownloadDialog.h
    Source/preset_creator/VoiceDownloadDialog.cpp

    # ImGui Sources
    ${imgui_fc_SOURCE_DIR}/imgui.cpp
    ${imgui_fc_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_fc_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_fc_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_fc_SOURCE_DIR}/backends/imgui_impl_opengl2.cpp
    ${imnodes_fc_SOURCE_DIR}/imnodes.cpp

    # Audio Engine Modules (EXCLUDING Video/CV)
    Source/audio/MidiDeviceManager.h
    Source/audio/MidiDeviceManager.cpp
    Source/audio/OscDeviceManager.h
    Source/audio/OscDeviceManager.cpp
    Source/audio/graph/ModularSynthProcessor.h
    Source/audio/graph/ModularSynthProcessor.cpp
    Source/audio/modules/ModuleProcessor.h
    Source/audio/modules/ModuleProcessor.cpp
    Source/audio/modules/AudioInputModuleProcessor.h
    Source/audio/modules/AudioInputModuleProcessor.cpp
    Source/audio/modules/VCOModuleProcessor.h
    Source/audio/modules/VCOModuleProcessor.cpp
    Source/audio/modules/stk/StkWrapper.h
    Source/audio/modules/stk/StkWrapper.cpp
    Source/audio/modules/stk/StkStringModuleProcessor.h
    Source/audio/modules/stk/StkStringModuleProcessor.cpp
    Source/audio/modules/stk/StkWindModuleProcessor.h
    Source/audio/modules/stk/StkWindModuleProcessor.cpp
    Source/audio/modules/stk/StkPercussionModuleProcessor.h
    Source/audio/modules/stk/StkPercussionModuleProcessor.cpp
    Source/audio/modules/stk/StkPluckedModuleProcessor.h
    Source/audio/modules/stk/StkPluckedModuleProcessor.cpp
    Source/audio/modules/essentia/EssentiaWrapper.h
    Source/audio/modules/essentia/EssentiaWrapper.cpp
    Source/audio/modules/essentia/EssentiaOnsetDetectorModuleProcessor.h
    Source/audio/modules/essentia/EssentiaOnsetDetectorModuleProcessor.cpp
    Source/audio/modules/essentia/EssentiaPitchTrackerModuleProcessor.h
    Source/audio/modules/essentia/EssentiaPitchTrackerModuleProcessor.cpp
    Source/audio/modules/VCFModuleProcessor.h
    Source/audio/modules/VCFModuleProcessor.cpp
    Source/audio/modules/VCAModuleProcessor.h
    Source/audio/modules/VCAModuleProcessor.cpp
    Source/audio/modules/NoiseModuleProcessor.h
    Source/audio/modules/NoiseModuleProcessor.cpp
    Source/audio/modules/LFOModuleProcessor.h
    Source/audio/modules/LFOModuleProcessor.cpp
    Source/audio/modules/ADSRModuleProcessor.h
    Source/audio/modules/ADSRModuleProcessor.cpp
    Source/audio/modules/MixerModuleProcessor.h
    Source/audio/modules/MixerModuleProcessor.cpp
    Source/audio/modules/CVMixerModuleProcessor.h
    Source/audio/modules/CVMixerModuleProcessor.cpp
    Source/audio/modules/DelayModuleProcessor.h
    Source/audio/modules/DelayModuleProcessor.cpp
    Source/audio/modules/ReverbModuleProcessor.h
    Source/audio/modules/ReverbModuleProcessor.cpp
    Source/audio/modules/SAndHModuleProcessor.h
    Source/audio/modules/SAndHModuleProcessor.cpp
    Source/audio/modules/AttenuverterModuleProcessor.h
    Source/audio/modules/AttenuverterModuleProcessor.cpp
    Source/audio/modules/ScopeModuleProcessor.h
    Source/audio/modules/ScopeModuleProcessor.cpp
    Source/audio/modules/StepSequencerModuleProcessor.h
    Source/audio/modules/StepSequencerModuleProcessor.cpp
    Source/audio/modules/MathModuleProcessor.h
    Source/audio/modules/MathModuleProcessor.cpp
    Source/audio/modules/MapRangeModuleProcessor.h
    Source/audio/modules/MapRangeModuleProcessor.cpp
    Source/audio/modules/ComparatorModuleProcessor.h
    Source/audio/modules/ComparatorModuleProcessor.cpp
    Source/audio/modules/RandomModuleProcessor.h
    Source/audio/modules/RandomModuleProcessor.cpp
    Source/audio/modules/RateModuleProcessor.h
    Source/audio/modules/RateModuleProcessor.cpp
    Source/audio/modules/QuantizerModuleProcessor.h
    Source/audio/modules/QuantizerModuleProcessor.cpp
    Source/audio/modules/SequentialSwitchModuleProcessor.h
    Source/audio/modules/SequentialSwitchModuleProcessor.cpp
    Source/audio/modules/LogicModuleProcessor.h
    Source/audio/modules/LogicModuleProcessor.cpp
    Source/audio/modules/ClockDividerModuleProcessor.h
    Source/audio/modules/ClockDividerModuleProcessor.cpp
    Source/audio/modules/WaveshaperModuleProcessor.h
    Source/audio/modules/WaveshaperModuleProcessor.cpp
    Source/audio/modules/MultiBandShaperModuleProcessor.h
    Source/audio/modules/MultiBandShaperModuleProcessor.cpp
    Source/audio/modules/GranulatorModuleProcessor.h
    Source/audio/modules/GranulatorModuleProcessor.cpp
    Source/audio/modules/SpatialGranulatorModuleProcessor.h
    Source/audio/modules/SpatialGranulatorModuleProcessor.cpp
    Source/audio/modules/HarmonicShaperModuleProcessor.h
    Source/audio/modules/HarmonicShaperModuleProcessor.cpp
    Source/audio/modules/ValueModuleProcessor.h
    Source/audio/modules/ValueModuleProcessor.cpp
    Source/audio/modules/DebugModuleProcessor.h
    Source/audio/modules/DebugModuleProcessor.cpp
    Source/audio/modules/CommentModuleProcessor.h
    Source/audio/modules/CommentModuleProcessor.cpp
    Source/audio/modules/InputDebugModuleProcessor.h
    Source/audio/modules/InputDebugModuleProcessor.cpp
    Source/audio/modules/TimePitchModuleProcessor.h
    Source/audio/modules/TimePitchModuleProcessor.cpp
    Source/audio/modules/MIDIPlayerModuleProcessor.h
    Source/audio/modules/MIDIPlayerModuleProcessor.cpp
    Source/audio/modules/TrackMixerModuleProcessor.h
    Source/audio/modules/TrackMixerModuleProcessor.cpp
    Source/audio/modules/PolyVCOModuleProcessor.h
    Source/audio/modules/PolyVCOModuleProcessor.cpp
    Source/audio/modules/RerouteModuleProcessor.h
    Source/audio/modules/RerouteModuleProcessor.cpp
    Source/audio/modules/TTSPerformerModuleProcessor.h
    Source/audio/modules/TTSPerformerModuleProcessor.cpp
    Source/audio/modules/TimingData.h
    Source/audio/modules/SampleLoaderModuleProcessor.h
    Source/audio/modules/SampleLoaderModuleProcessor.cpp
    Source/audio/modules/SampleSfxModuleProcessor.h
    Source/audio/modules/SampleSfxModuleProcessor.cpp
    Source/audio/modules/FunctionGeneratorModuleProcessor.h
    Source/audio/modules/FunctionGeneratorModuleProcessor.cpp
    Source/audio/modules/BestPracticeNodeProcessor.h
    Source/audio/modules/BestPracticeNodeProcessor.cpp
    Source/audio/modules/TimelineModuleProcessor.h
    Source/audio/modules/TimelineModuleProcessor.cpp
    Source/audio/modules/ShapingOscillatorModuleProcessor.h
    Source/audio/modules/ShapingOscillatorModuleProcessor.cpp
    Source/audio/voices/SampleVoiceProcessor.h
    Source/audio/voices/SampleVoiceProcessor.cpp
    Source/audio/voices/VoiceDownloadHelper.h
    Source/audio/voices/VoiceDownloadHelper.cpp
    Source/audio/voices/VoiceDownloadThread.h
    Source/audio/voices/VoiceDownloadThread.cpp
    Source/audio/graph/VoiceProcessor.h
    Source/audio/graph/VoiceProcessor.cpp
    Source/audio/assets/SampleBank.h
    Source/audio/assets/SampleBank.cpp
    Source/utils/RtLogger.h
    Source/utils/RtLogger.cpp
    Source/audio/modules/VocalTractFilterModuleProcessor.h
    Source/audio/modules/VocalTractFilterModuleProcessor.cpp
    Source/audio/modules/VstHostModuleProcessor.h
    Source/audio/modules/VstHostModuleProcessor.cpp
    Source/audio/modules/MultiSequencerModuleProcessor.h
    Source/audio/modules/MultiSequencerModuleProcessor.cpp
    Source/audio/modules/ChordArpModuleProcessor.h
    Source/audio/modules/ChordArpModuleProcessor.cpp
    Source/audio/modules/LagProcessorModuleProcessor.h
    Source/audio/modules/LagProcessorModuleProcessor.cpp
    Source/audio/modules/DeCrackleModuleProcessor.h
    Source/audio/modules/DeCrackleModuleProcessor.cpp
    Source/audio/modules/GraphicEQModuleProcessor.h
    Source/audio/modules/GraphicEQModuleProcessor.cpp
    Source/audio/modules/FrequencyGraphModuleProcessor.h
    Source/audio/modules/FrequencyGraphModuleProcessor.cpp
    Source/audio/modules/ChorusModuleProcessor.h
    Source/audio/modules/ChorusModuleProcessor.cpp
    Source/audio/modules/PhaserModuleProcessor.h
    Source/audio/modules/PhaserModuleProcessor.cpp
    Source/audio/modules/CompressorModuleProcessor.h
    Source/audio/modules/CompressorModuleProcessor.cpp
    Source/audio/modules/RecordModuleProcessor.h
    Source/audio/modules/RecordModuleProcessor.cpp
    Source/audio/modules/LimiterModuleProcessor.h
    Source/audio/modules/LimiterModuleProcessor.cpp
    Source/audio/modules/GateModuleProcessor.h
    Source/audio/modules/GateModuleProcessor.cpp
    Source/audio/modules/DriveModuleProcessor.h
    Source/audio/modules/DriveModuleProcessor.cpp
    Source/audio/modules/BitCrusherModuleProcessor.h
    Source/audio/modules/BitCrusherModuleProcessor.cpp
    Source/audio/modules/PanVolModuleProcessor.h
    Source/audio/modules/PanVolModuleProcessor.cpp
    Source/audio/modules/InletModuleProcessor.h
    Source/audio/modules/InletModuleProcessor.cpp
    Source/audio/modules/OutletModuleProcessor.h
    Source/audio/modules/OutletModuleProcessor.cpp
    Source/audio/modules/MetaModuleProcessor.h
    Source/audio/modules/MetaModuleProcessor.cpp
    Source/audio/modules/SnapshotSequencerModuleProcessor.h
    Source/audio/modules/SnapshotSequencerModuleProcessor.cpp
    Source/audio/modules/MIDICVModuleProcessor.h
    Source/audio/modules/MIDICVModuleProcessor.cpp
        Source/audio/modules/OSCCVModuleProcessor.h
        Source/audio/modules/OSCCVModuleProcessor.cpp
        Source/audio/modules/CVOSCSenderModuleProcessor.h
        Source/audio/modules/CVOSCSenderModuleProcessor.cpp
    Source/audio/modules/MIDIFadersModuleProcessor.h
    Source/audio/modules/MIDIFadersModuleProcessor.cpp
    Source/audio/modules/MIDIKnobsModuleProcessor.h
    Source/audio/modules/MIDIKnobsModuleProcessor.cpp
    Source/audio/modules/MIDIButtonsModuleProcessor.h
    Source/audio/modules/MIDIButtonsModuleProcessor.cpp
    Source/audio/modules/MIDIJogWheelModuleProcessor.h
    Source/audio/modules/MIDIJogWheelModuleProcessor.cpp
    Source/audio/modules/MIDIPadModuleProcessor.h
    Source/audio/modules/MIDIPadModuleProcessor.cpp
    Source/audio/modules/MidiLoggerModuleProcessor.h
    Source/audio/modules/MidiLoggerModuleProcessor.cpp
    Source/audio/modules/TempoClockModuleProcessor.h
    Source/audio/modules/TempoClockModuleProcessor.cpp
    Source/audio/modules/PhysicsModuleProcessor.h
    Source/audio/modules/PhysicsModuleProcessor.cpp
    Source/audio/modules/StrokeSequencerModuleProcessor.h
    Source/audio/modules/StrokeSequencerModuleProcessor.cpp
    Source/audio/modules/TapTempo.h
    Source/audio/modules/TapTempo.cpp
    Source/audio/modules/BPMMonitorModuleProcessor.h
    Source/audio/modules/BPMMonitorModuleProcessor.cpp
    Source/audio/modules/AnimationModuleProcessor.h
    Source/audio/modules/AnimationModuleProcessor.cpp
    Source/audio/modules/AutomationLaneModuleProcessor.h
    Source/audio/modules/AutomationLaneModuleProcessor.cpp
    Source/audio/modules/AutomatoModuleProcessor.h
    Source/audio/modules/AutomatoModuleProcessor.cpp
    
    # Wireless/Streaming Audio Receivers
    Source/audio/modules/SdrReceiverModule.h
    Source/audio/modules/SdrReceiverModule.cpp
    Source/audio/modules/InternetRadioReceiverModule.h
    Source/audio/modules/InternetRadioReceiverModule.cpp
    Source/audio/modules/SystemAudioCaptureModule.h
    Source/audio/modules/SystemAudioCaptureModule.cpp
    Source/audio/modules/SrtReceiverModule.h
    Source/audio/modules/SrtReceiverModule.cpp
    Source/audio/modules/RtmpReceiverModule.h
    Source/audio/modules/RtmpReceiverModule.cpp
    Source/audio/modules/BluetoothAudioReceiverModule.h
    Source/audio/modules/BluetoothAudioReceiverModule.cpp
    
    # Animation Sources (Keep these as they are not OpenCV dependent)
    Source/animation/AnimationData.h
    Source/animation/RawAnimationData.h
    Source/animation/RawAnimationData.cpp
    Source/animation/AnimationBinder.h
    Source/animation/AnimationBinder.cpp
    Source/animation/Animator.h
    Source/animation/Animator.cpp
    Source/animation/AnimationFileLoader.h
    Source/animation/AnimationFileLoader.cpp
    Source/animation/GltfLoader.h
    Source/animation/GltfLoader.cpp
    Source/animation/FbxLoader.h
    Source/animation/FbxLoader.cpp
    Source/animation/AnimationRenderer.h
    Source/animation/AnimationRenderer.cpp
)

# Copy splash screen image to executable directory
add_custom_command(
    TARGET PikonRaditszAudioApp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:PikonRaditszAudioApp>/icons"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/../icons/splash clean.png"
            "$<TARGET_FILE_DIR:PikonRaditszAudioApp>/icons/splash clean.png"
    COMMENT "Copying splash screen image to executable directory"
)

# Include Directories (EXCLUDING OpenCV, but INCLUDING FFmpeg for audio streaming)
target_include_directories(PikonRaditszAudioApp PRIVATE
    ${imgui_fc_SOURCE_DIR}
    ${imgui_fc_SOURCE_DIR}/backends
    ${imnodes_fc_SOURCE_DIR}
    ${imgui_juce_fc_SOURCE_DIR}
    ${PIPER_DIR}
    ${ONNXRUNTIME_DIR}/include
    ${CMAKE_SOURCE_DIR}/../vendor/openvino_toolkit_windows_2025.3.0.19807.44526285f24_x86_64/samples/cpp/thirdparty/nlohmann_json/single_include
    ${CMAKE_SOURCE_DIR}/../soundtouch/include
    ${CMAKE_SOURCE_DIR}/../soundtouch/source/SoundTouch
    ${ufbx_SOURCE_DIR}
    # FFmpeg for audio streaming (Internet Radio, RTMP)
    $<$<BOOL:${FFMPEG_FOUND}>:${FFMPEG_INCLUDE_DIR}>
    # NO OPENCV
    $<$<BOOL:${USE_RUBBERBAND}>:${RUBBERBAND_INCLUDE_DIR}>
    # ASIO SDK headers (Windows only, conditional)
    $<$<BOOL:${ASIO_SDK_FOUND}>:${ASIO_SDK_DIR}/common>
    $<$<BOOL:${ASIO_SDK_FOUND}>:${ASIO_SDK_DIR}/host>
    # STK and Essentia headers
    $<$<BOOL:${STK_FOUND}>:${STK_SOURCE_DIR}/include>
    $<$<BOOL:${ESSENTIA_FOUND}>:${ESSENTIA_DIR}/src>
)

# Compile Definitions
target_compile_definitions(PikonRaditszAudioApp PRIVATE
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:PikonRaditszAudioApp,PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:PikonRaditszAudioApp,VERSION>"
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
    JUCE_PLUGINHOST_VST3=1
    IMGUI_IMPL_JUCE_BEZEL=0
    IMGUI_ENABLE_VIEWPORTS
    IMGUI_DEFINE_MATH_OPERATORS
    IMNODES_NAMESPACE=ImNodes
    IMNODES_STATIC_DEFINE
    PRESET_CREATOR_UI=1
    AUDIO_ONLY_BUILD=1  # CRITICAL: Disables Video/CV code paths
    # Prevent winsock.h/winsock2.h conflict on Windows
    $<$<PLATFORM_ID:Windows>:WIN32_LEAN_AND_MEAN>
    $<$<BOOL:${USE_RUBBERBAND}>:USE_RUBBERBAND=1>
    $<$<NOT:$<BOOL:${USE_RUBBERBAND}>>:USE_RUBBERBAND=0>
    # NO WITH_CUDA_SUPPORT
    # Enable ASIO if SDK is found
    $<$<BOOL:${ASIO_SDK_FOUND}>:JUCE_ASIO=1>
    # Enable STK if found
    $<$<BOOL:${STK_FOUND}>:STK_FOUND=1>
    # Enable Essentia if found
    $<$<BOOL:${ESSENTIA_FOUND}>:ESSENTIA_FOUND=1>
)

# Link Libraries (EXCLUDING OpenCV, but INCLUDING FFmpeg for audio streaming)
target_link_libraries(PikonRaditszAudioApp PRIVATE
    juce::juce_gui_extra
    juce::juce_opengl
    juce::juce_audio_devices
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_audio_formats
    juce::juce_dsp
    juce::juce_osc
    juce::juce_cryptography
    imgui_impl_juce
    soundtouch
    $<$<BOOL:${STK_FOUND}>:stk>
    $<$<BOOL:${ESSENTIA_FOUND}>:${ESSENTIA_LIBRARY}>
    box2d
    glm::glm
    tinygltf
    ufbx_static
    # NO OPENCV
    $<$<BOOL:${USE_RUBBERBAND}>:${RUBBERBAND_TARGET}>
    # FFmpeg for audio streaming (Internet Radio, RTMP)
    $<$<BOOL:${FFMPEG_FOUND}>:${AVFORMAT_LIBRARY}>
    $<$<BOOL:${FFMPEG_FOUND}>:${AVCODEC_LIBRARY}>
    $<$<BOOL:${FFMPEG_FOUND}>:${AVUTIL_LIBRARY}>
    $<$<BOOL:${FFMPEG_FOUND}>:${SWRESAMPLE_LIBRARY}>
    # Piper TTS libraries (pre-built)
    ${ONNXRUNTIME_DIR}/lib/onnxruntime.lib
)

# Link Windows libraries for native camera enumeration (DirectShow) - Still needed for audio device enumeration? No, but maybe for other things.
# Actually, ole32 is needed for COM. strmiids might be needed for some audio stuff too?
if(WIN32)
    target_link_libraries(PikonRaditszAudioApp PRIVATE ole32 strmiids)
endif()

# Link SRT if found (for SRT Receiver module)
if(SRT_FOUND)
    target_link_libraries(PikonRaditszAudioApp PRIVATE ${SRT_LIBRARIES})
    if(SRT_INCLUDE_DIRS)
        target_include_directories(PikonRaditszAudioApp PRIVATE ${SRT_INCLUDE_DIRS})
    endif()
    target_compile_definitions(PikonRaditszAudioApp PRIVATE HAS_SRT=1)
endif()

# Link WinRT for Bluetooth Audio and System Audio Capture (Windows only)
if(WIN32 AND CPPWINRT_AVAILABLE)
    target_link_libraries(PikonRaditszAudioApp PRIVATE WindowsApp.lib)
    target_compile_definitions(PikonRaditszAudioApp PRIVATE HAS_WINRT_AUDIO=1)
endif()

# Link librtlsdr if found
if(LIBRTLSDR_FOUND)
    target_link_libraries(PikonRaditszAudioApp PRIVATE ${LIBRTLSDR_LIBRARIES})
    if(LIBRTLSDR_INCLUDE_DIRS)
        target_include_directories(PikonRaditszAudioApp PRIVATE ${LIBRTLSDR_INCLUDE_DIRS})
    endif()
    target_compile_definitions(PikonRaditszAudioApp PRIVATE HAS_LIBRTLSDR=1)
endif()

if(MSVC)
    target_compile_options(PikonRaditszAudioApp PRIVATE "$<$<CONFIG:Debug>:/Z7>")
endif()

# Add compile definition to signal zoom mode for app code if enabled
if(PRESET_CREATOR_IMNODES_ZOOM)
    target_compile_definitions(PikonRaditszAudioApp PRIVATE IMNODES_ZOOM_ENABLED=1)
endif()

# Copy Assets for PikonRaditszAudioApp
    # Redirect output to PresetCreatorApp artifacts directory to share assets
    set_target_properties(PikonRaditszAudioApp PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/PresetCreatorApp_artefacts/$<CONFIG>"
    )

    # Copy audio-only splash screen
    add_custom_command(
        TARGET PikonRaditszAudioApp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:PikonRaditszAudioApp>/icons"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/../icons/splash clean-audio.png"
                "$<TARGET_FILE_DIR:PikonRaditszAudioApp>/icons/splash-audio.png"
        COMMENT "Copying audio-only splash screen to icons/splash-audio.png"
    )
    
    # ==============================================================================
    # Copy STK runtime assets for PikonRaditszAudioApp
    # ==============================================================================
    if(STK_FOUND)
        # Copy STK rawwaves directory (required for some STK instruments)
        if(EXISTS "${STK_SOURCE_DIR}/rawwaves")
            add_custom_command(
                TARGET PikonRaditszAudioApp POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E echo "Copying STK rawwaves from: ${STK_SOURCE_DIR}/rawwaves"
                COMMAND ${CMAKE_COMMAND} -E echo "Copying STK rawwaves to: $<TARGET_FILE_DIR:PikonRaditszAudioApp>/rawwaves"
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                        "${STK_SOURCE_DIR}/rawwaves"
                        "$<TARGET_FILE_DIR:PikonRaditszAudioApp>/rawwaves"
                COMMENT "Copying STK rawwaves to PikonRaditszAudioApp output directory"
            )
        endif()
    endif()
    
    # ==============================================================================
    # Copy Essentia runtime assets for PikonRaditszAudioApp
    # ==============================================================================
    if(ESSENTIA_FOUND)
        # Essentia may need models or data files at runtime
        # Check for common Essentia runtime directories
        if(EXISTS "${ESSENTIA_DIR}/models")
            add_custom_command(
                TARGET PikonRaditszAudioApp POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E echo "Copying Essentia models from: ${ESSENTIA_DIR}/models"
                COMMAND ${CMAKE_COMMAND} -E echo "Copying Essentia models to: $<TARGET_FILE_DIR:PikonRaditszAudioApp>/essentia/models"
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                        "${ESSENTIA_DIR}/models"
                        "$<TARGET_FILE_DIR:PikonRaditszAudioApp>/essentia/models"
                COMMENT "Copying Essentia models to PikonRaditszAudioApp output directory"
            )
        endif()
        
        # Copy Essentia data files if they exist
        if(EXISTS "${ESSENTIA_DIR}/data")
            add_custom_command(
                TARGET PikonRaditszAudioApp POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E echo "Copying Essentia data from: ${ESSENTIA_DIR}/data"
                COMMAND ${CMAKE_COMMAND} -E echo "Copying Essentia data to: $<TARGET_FILE_DIR:PikonRaditszAudioApp>/essentia/data"
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                        "${ESSENTIA_DIR}/data"
                        "$<TARGET_FILE_DIR:PikonRaditszAudioApp>/essentia/data"
                COMMENT "Copying Essentia data to PikonRaditszAudioApp output directory"
            )
        endif()
    endif()
    
    # ==============================================================================
    # Copy FFmpeg DLLs for PikonRaditszAudioApp (Audio streaming)
    # ==============================================================================
    if(FFMPEG_FOUND AND EXISTS "${FFMPEG_BIN_DIR}")
        file(GLOB FFMPEG_DLLS_AUDIO "${FFMPEG_BIN_DIR}/*.dll")
        if(FFMPEG_DLLS_AUDIO)
            foreach(DLL ${FFMPEG_DLLS_AUDIO})
                get_filename_component(DLL_NAME ${DLL} NAME)
                add_custom_command(
                    TARGET PikonRaditszAudioApp POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            "${DLL}"
                            "$<TARGET_FILE_DIR:PikonRaditszAudioApp>"
                    COMMENT "Copying ${DLL_NAME} to PikonRaditszAudioApp"
                )
            endforeach()
        endif()
    endif()
    
    # ==============================================================================
    # Copy SRT DLL for PikonRaditszAudioApp (if found)
    # ==============================================================================
    if(SRT_FOUND AND SRT_DLL)
        add_custom_command(
            TARGET PikonRaditszAudioApp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${SRT_DLL}"
                    "$<TARGET_FILE_DIR:PikonRaditszAudioApp>"
            COMMENT "Copying SRT DLL to PikonRaditszAudioApp"
        )
    endif()

