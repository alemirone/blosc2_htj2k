# Blosc - Blocked Shuffling and Compression Library
#
# Copyright (C) 2023  The Blosc Developers <blosc@blosc.org>
# https://blosc.org
# License: GNU Affero General Public License v3.0 (see LICENSE.txt)

cmake_minimum_required(VERSION 3.20)
project(blosc2_htj2k LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)

# Grok needs MacOS 11.0 or later
set(CMAKE_OSX_DEPLOYMENT_TARGET 11.0)

# Debug build
# set(CMAKE_C_FLAGS_DEBUG_INIT "-O0 -g")
# set(CMAKE_CXX_FLAGS_DEBUG_INIT "-O0 -g")

include(GNUInstallDirs)

message("Building Blosc2 plugin...")

# Necessary when calling via cmake (to know which python to call)
find_package(Python REQUIRED COMPONENTS Interpreter) 

# Propagate CMAKE_OSX_ARCHITECTURES env variable into CMAKE_SYSTEM_PROCESSOR
if(DEFINED ENV{CMAKE_OSX_ARCHITECTURES})
    if("$ENV{CMAKE_OSX_ARCHITECTURES}" STREQUAL "arm64")
        set(CMAKE_SYSTEM_PROCESSOR arm64)
    endif()
endif()

# ============================================================
# Step 1: Find C-Blosc2
# ============================================================
# Find blosc2 package location using Python
execute_process(
    COMMAND "${Python_EXECUTABLE}" -c "import blosc2, pathlib; print(pathlib.Path(blosc2.__file__).parent)"
    OUTPUT_VARIABLE BLOSC2_PACKAGE_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE BLOSC2_IMPORT_RESULT
)

# To compile blosc2_htj2k, we need the include files and the actual lib.
# Prefer the PEP 427-compliant python-blosc2 wheel layout when available, but
# also support python-blosc2 builds linked against a separately installed
# c-blosc2.  The bundled python-blosc2 path is preferred while released wheels
# lag the official c-blosc2 registry entries for J2K/HTJ2K.
set(BLOSC2_INCLUDE_DIR "")
set(BLOSC2_LIBRARIES "")
set(BLOSC2_LIBRARY_DIR_RESOLVED "")

if(BLOSC2_IMPORT_RESULT EQUAL 0 AND EXISTS "${BLOSC2_PACKAGE_DIR}")
    message(STATUS "Found blosc2 package at: ${BLOSC2_PACKAGE_DIR}")

    # Try PEP 427 compliant structure (blosc2/include/, blosc2/lib/) first.
    set(BLOSC2_INCLUDE_DIR "${BLOSC2_PACKAGE_DIR}/include")
    if(NOT EXISTS "${BLOSC2_INCLUDE_DIR}/blosc2.h")
        message(STATUS "blosc2 package does not ship blosc2.h at ${BLOSC2_INCLUDE_DIR}; trying system c-blosc2")
        set(BLOSC2_INCLUDE_DIR "")
    else()
        message(STATUS "Found Blosc2 include: ${BLOSC2_INCLUDE_DIR}/blosc2.h")

        set(BLOSC2_LIB_DIR "${BLOSC2_PACKAGE_DIR}/lib")
        if(EXISTS "${BLOSC2_LIB_DIR}")
            find_library(BLOSC2_LIBRARY
                NAMES blosc2 libblosc2
                PATHS "${BLOSC2_LIB_DIR}"
                NO_DEFAULT_PATH
            )
            if(BLOSC2_LIBRARY)
                message(STATUS "Found Blosc2 library: ${BLOSC2_LIBRARY}")
                set(BLOSC2_LIBRARIES "${BLOSC2_LIBRARY}")
                get_filename_component(BLOSC2_LIBRARY_DIR_RESOLVED "${BLOSC2_LIBRARY}" DIRECTORY)
            endif()
        endif()
    endif()
else()
    message(STATUS "Could not import blosc2 package; trying system c-blosc2")
endif()

if(NOT BLOSC2_INCLUDE_DIR OR NOT BLOSC2_LIBRARIES)
    find_package(Blosc2 CONFIG QUIET)
    if(Blosc2_FOUND AND TARGET Blosc2::blosc2_shared)
        get_target_property(_blosc2_include Blosc2::blosc2 INTERFACE_INCLUDE_DIRECTORIES)
        if(NOT _blosc2_include)
            get_target_property(_blosc2_include Blosc2::blosc2_shared INTERFACE_INCLUDE_DIRECTORIES)
        endif()
        set(BLOSC2_INCLUDE_DIR ${_blosc2_include})
        set(BLOSC2_LIBRARIES Blosc2::blosc2_shared)
        get_target_property(_blosc2_location Blosc2::blosc2_shared IMPORTED_LOCATION_RELEASE)
        if(NOT _blosc2_location)
            get_target_property(_blosc2_location Blosc2::blosc2_shared IMPORTED_LOCATION)
        endif()
        if(_blosc2_location)
            get_filename_component(BLOSC2_LIBRARY_DIR_RESOLVED "${_blosc2_location}" DIRECTORY)
        endif()
        message(STATUS "Found system Blosc2 via CMake package")
    else()
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(BLOSC2_PC QUIET IMPORTED_TARGET blosc2)
            if(BLOSC2_PC_FOUND)
                set(BLOSC2_INCLUDE_DIR ${BLOSC2_PC_INCLUDE_DIRS})
                set(BLOSC2_LIBRARIES PkgConfig::BLOSC2_PC)
                list(GET BLOSC2_PC_LIBRARY_DIRS 0 BLOSC2_LIBRARY_DIR_RESOLVED)
                message(STATUS "Found system Blosc2 via pkg-config")
            endif()
        endif()
    endif()
endif()

if(NOT BLOSC2_INCLUDE_DIR OR NOT BLOSC2_LIBRARIES)
    message(FATAL_ERROR "Could not find Blosc2 headers/libraries. Install python-blosc2 with bundled libraries, or install c-blosc2 and set CMAKE_PREFIX_PATH/PKG_CONFIG_PATH.")
endif()

include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_INCLUDES "${BLOSC2_INCLUDE_DIR}")
set(_BLOSC2_PREVIOUS_TRY_COMPILE_TARGET_TYPE "${CMAKE_TRY_COMPILE_TARGET_TYPE}")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
check_cxx_source_compiles("
#include <stdint.h>
#include \"blosc2.h\"
#include \"b2nd.h\"
int main(void) {
  uint8_t meta[1] = {0};
  int8_t ndim = 0;
  int64_t shape[B2ND_MAX_DIM] = {0};
  int32_t chunkshape[B2ND_MAX_DIM] = {0};
  int32_t blockshape[B2ND_MAX_DIM] = {0};
  return b2nd_deserialize_meta_inline(meta, 1, &ndim, shape, chunkshape, blockshape, 0, 0);
}
" BLOSC2_HAS_B2ND_DESERIALIZE_META_INLINE)
set(CMAKE_TRY_COMPILE_TARGET_TYPE "${_BLOSC2_PREVIOUS_TRY_COMPILE_TARGET_TYPE}")
unset(_BLOSC2_PREVIOUS_TRY_COMPILE_TARGET_TYPE)
unset(CMAKE_REQUIRED_INCLUDES)
if(NOT BLOSC2_HAS_B2ND_DESERIALIZE_META_INLINE)
    message(FATAL_ERROR "The Blosc2 headers found at ${BLOSC2_INCLUDE_DIR} do not provide b2nd_deserialize_meta_inline(). Please use c-blosc2 >= 3.1.0 / a python-blosc2 build that bundles it.")
endif()

# ============================================================
# blosc2_htj2k plugin source files
# ============================================================
add_subdirectory(src)
