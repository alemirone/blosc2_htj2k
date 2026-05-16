##############################################################################
# blosc2_htj2k: Grok (JPEG2000 codec) plugin for Blosc2
#
# Copyright (c) 2023  The Blosc Development Team <blosc@blosc.org>
# https://blosc.org
# License: GNU Affero General Public License v3.0 (see LICENSE.txt)
##############################################################################

set(GRK_BUILD_LIBPNG OFF)
set(GRK_BUILD_JPEG OFF)
set(GRK_BUILD_CORE_EXAMPLES OFF)

# Avoid adding unneeded files to the wheel
set(GRK_BUILD_PKGCONFIG_FILES OFF)
set(GRK_BUILD_CODEC OFF)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# Build statically
set(BUILD_SHARED_LIBS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if (APPLE)
    # Apple Rosetta cannot emulate AVX due to Intel patents
    set(ENABLE_AVX2 OFF)
endif()
set(CMAKE_FIND_FRAMEWORK NEVER)
if(WIN32)
    add_compile_definitions(NOMINMAX)
endif()

# Grok's static-library bundling step writes to ${CMAKE_BINARY_DIR}/bin.
# Create it explicitly so isolated wheel/editable builds do not depend on
# another target creating that directory first.
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

add_subdirectory(grok)
 # explicitly include headers as not exported by grok
target_include_directories(grokj2k
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/grok/src/lib/core  # source headers
        ${CMAKE_CURRENT_BINARY_DIR}/grok/src/lib/core  # generated headers
)
# Build libblosc2_htj2k as a shared library on every platform.  Replacement
# backends link to the native Grok entry points, so the main target must be
# linkable and not only a Python extension module.
set(BLOSC2_HTJ2K_CORE_SOURCES
    b2nd_layout.cpp
    blosc2_htj2k.cpp
    codec_requests.cpp
    codestream_detector.cpp
    jpeg2000_codec_paths.cpp
    plugin_loader.cpp
    runtime_config.cpp
)
add_library(blosc2_htj2k SHARED ${BLOSC2_HTJ2K_CORE_SOURCES})

if (MSVC OR MINGW)
    # Necessary for compiling blosc2_htj2k.lib
    set_property(TARGET blosc2_htj2k PROPERTY WINDOWS_EXPORT_ALL_SYMBOLS TRUE)
endif()

# The plugin API header is private to this package but needed by the core
# loader and by the replacement backends built below.
target_include_directories(blosc2_htj2k PUBLIC ${BLOSC2_INCLUDE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/plugin)
target_link_libraries(blosc2_htj2k PRIVATE grokj2k)
if(APPLE OR WIN32)
    # Keep the original Linux wheel behavior: libblosc2 is provided by the
    # host process/package and is not a direct dependency of this shared
    # library.  Linking it on Linux makes auditwheel try to vendor
    # libblosc2.so.8 from the python-blosc2 wheel and breaks wheel repair.
    target_link_libraries(blosc2_htj2k PRIVATE ${BLOSC2_LIBRARIES})
endif()
if (UNIX AND NOT APPLE)
    target_link_libraries(blosc2_htj2k PRIVATE dl)
endif()

# Add runtime path just in case (e.g. load plugin before blosc2 for example)
# will not always be used, but if can't find blosc2 gives another thing to check
# before failing
if(UNIX AND NOT APPLE)
    set_target_properties(blosc2_htj2k PROPERTIES
        INSTALL_RPATH "$ORIGIN/../blosc2/lib"
    )
    # C-Blosc2's HDF5 filter can load external codecs with dlopen()/dlclose()
    # without the Python module keeping a reference to this shared library.  The
    # codec owns process-global state and lazily loaded backend handles, so it
    # must remain resident for the lifetime of the process when used through
    # HDF5's dynamic plugin path.
    target_link_options(blosc2_htj2k PRIVATE "-Wl,-z,nodelete")
elseif(APPLE)
    set_target_properties(blosc2_htj2k PROPERTIES
        INSTALL_RPATH "@loader_path/../blosc2/lib"
    )
endif()

# Install
install(TARGETS blosc2_htj2k
    RUNTIME DESTINATION blosc2_htj2k
    LIBRARY DESTINATION blosc2_htj2k
)
install(FILES "${CMAKE_SOURCE_DIR}/blosc2_htj2k/blosc2_htj2k_plugins.json"
    DESTINATION blosc2_htj2k
)

add_library(blosc2_jpeg2000_bootstrap SHARED jpeg2000_bootstrap.c)
target_include_directories(blosc2_jpeg2000_bootstrap PRIVATE ${BLOSC2_INCLUDE_DIR})
target_link_libraries(blosc2_jpeg2000_bootstrap PRIVATE ${BLOSC2_LIBRARIES})
if(UNIX AND NOT APPLE)
    target_link_libraries(blosc2_jpeg2000_bootstrap PRIVATE dl)
    set_target_properties(blosc2_jpeg2000_bootstrap PROPERTIES
        INSTALL_RPATH "$ORIGIN/../blosc2/lib"
    )
elseif(APPLE)
    set_target_properties(blosc2_jpeg2000_bootstrap PROPERTIES
        INSTALL_RPATH "@loader_path/../blosc2/lib"
    )
endif()
if (MSVC OR MINGW)
    set_property(TARGET blosc2_jpeg2000_bootstrap PROPERTY WINDOWS_EXPORT_ALL_SYMBOLS TRUE)
endif()
install(TARGETS blosc2_jpeg2000_bootstrap
    RUNTIME DESTINATION blosc2_htj2k
    LIBRARY DESTINATION blosc2_htj2k
)
# Note: blosc2_htj2k.h and grok headers are not installed in the wheel
# Python users don't need them, and C developers should build from source

# Optional Kakadu backend.  The core codec does not link to Kakadu; this target
# is built only when the Kakadu headers and libraries are explicitly available.
set(KAKADU_ROOT "" CACHE PATH "Kakadu installation root")
set(KAKADU_LIBRARY_DIR "" CACHE PATH "Kakadu library directory")
set(KAKADU_INCLUDE_HINTS "")
set(KAKADU_LIBRARY_HINTS "")

if(NOT KAKADU_ROOT AND DEFINED ENV{KAKADU_ROOT})
    set(KAKADU_ROOT "$ENV{KAKADU_ROOT}")
elseif(NOT KAKADU_ROOT AND EXISTS "${CMAKE_SOURCE_DIR}/../kakadu_install")
    set(KAKADU_ROOT "${CMAKE_SOURCE_DIR}/../kakadu_install")
endif()
if(DEFINED ENV{KAKADU_INCLUDE_DIR})
    list(APPEND KAKADU_INCLUDE_HINTS "$ENV{KAKADU_INCLUDE_DIR}")
endif()
if(NOT KAKADU_LIBRARY_DIR AND DEFINED ENV{KAKADU_LIBRARY_DIR})
    set(KAKADU_LIBRARY_DIR "$ENV{KAKADU_LIBRARY_DIR}")
elseif(NOT KAKADU_LIBRARY_DIR AND DEFINED ENV{KAKADU_LIB_PATH})
    set(KAKADU_LIBRARY_DIR "$ENV{KAKADU_LIB_PATH}")
endif()
if(KAKADU_LIBRARY_DIR)
    list(APPEND KAKADU_LIBRARY_HINTS "${KAKADU_LIBRARY_DIR}")
endif()

find_path(KAKADU_INCLUDE_DIR
    NAMES kdu_stripe_compressor.h
    HINTS ${KAKADU_INCLUDE_HINTS} ${KAKADU_ROOT}
    PATH_SUFFIXES managed/all_includes all_includes include
)
find_library(KAKADU_CORE_LIBRARY
    NAMES kdu_v86R kdu_v85R kdu_v84R kdu_v83R kdu_v82R kdu_v81R kdu_v80R kdu
    HINTS ${KAKADU_LIBRARY_HINTS} ${KAKADU_ROOT}
    PATH_SUFFIXES lib lib/Linux-x86-64-gcc apps/make
)
find_library(KAKADU_AUX_LIBRARY
    NAMES kdu_a86R kdu_a85R kdu_a84R kdu_a83R kdu_a82R kdu_a81R kdu_a80R kdu_aux
    HINTS ${KAKADU_LIBRARY_HINTS} ${KAKADU_ROOT}
    PATH_SUFFIXES lib lib/Linux-x86-64-gcc apps/make
)

if(KAKADU_INCLUDE_DIR AND KAKADU_CORE_LIBRARY AND KAKADU_AUX_LIBRARY)
    add_library(blosc2_kakadu_htj2k_backend SHARED b2nd_layout.cpp plugin/kakadu_backend.cpp plugin/htj2k_kakadu_plugin.cpp)
    target_include_directories(blosc2_kakadu_htj2k_backend PRIVATE ${KAKADU_INCLUDE_DIR} ${BLOSC2_INCLUDE_DIR} ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/plugin)
    target_link_libraries(blosc2_kakadu_htj2k_backend PRIVATE ${KAKADU_CORE_LIBRARY} ${KAKADU_AUX_LIBRARY})

    if(UNIX AND NOT APPLE)
        set_target_properties(blosc2_kakadu_htj2k_backend PROPERTIES
            INSTALL_RPATH "$ORIGIN/../../../../blosc2/lib"
        )
    elseif(APPLE)
        set_target_properties(blosc2_kakadu_htj2k_backend PROPERTIES
            INSTALL_RPATH "@loader_path/../../../../blosc2/lib"
        )
    endif()

    if (MSVC OR MINGW)
        set_property(TARGET blosc2_kakadu_htj2k_backend PROPERTY WINDOWS_EXPORT_ALL_SYMBOLS TRUE)
    endif()

    install(TARGETS blosc2_kakadu_htj2k_backend
        RUNTIME DESTINATION blosc2_htj2k/plugins/htj2k/kakadu
        LIBRARY DESTINATION blosc2_htj2k/plugins/htj2k/kakadu
    )
    message(STATUS "Kakadu HTJ2K backend enabled")
else()
    message(STATUS "Kakadu HTJ2K backend disabled (Kakadu headers/libraries not found)")
endif()

# Optional OpenHTJ2K backend.  It is built only when an OpenHTJ2K installation
# with the public C++ encoder/decoder interface is available.
set(OPENHTJ2K_ROOT "" CACHE PATH "OpenHTJ2K installation root")
set(OPENHTJ2K_LIBRARY_DIR "" CACHE PATH "OpenHTJ2K library directory")
option(BLOSC2_HTJ2K_BUILD_OPENHTJ2K "Download and build OpenHTJ2K PR190 when no OpenHTJ2K installation is found" ON)
set(BLOSC2_HTJ2K_OPENHTJ2K_REPOSITORY "https://github.com/osamu620/OpenHTJ2K.git"
    CACHE STRING "OpenHTJ2K repository used by the optional in-tree build")
set(BLOSC2_HTJ2K_OPENHTJ2K_GIT_TAG "refs/pull/190/head"
    CACHE STRING "OpenHTJ2K git ref used by the optional in-tree build")
set(BLOSC2_HTJ2K_OPENHTJ2K_GIT_COMMIT "014011530be3d6217b0f8814a9ab0ca25583bd64"
    CACHE STRING "Expected OpenHTJ2K commit used by the optional in-tree build")
set(BLOSC2_HTJ2K_OPENHTJ2K_SOURCE_DIR "" CACHE PATH
    "Existing OpenHTJ2K source tree used by the optional in-tree build")
set(BLOSC2_HTJ2K_OPENHTJ2K_INSTALL_DIR "" CACHE PATH
    "Install prefix for the optional in-tree OpenHTJ2K build")
set(OPENHTJ2K_INCLUDE_HINTS "")
set(OPENHTJ2K_LIBRARY_HINTS "")

if(NOT OPENHTJ2K_ROOT AND DEFINED ENV{OPENHTJ2K_ROOT})
    set(OPENHTJ2K_ROOT "$ENV{OPENHTJ2K_ROOT}")
endif()
if(NOT BLOSC2_HTJ2K_OPENHTJ2K_SOURCE_DIR AND DEFINED ENV{BLOSC2_HTJ2K_OPENHTJ2K_SOURCE_DIR})
    set(BLOSC2_HTJ2K_OPENHTJ2K_SOURCE_DIR "$ENV{BLOSC2_HTJ2K_OPENHTJ2K_SOURCE_DIR}")
elseif(NOT BLOSC2_HTJ2K_OPENHTJ2K_SOURCE_DIR AND DEFINED ENV{OPENHTJ2K_SOURCE_DIR})
    set(BLOSC2_HTJ2K_OPENHTJ2K_SOURCE_DIR "$ENV{OPENHTJ2K_SOURCE_DIR}")
elseif(NOT BLOSC2_HTJ2K_OPENHTJ2K_SOURCE_DIR AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/openhtj2k/CMakeLists.txt")
    set(BLOSC2_HTJ2K_OPENHTJ2K_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/openhtj2k")
elseif(NOT BLOSC2_HTJ2K_OPENHTJ2K_SOURCE_DIR AND EXISTS "${CMAKE_SOURCE_DIR}/../OpenHTJ2K_PR190/CMakeLists.txt")
    set(BLOSC2_HTJ2K_OPENHTJ2K_SOURCE_DIR "${CMAKE_SOURCE_DIR}/../OpenHTJ2K_PR190")
endif()
if(DEFINED ENV{OPENHTJ2K_INCLUDE_DIR})
    list(APPEND OPENHTJ2K_INCLUDE_HINTS "$ENV{OPENHTJ2K_INCLUDE_DIR}")
endif()
if(NOT OPENHTJ2K_LIBRARY_DIR AND DEFINED ENV{OPENHTJ2K_LIBRARY_DIR})
    set(OPENHTJ2K_LIBRARY_DIR "$ENV{OPENHTJ2K_LIBRARY_DIR}")
elseif(NOT OPENHTJ2K_LIBRARY_DIR AND DEFINED ENV{OPENHTJ2K_LIB_PATH})
    set(OPENHTJ2K_LIBRARY_DIR "$ENV{OPENHTJ2K_LIB_PATH}")
endif()
if(OPENHTJ2K_LIBRARY_DIR)
    list(APPEND OPENHTJ2K_LIBRARY_HINTS "${OPENHTJ2K_LIBRARY_DIR}")
endif()

find_path(OPENHTJ2K_INCLUDE_DIR
    NAMES encoder.hpp decoder.hpp
    HINTS ${OPENHTJ2K_INCLUDE_HINTS} ${OPENHTJ2K_ROOT}
    PATH_SUFFIXES include/open_htj2k/interface open_htj2k/interface interface include
)
find_library(OPENHTJ2K_LIBRARY
    NAMES open_htj2k_R open_htj2k open_htj2k_d
    HINTS ${OPENHTJ2K_LIBRARY_HINTS} ${OPENHTJ2K_ROOT}
    PATH_SUFFIXES lib lib64
)

set(OPENHTJ2K_BUILT_BY_PROJECT OFF)
if(BLOSC2_HTJ2K_BUILD_OPENHTJ2K AND (NOT OPENHTJ2K_INCLUDE_DIR OR NOT OPENHTJ2K_LIBRARY))
    include(ExternalProject)

    if(NOT BLOSC2_HTJ2K_OPENHTJ2K_INSTALL_DIR)
        set(BLOSC2_HTJ2K_OPENHTJ2K_INSTALL_DIR
            "${CMAKE_BINARY_DIR}/openhtj2k_pr190_install")
    endif()

    set(OPENHTJ2K_EXTERNAL_SOURCE_ARGS "")
    if(BLOSC2_HTJ2K_OPENHTJ2K_SOURCE_DIR)
        set(OPENHTJ2K_EXTERNAL_SOURCE_ARGS
            SOURCE_DIR "${BLOSC2_HTJ2K_OPENHTJ2K_SOURCE_DIR}"
            DOWNLOAD_COMMAND ""
            UPDATE_COMMAND ""
        )
        message(STATUS "OpenHTJ2K will be built from local source: ${BLOSC2_HTJ2K_OPENHTJ2K_SOURCE_DIR}")
    elseif(BLOSC2_HTJ2K_OPENHTJ2K_GIT_TAG MATCHES "^refs/")
        find_package(Git REQUIRED)
        set(OPENHTJ2K_EXTERNAL_SOURCE_ARGS
            DOWNLOAD_COMMAND
                ${CMAKE_COMMAND} -E rm -rf <SOURCE_DIR>
                COMMAND ${GIT_EXECUTABLE} clone --no-checkout
                    "${BLOSC2_HTJ2K_OPENHTJ2K_REPOSITORY}" <SOURCE_DIR>
                COMMAND ${GIT_EXECUTABLE} -C <SOURCE_DIR> fetch --depth 1 origin
                    "${BLOSC2_HTJ2K_OPENHTJ2K_GIT_TAG}"
                COMMAND ${GIT_EXECUTABLE} -C <SOURCE_DIR> checkout --detach
                    "${BLOSC2_HTJ2K_OPENHTJ2K_GIT_COMMIT}"
        )
    else()
        set(OPENHTJ2K_EXTERNAL_SOURCE_ARGS
            GIT_REPOSITORY "${BLOSC2_HTJ2K_OPENHTJ2K_REPOSITORY}"
            GIT_TAG "${BLOSC2_HTJ2K_OPENHTJ2K_GIT_TAG}"
            GIT_SHALLOW TRUE
        )
    endif()

    ExternalProject_Add(openhtj2k_pr190_external
        ${OPENHTJ2K_EXTERNAL_SOURCE_ARGS}
        CMAKE_ARGS
            -DCMAKE_BUILD_TYPE=Release
            -DCMAKE_INSTALL_PREFIX=${BLOSC2_HTJ2K_OPENHTJ2K_INSTALL_DIR}
            -DBUILD_SHARED_LIBS=ON
        BUILD_BYPRODUCTS
            "${BLOSC2_HTJ2K_OPENHTJ2K_INSTALL_DIR}/lib/${CMAKE_SHARED_LIBRARY_PREFIX}open_htj2k_R${CMAKE_SHARED_LIBRARY_SUFFIX}"
    )

    set(OPENHTJ2K_ROOT "${BLOSC2_HTJ2K_OPENHTJ2K_INSTALL_DIR}")
    set(OPENHTJ2K_INCLUDE_DIR "${OPENHTJ2K_ROOT}/include/open_htj2k/interface")
    set(OPENHTJ2K_LIBRARY_DIR "${OPENHTJ2K_ROOT}/lib")
    set(OPENHTJ2K_LIBRARY
        "${OPENHTJ2K_LIBRARY_DIR}/${CMAKE_SHARED_LIBRARY_PREFIX}open_htj2k_R${CMAKE_SHARED_LIBRARY_SUFFIX}")
    set(OPENHTJ2K_BUILT_BY_PROJECT ON)
    if(NOT BLOSC2_HTJ2K_OPENHTJ2K_SOURCE_DIR)
        message(STATUS "OpenHTJ2K will be downloaded and built from ${BLOSC2_HTJ2K_OPENHTJ2K_REPOSITORY} (${BLOSC2_HTJ2K_OPENHTJ2K_GIT_TAG})")
    endif()
endif()

unset(OPENHTJ2K_HAS_PR190_UINT16_API)
unset(OPENHTJ2K_HAS_PR190_UINT16_API CACHE)
if(OPENHTJ2K_BUILT_BY_PROJECT)
    # The downloaded source is pinned to the PR190-style API.  The headers and
    # library are produced during the build step, so a configure-time
    # compile/link probe cannot run yet.
    set(OPENHTJ2K_HAS_PR190_UINT16_API TRUE)
elseif(OPENHTJ2K_INCLUDE_DIR AND OPENHTJ2K_LIBRARY)
    include(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_INCLUDES_SAVE "${CMAKE_REQUIRED_INCLUDES}")
    set(CMAKE_REQUIRED_LIBRARIES_SAVE "${CMAKE_REQUIRED_LIBRARIES}")
    set(CMAKE_REQUIRED_INCLUDES "${OPENHTJ2K_INCLUDE_DIR}")
    set(CMAKE_REQUIRED_LIBRARIES "${OPENHTJ2K_LIBRARY}")
    check_cxx_source_compiles("
        #include <cstdint>
        #include <vector>
        #include \"encoder.hpp\"
        #include \"decoder.hpp\"
        int main() {
            std::vector<int32_t*> planes;
            open_htj2k::siz_params siz{};
            siz.Ssiz.push_back(15);
            open_htj2k::cod_params cod{};
            cod.number_of_layers = 1;
            open_htj2k::qcd_params qcd{};
            qcd.base_step = 1.0 / 65536.0;
            open_htj2k::openhtj2k_encoder encoder(\"\", planes, siz, cod, qcd, 255, false, 0, 0);
            const uint8_t bytes[2] = {0xff, 0x4f};
            open_htj2k::openhtj2k_decoder decoder(bytes, sizeof(bytes), 0, 0);
            return 0;
        }
    " OPENHTJ2K_HAS_PR190_UINT16_API)
    set(CMAKE_REQUIRED_INCLUDES "${CMAKE_REQUIRED_INCLUDES_SAVE}")
    set(CMAKE_REQUIRED_LIBRARIES "${CMAKE_REQUIRED_LIBRARIES_SAVE}")
endif()

if(OPENHTJ2K_INCLUDE_DIR AND OPENHTJ2K_LIBRARY AND OPENHTJ2K_HAS_PR190_UINT16_API)
    add_library(blosc2_openhtj2k_backend SHARED b2nd_layout.cpp plugin/openhtj2k_backend.cpp plugin/htj2k_openhtj2k_plugin.cpp)
    target_include_directories(blosc2_openhtj2k_backend PRIVATE ${OPENHTJ2K_INCLUDE_DIR} ${BLOSC2_INCLUDE_DIR} ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/plugin)
    target_link_libraries(blosc2_openhtj2k_backend PRIVATE ${OPENHTJ2K_LIBRARY})
    if(OPENHTJ2K_BUILT_BY_PROJECT)
        add_dependencies(blosc2_openhtj2k_backend openhtj2k_pr190_external)
    endif()

    if(UNIX AND NOT APPLE)
        set_target_properties(blosc2_openhtj2k_backend PROPERTIES
            INSTALL_RPATH "$ORIGIN;$ORIGIN/../../../../blosc2/lib"
        )
    elseif(APPLE)
        set_target_properties(blosc2_openhtj2k_backend PROPERTIES
            INSTALL_RPATH "@loader_path;@loader_path/../../../../blosc2/lib"
        )
    endif()

    if (MSVC OR MINGW)
        set_property(TARGET blosc2_openhtj2k_backend PROPERTY WINDOWS_EXPORT_ALL_SYMBOLS TRUE)
    endif()

    install(TARGETS blosc2_openhtj2k_backend
        RUNTIME DESTINATION blosc2_htj2k/plugins/htj2k/openhtj2k
        LIBRARY DESTINATION blosc2_htj2k/plugins/htj2k/openhtj2k
    )
    if(WIN32)
        set(OPENHTJ2K_RUNTIME_PATTERN "*.dll")
    elseif(APPLE)
        set(OPENHTJ2K_RUNTIME_PATTERN "libopen_htj2k*.dylib*")
    else()
        set(OPENHTJ2K_RUNTIME_PATTERN "libopen_htj2k*.so*")
    endif()
    install(DIRECTORY "${OPENHTJ2K_LIBRARY_DIR}/"
        DESTINATION blosc2_htj2k/plugins/htj2k/openhtj2k
        FILES_MATCHING PATTERN "${OPENHTJ2K_RUNTIME_PATTERN}"
    )
    message(STATUS "OpenHTJ2K backend enabled")
else()
    message(STATUS "OpenHTJ2K backend disabled (headers/libraries missing, or PR190 uint16 API probe failed)")
endif()

# Test program
if(NOT DEFINED ENV{DONT_BUILD_EXAMPLES})
    message(STATUS "DONT_BUILD_EXAMPLES not set-> Building examples")
    add_executable(test_grok test_grok.cpp ${BLOSC2_HTJ2K_CORE_SOURCES} utils.cpp)
    target_include_directories(test_grok PRIVATE ${BLOSC2_INCLUDE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/plugin)
    add_executable(roundtrip roundtrip.cpp ${BLOSC2_HTJ2K_CORE_SOURCES} utils.cpp)
    target_include_directories(roundtrip PRIVATE ${BLOSC2_INCLUDE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/plugin)
    if(MSVC OR MINGW)
        target_link_libraries(test_grok ${BLOSC2_LIBRARIES} grokj2k)
        target_link_libraries(roundtrip ${BLOSC2_LIBRARIES} grokj2k)
    elseif(UNIX AND NOT APPLE)
        target_link_libraries(test_grok ${BLOSC2_LIBRARIES} grokj2k m dl)
        target_link_libraries(roundtrip ${BLOSC2_LIBRARIES} grokj2k m dl)
    else()
        target_link_libraries(test_grok ${BLOSC2_LIBRARIES} grokj2k m)
        target_link_libraries(roundtrip ${BLOSC2_LIBRARIES} grokj2k m)
    endif()
    if(DEFINED BLOSC2_LIBRARY_DIR_RESOLVED)
        set_target_properties(test_grok PROPERTIES
            BUILD_RPATH "${BLOSC2_LIBRARY_DIR_RESOLVED}"
            INSTALL_RPATH "${BLOSC2_LIBRARY_DIR_RESOLVED}"
        )
        set_target_properties(roundtrip PROPERTIES
            BUILD_RPATH "${BLOSC2_LIBRARY_DIR_RESOLVED}"
            INSTALL_RPATH "${BLOSC2_LIBRARY_DIR_RESOLVED}"
        )
    endif()
else()
    message(STATUS "DONT_BUILD_EXAMPLES set")
endif()
