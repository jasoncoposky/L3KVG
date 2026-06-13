# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/home/darkfell/dev/l3kvg/build_rel/_deps/cppzmq-src"
  "/home/darkfell/dev/l3kvg/build_rel/_deps/cppzmq-build"
  "/home/darkfell/dev/l3kvg/build_rel/_deps/cppzmq-subbuild/cppzmq-populate-prefix"
  "/home/darkfell/dev/l3kvg/build_rel/_deps/cppzmq-subbuild/cppzmq-populate-prefix/tmp"
  "/home/darkfell/dev/l3kvg/build_rel/_deps/cppzmq-subbuild/cppzmq-populate-prefix/src/cppzmq-populate-stamp"
  "/home/darkfell/dev/l3kvg/build_rel/_deps/cppzmq-subbuild/cppzmq-populate-prefix/src"
  "/home/darkfell/dev/l3kvg/build_rel/_deps/cppzmq-subbuild/cppzmq-populate-prefix/src/cppzmq-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/home/darkfell/dev/l3kvg/build_rel/_deps/cppzmq-subbuild/cppzmq-populate-prefix/src/cppzmq-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/home/darkfell/dev/l3kvg/build_rel/_deps/cppzmq-subbuild/cppzmq-populate-prefix/src/cppzmq-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
