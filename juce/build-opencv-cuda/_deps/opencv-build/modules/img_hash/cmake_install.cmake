# Install script for directory: H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/opencv_contrib-src/modules/img_hash

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
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/x64/vc17/staticlib" TYPE STATIC_LIBRARY OPTIONAL FILES "H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/opencv-build/lib/opencv_img_hash4140.lib")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "dev" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/opencv2" TYPE FILE OPTIONAL FILES "H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/opencv_contrib-src/modules/img_hash/include/opencv2/img_hash.hpp")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "dev" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/opencv2/img_hash" TYPE FILE OPTIONAL FILES "H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/opencv_contrib-src/modules/img_hash/include/opencv2/img_hash/average_hash.hpp")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "dev" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/opencv2/img_hash" TYPE FILE OPTIONAL FILES "H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/opencv_contrib-src/modules/img_hash/include/opencv2/img_hash/block_mean_hash.hpp")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "dev" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/opencv2/img_hash" TYPE FILE OPTIONAL FILES "H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/opencv_contrib-src/modules/img_hash/include/opencv2/img_hash/color_moment_hash.hpp")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "dev" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/opencv2/img_hash" TYPE FILE OPTIONAL FILES "H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/opencv_contrib-src/modules/img_hash/include/opencv2/img_hash/img_hash_base.hpp")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "dev" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/opencv2/img_hash" TYPE FILE OPTIONAL FILES "H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/opencv_contrib-src/modules/img_hash/include/opencv2/img_hash/marr_hildreth_hash.hpp")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "dev" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/opencv2/img_hash" TYPE FILE OPTIONAL FILES "H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/opencv_contrib-src/modules/img_hash/include/opencv2/img_hash/phash.hpp")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "dev" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/opencv2/img_hash" TYPE FILE OPTIONAL FILES "H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/opencv_contrib-src/modules/img_hash/include/opencv2/img_hash/radial_variance_hash.hpp")
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/opencv-build/modules/img_hash/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
