# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/home/darkfell/dev/l3kvg/build_rwdi/_deps/xxhash-src"
  "/home/darkfell/dev/l3kvg/build_rwdi/_deps/xxhash-build"
  "/home/darkfell/dev/l3kvg/build_rwdi/_deps/xxhash-subbuild/xxhash-populate-prefix"
  "/home/darkfell/dev/l3kvg/build_rwdi/_deps/xxhash-subbuild/xxhash-populate-prefix/tmp"
  "/home/darkfell/dev/l3kvg/build_rwdi/_deps/xxhash-subbuild/xxhash-populate-prefix/src/xxhash-populate-stamp"
  "/home/darkfell/dev/l3kvg/build_rwdi/_deps/xxhash-subbuild/xxhash-populate-prefix/src"
  "/home/darkfell/dev/l3kvg/build_rwdi/_deps/xxhash-subbuild/xxhash-populate-prefix/src/xxhash-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/home/darkfell/dev/l3kvg/build_rwdi/_deps/xxhash-subbuild/xxhash-populate-prefix/src/xxhash-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/home/darkfell/dev/l3kvg/build_rwdi/_deps/xxhash-subbuild/xxhash-populate-prefix/src/xxhash-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
