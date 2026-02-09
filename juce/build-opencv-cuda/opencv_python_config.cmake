
set(CMAKE_BUILD_TYPE "Release")

set(BUILD_SHARED_LIBS "OFF")

set(CMAKE_C_FLAGS " /DWIN32 /D_WINDOWS /FS  /D _CRT_SECURE_NO_DEPRECATE /D _CRT_NONSTDC_NO_DEPRECATE /D _SCL_SECURE_NO_WARNINGS /Gy /bigobj /Oi  /fp:precise /FS   ")

set(CMAKE_C_FLAGS_DEBUG "/Zi /Ob0 /Od /RTC1 /Z7 ")

set(CMAKE_C_FLAGS_RELEASE "  /O2 /Ob2 /DNDEBUG ")

set(CMAKE_CXX_FLAGS " /DWIN32 /D_WINDOWS /FS  /D _CRT_SECURE_NO_DEPRECATE /D _CRT_NONSTDC_NO_DEPRECATE /D _SCL_SECURE_NO_WARNINGS /Gy /bigobj /Oi  /fp:precise /FS    /EHa /wd4127 /wd4251 /wd4324 /wd4275 /wd4512 /wd4589 /wd4819")

set(CMAKE_CXX_FLAGS_DEBUG " /Zi /Ob0 /Od /RTC1 /Z7 ")

set(CMAKE_CXX_FLAGS_RELEASE " /O2 /Ob2 /DNDEBUG ")

set(CV_GCC "")

set(CV_CLANG "")

set(ENABLE_NOISY_WARNINGS "OFF")

set(CMAKE_MODULE_LINKER_FLAGS "/machine:x64 ")

set(CMAKE_INSTALL_PREFIX "H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/install")

set(OPENCV_PYTHON_INSTALL_PATH "")

set(OpenCV_SOURCE_DIR "H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/opencv-src")

set(OPENCV_FORCE_PYTHON_LIBS "")

set(OPENCV_PYTHON_SKIP_LINKER_EXCLUDE_LIBS "")

set(OPENCV_PYTHON_BINDINGS_DIR "H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/opencv-build/modules/python_bindings_generator")

set(cv2_custom_hdr "H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/opencv-build/modules/python_bindings_generator/pyopencv_custom_headers.h")

set(cv2_generated_files "H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/opencv-build/modules/python_bindings_generator/pyopencv_generated_enums.h;H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/opencv-build/modules/python_bindings_generator/pyopencv_generated_funcs.h;H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/opencv-build/modules/python_bindings_generator/pyopencv_generated_include.h;H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/opencv-build/modules/python_bindings_generator/pyopencv_generated_modules.h;H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/opencv-build/modules/python_bindings_generator/pyopencv_generated_modules_content.h;H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/opencv-build/modules/python_bindings_generator/pyopencv_generated_types.h;H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/opencv-build/modules/python_bindings_generator/pyopencv_generated_types_content.h;H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/opencv-build/modules/python_bindings_generator/pyopencv_signatures.json")
