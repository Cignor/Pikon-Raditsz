# Install script for directory: H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/opencv-src/3rdparty/libjpeg-turbo

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
    set(CMAKE_INSTALL_CONFIG_NAME "Release")
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

if(CMAKE_INSTALL_COMPONENT STREQUAL "dev" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/x64/vc17/staticlib" TYPE STATIC_LIBRARY OPTIONAL FILES "H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/opencv-build/3rdparty/lib/libjpeg-turbo.lib")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "licenses" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/etc/licenses" TYPE FILE RENAME "libjpeg-turbo-README.md" FILES "H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/opencv-src/3rdparty/libjpeg-turbo/README.md")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "licenses" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/etc/licenses" TYPE FILE RENAME "libjpeg-turbo-LICENSE.md" FILES "H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/opencv-src/3rdparty/libjpeg-turbo/LICENSE.md")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "licenses" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/etc/licenses" TYPE FILE RENAME "libjpeg-turbo-README.ijg" FILES "H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/opencv-src/3rdparty/libjpeg-turbo/README.ijg")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for each subdirectory.
  include("H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/opencv-build/3rdparty/libjpeg-turbo/simd/cmake_install.cmake")

endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/opencv-build/3rdparty/libjpeg-turbo/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
