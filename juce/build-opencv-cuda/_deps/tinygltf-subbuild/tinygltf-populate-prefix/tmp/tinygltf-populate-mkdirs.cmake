# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/tinygltf-src")
  file(MAKE_DIRECTORY "H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/tinygltf-src")
endif()
file(MAKE_DIRECTORY
  "H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/tinygltf-build"
  "H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/tinygltf-subbuild/tinygltf-populate-prefix"
  "H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/tinygltf-subbuild/tinygltf-populate-prefix/tmp"
  "H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/tinygltf-subbuild/tinygltf-populate-prefix/src/tinygltf-populate-stamp"
  "H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/tinygltf-subbuild/tinygltf-populate-prefix/src"
  "H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/tinygltf-subbuild/tinygltf-populate-prefix/src/tinygltf-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/tinygltf-subbuild/tinygltf-populate-prefix/src/tinygltf-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "H:/0000_CODE/01_collider_pyo/juce/build-opencv-cuda/_deps/tinygltf-subbuild/tinygltf-populate-prefix/src/tinygltf-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
