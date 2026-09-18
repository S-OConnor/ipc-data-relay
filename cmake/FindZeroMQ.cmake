# FindZeroMQ.cmake
#
# Locates libzmq. Prefers pkg-config (which is what Yocto's cmake class
# provides), then falls back to a plain header/library search, including the
# Homebrew prefix for developer machines.
#
# Defines the imported target ZeroMQ::ZeroMQ and the variables
#   ZeroMQ_FOUND, ZeroMQ_INCLUDE_DIRS, ZeroMQ_LIBRARIES, ZeroMQ_VERSION

if(TARGET ZeroMQ::ZeroMQ)
  set(ZeroMQ_FOUND TRUE)
  return()
endif()

find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(PC_ZMQ QUIET libzmq)
endif()

set(_zmq_hints "")
if(DEFINED ENV{HOMEBREW_PREFIX})
  list(APPEND _zmq_hints "$ENV{HOMEBREW_PREFIX}")
endif()
list(APPEND _zmq_hints "/home/linuxbrew/.linuxbrew" "/opt/homebrew" "/usr/local")

find_path(ZeroMQ_INCLUDE_DIR
  NAMES zmq.h
  HINTS ${PC_ZMQ_INCLUDE_DIRS}
  PATHS ${_zmq_hints}
  PATH_SUFFIXES include)

find_library(ZeroMQ_LIBRARY
  NAMES zmq libzmq
  HINTS ${PC_ZMQ_LIBRARY_DIRS}
  PATHS ${_zmq_hints}
  PATH_SUFFIXES lib lib64)

if(PC_ZMQ_VERSION)
  set(ZeroMQ_VERSION "${PC_ZMQ_VERSION}")
elseif(ZeroMQ_INCLUDE_DIR AND EXISTS "${ZeroMQ_INCLUDE_DIR}/zmq.h")
  file(STRINGS "${ZeroMQ_INCLUDE_DIR}/zmq.h" _zmq_ver_lines
    REGEX "#define ZMQ_VERSION_(MAJOR|MINOR|PATCH) [0-9]+")
  string(REGEX REPLACE ".*MAJOR ([0-9]+).*" "\\1" _zmq_major "${_zmq_ver_lines}")
  string(REGEX REPLACE ".*MINOR ([0-9]+).*" "\\1" _zmq_minor "${_zmq_ver_lines}")
  string(REGEX REPLACE ".*PATCH ([0-9]+).*" "\\1" _zmq_patch "${_zmq_ver_lines}")
  set(ZeroMQ_VERSION "${_zmq_major}.${_zmq_minor}.${_zmq_patch}")
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(ZeroMQ
  REQUIRED_VARS ZeroMQ_LIBRARY ZeroMQ_INCLUDE_DIR
  VERSION_VAR ZeroMQ_VERSION)

if(ZeroMQ_FOUND)
  set(ZeroMQ_INCLUDE_DIRS "${ZeroMQ_INCLUDE_DIR}")
  set(ZeroMQ_LIBRARIES "${ZeroMQ_LIBRARY}")
  add_library(ZeroMQ::ZeroMQ UNKNOWN IMPORTED)
  set_target_properties(ZeroMQ::ZeroMQ PROPERTIES
    IMPORTED_LOCATION "${ZeroMQ_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${ZeroMQ_INCLUDE_DIR}")
endif()

mark_as_advanced(ZeroMQ_INCLUDE_DIR ZeroMQ_LIBRARY)
