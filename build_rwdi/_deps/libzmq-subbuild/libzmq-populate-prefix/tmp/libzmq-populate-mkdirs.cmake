# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/home/darkfell/dev/l3kvg/build_rwdi/_deps/libzmq-src"
  "/home/darkfell/dev/l3kvg/build_rwdi/_deps/libzmq-build"
  "/home/darkfell/dev/l3kvg/build_rwdi/_deps/libzmq-subbuild/libzmq-populate-prefix"
  "/home/darkfell/dev/l3kvg/build_rwdi/_deps/libzmq-subbuild/libzmq-populate-prefix/tmp"
  "/home/darkfell/dev/l3kvg/build_rwdi/_deps/libzmq-subbuild/libzmq-populate-prefix/src/libzmq-populate-stamp"
  "/home/darkfell/dev/l3kvg/build_rwdi/_deps/libzmq-subbuild/libzmq-populate-prefix/src"
  "/home/darkfell/dev/l3kvg/build_rwdi/_deps/libzmq-subbuild/libzmq-populate-prefix/src/libzmq-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/home/darkfell/dev/l3kvg/build_rwdi/_deps/libzmq-subbuild/libzmq-populate-prefix/src/libzmq-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/home/darkfell/dev/l3kvg/build_rwdi/_deps/libzmq-subbuild/libzmq-populate-prefix/src/libzmq-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
