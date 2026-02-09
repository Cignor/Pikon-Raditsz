# Install script for directory: H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/juce-src

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/install")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "Debug")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/juce-build/tools/modules/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/juce-build/tools/extras/Build/cmake_install.cmake")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/JUCE-7.0.9" TYPE FILE FILES
    "H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/juce-build/tools/JUCEConfigVersion.cmake"
    "H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/juce-build/tools/JUCEConfig.cmake"
    "H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/juce-src/extras/Build/CMake/JUCECheckAtomic.cmake"
    "H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/juce-src/extras/Build/CMake/JUCEHelperTargets.cmake"
    "H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/juce-src/extras/Build/CMake/JUCEModuleSupport.cmake"
    "H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/juce-src/extras/Build/CMake/JUCEUtils.cmake"
    "H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/juce-src/extras/Build/CMake/JuceLV2Defines.h.in"
    "H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/juce-src/extras/Build/CMake/LaunchScreen.storyboard"
    "H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/juce-src/extras/Build/CMake/PIPAudioProcessor.cpp.in"
    "H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/juce-src/extras/Build/CMake/PIPAudioProcessorWithARA.cpp.in"
    "H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/juce-src/extras/Build/CMake/PIPComponent.cpp.in"
    "H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/juce-src/extras/Build/CMake/PIPConsole.cpp.in"
    "H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/juce-src/extras/Build/CMake/RecentFilesMenuTemplate.nib"
    "H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/juce-src/extras/Build/CMake/UnityPluginGUIScript.cs.in"
    "H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/juce-src/extras/Build/CMake/checkBundleSigning.cmake"
    "H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/juce-src/extras/Build/CMake/copyDir.cmake"
    "H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/juce-src/extras/Build/CMake/juce_runtime_arch_detection.cpp"
    "H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/juce-src/extras/Build/CMake/juce_LinuxSubprocessHelper.cpp"
    )
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/juce-build/tools/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
if(CMAKE_INSTALL_COMPONENT)
  if(CMAKE_INSTALL_COMPONENT MATCHES "^[a-zA-Z0-9_.+-]+$")
    set(CMAKE_INSTALL_MANIFEST "install_manifest_${CMAKE_INSTALL_COMPONENT}.txt")
  else()
    string(MD5 CMAKE_INST_COMP_HASH "${CMAKE_INSTALL_COMPONENT}")
    set(CMAKE_INSTALL_MANIFEST "install_manifest_${CMAKE_INST_COMP_HASH}.txt")
    unset(CMAKE_INST_COMP_HASH)
  endif()
else()
  set(CMAKE_INSTALL_MANIFEST "install_manifest.txt")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/juce-build/tools/${CMAKE_INSTALL_MANIFEST}"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
