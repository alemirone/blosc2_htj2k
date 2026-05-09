##############################################################################
# blosc2_grok: Grok (JPEG2000 codec) plugin for Blosc2
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
# Build libblosc2_grok as a shared library on every platform.  Replacement
# backends link to the native Grok entry points, so the main target must be
# linkable and not only a Python extension module.
add_library(blosc2_grok SHARED blosc2_grok.cpp)

if (MSVC OR MINGW)
    # Necessary for compiling blosc2_grok.lib
    set_property(TARGET blosc2_grok PROPERTY WINDOWS_EXPORT_ALL_SYMBOLS TRUE)
endif()

# The plugin API header is private to this package but needed by the core
# loader and by the replacement backends built below.
target_include_directories(blosc2_grok PUBLIC ${BLOSC2_INCLUDE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/plugin)
target_link_libraries(blosc2_grok PRIVATE grokj2k ${BLOSC2_LIBRARIES})
if (UNIX AND NOT APPLE)
    target_link_libraries(blosc2_grok PRIVATE dl)
endif()

# Add runtime path just in case (e.g. load plugin before blosc2 for example)
# will not always be used, but if can't find blosc2 gives another thing to check
# before failing
if(UNIX AND NOT APPLE)
    set_target_properties(blosc2_grok PROPERTIES
        INSTALL_RPATH "$ORIGIN/../blosc2/lib"
    )
elseif(APPLE)
    set_target_properties(blosc2_grok PROPERTIES
        INSTALL_RPATH "@loader_path/../blosc2/lib"
    )
endif()

# Install
install(TARGETS blosc2_grok
    RUNTIME DESTINATION blosc2_grok
    LIBRARY DESTINATION blosc2_grok
)
# Note: blosc2_grok.h and grok headers are not installed in the wheel
# Python users don't need them, and C developers should build from source

# Optional Kakadu backend.  The core codec does not link to Kakadu; this target
# is built only when the Kakadu headers and libraries are explicitly available.
set(KAKADU_ROOT "" CACHE PATH "Kakadu installation root")
set(KAKADU_LIBRARY_DIR "" CACHE PATH "Kakadu library directory")
set(KAKADU_INCLUDE_HINTS "")
set(KAKADU_LIBRARY_HINTS "")

if(NOT KAKADU_ROOT AND DEFINED ENV{KAKADU_ROOT})
    set(KAKADU_ROOT "$ENV{KAKADU_ROOT}")
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
    NAMES kdu_v86R
    HINTS ${KAKADU_LIBRARY_HINTS} ${KAKADU_ROOT}
    PATH_SUFFIXES lib lib/Linux-x86-64-gcc apps/make
)
find_library(KAKADU_AUX_LIBRARY
    NAMES kdu_a86R
    HINTS ${KAKADU_LIBRARY_HINTS} ${KAKADU_ROOT}
    PATH_SUFFIXES lib lib/Linux-x86-64-gcc apps/make
)

if(KAKADU_INCLUDE_DIR AND KAKADU_CORE_LIBRARY AND KAKADU_AUX_LIBRARY)
    add_library(blosc2_kakadu_j2k_backend SHARED plugin/kakadu_backend.cpp plugin/j2k_kakadu_plugin.cpp)
    target_include_directories(blosc2_kakadu_j2k_backend PRIVATE ${KAKADU_INCLUDE_DIR} ${BLOSC2_INCLUDE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/plugin)
    target_link_libraries(blosc2_kakadu_j2k_backend PRIVATE ${KAKADU_CORE_LIBRARY} ${KAKADU_AUX_LIBRARY} ${BLOSC2_LIBRARIES})

    add_library(blosc2_kakadu_htj2k_backend SHARED plugin/kakadu_backend.cpp plugin/htj2k_kakadu_plugin.cpp)
    target_include_directories(blosc2_kakadu_htj2k_backend PRIVATE ${KAKADU_INCLUDE_DIR} ${BLOSC2_INCLUDE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/plugin)
    target_link_libraries(blosc2_kakadu_htj2k_backend PRIVATE ${KAKADU_CORE_LIBRARY} ${KAKADU_AUX_LIBRARY} ${BLOSC2_LIBRARIES})

    if(UNIX AND NOT APPLE)
        set_target_properties(blosc2_kakadu_j2k_backend blosc2_kakadu_htj2k_backend PROPERTIES
            INSTALL_RPATH "$ORIGIN/../../../../blosc2/lib"
        )
    elseif(APPLE)
        set_target_properties(blosc2_kakadu_j2k_backend blosc2_kakadu_htj2k_backend PROPERTIES
            INSTALL_RPATH "@loader_path/../../../../blosc2/lib"
        )
    endif()

    if (MSVC OR MINGW)
        set_property(TARGET blosc2_kakadu_j2k_backend PROPERTY WINDOWS_EXPORT_ALL_SYMBOLS TRUE)
        set_property(TARGET blosc2_kakadu_htj2k_backend PROPERTY WINDOWS_EXPORT_ALL_SYMBOLS TRUE)
    endif()

    install(TARGETS blosc2_kakadu_j2k_backend
        RUNTIME DESTINATION blosc2_grok/plugins/j2k/kakadu
        LIBRARY DESTINATION blosc2_grok/plugins/j2k/kakadu
    )
    install(TARGETS blosc2_kakadu_htj2k_backend
        RUNTIME DESTINATION blosc2_grok/plugins/htj2k/kakadu
        LIBRARY DESTINATION blosc2_grok/plugins/htj2k/kakadu
    )
    message(STATUS "Kakadu backend enabled")
else()
    message(STATUS "Kakadu backend disabled (Kakadu headers/libraries not found)")
endif()

# Optional OpenHTJ2K backend.  It is built only when an OpenHTJ2K installation
# with the public C++ encoder/decoder interface is available.
set(OPENHTJ2K_ROOT "" CACHE PATH "OpenHTJ2K installation root")
set(OPENHTJ2K_LIBRARY_DIR "" CACHE PATH "OpenHTJ2K library directory")
set(OPENHTJ2K_INCLUDE_HINTS "")
set(OPENHTJ2K_LIBRARY_HINTS "")

if(NOT OPENHTJ2K_ROOT AND DEFINED ENV{OPENHTJ2K_ROOT})
    set(OPENHTJ2K_ROOT "$ENV{OPENHTJ2K_ROOT}")
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

unset(OPENHTJ2K_HAS_PR190_UINT16_API)
unset(OPENHTJ2K_HAS_PR190_UINT16_API CACHE)
if(OPENHTJ2K_INCLUDE_DIR AND OPENHTJ2K_LIBRARY)
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
    add_library(blosc2_openhtj2k_backend SHARED plugin/openhtj2k_backend.cpp plugin/htj2k_openhtj2k_plugin.cpp)
    target_include_directories(blosc2_openhtj2k_backend PRIVATE ${OPENHTJ2K_INCLUDE_DIR} ${BLOSC2_INCLUDE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/plugin)
    target_link_libraries(blosc2_openhtj2k_backend PRIVATE ${OPENHTJ2K_LIBRARY} ${BLOSC2_LIBRARIES})

    if(UNIX AND NOT APPLE)
        set_target_properties(blosc2_openhtj2k_backend PROPERTIES
            INSTALL_RPATH "$ORIGIN/../../../../blosc2/lib"
        )
    elseif(APPLE)
        set_target_properties(blosc2_openhtj2k_backend PROPERTIES
            INSTALL_RPATH "@loader_path/../../../../blosc2/lib"
        )
    endif()

    if (MSVC OR MINGW)
        set_property(TARGET blosc2_openhtj2k_backend PROPERTY WINDOWS_EXPORT_ALL_SYMBOLS TRUE)
    endif()

    install(TARGETS blosc2_openhtj2k_backend
        RUNTIME DESTINATION blosc2_grok/plugins/htj2k/openhtj2k
        LIBRARY DESTINATION blosc2_grok/plugins/htj2k/openhtj2k
    )
    message(STATUS "OpenHTJ2K backend enabled")
else()
    message(STATUS "OpenHTJ2K backend disabled (headers/libraries missing, or PR190 uint16 API probe failed)")
endif()

# Reference replacement backend.  It uses the native Grok path so the runtime
# replacement mechanism is always testable, even when Kakadu is unavailable.
add_library(blosc2_grok_backend SHARED plugin/j2k_grok_plugin.cpp)
target_include_directories(blosc2_grok_backend PRIVATE ${BLOSC2_INCLUDE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/plugin ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(blosc2_grok_backend PRIVATE blosc2_grok grokj2k ${BLOSC2_LIBRARIES})
if (MSVC OR MINGW)
    set_property(TARGET blosc2_grok_backend PROPERTY WINDOWS_EXPORT_ALL_SYMBOLS TRUE)
endif()
if(UNIX AND NOT APPLE)
    set_target_properties(blosc2_grok_backend PROPERTIES
        INSTALL_RPATH "$ORIGIN/../../..;$ORIGIN/../../../../blosc2/lib"
    )
elseif(APPLE)
    set_target_properties(blosc2_grok_backend PROPERTIES
        INSTALL_RPATH "@loader_path/../../..;@loader_path/../../../../blosc2/lib"
    )
endif()
install(TARGETS blosc2_grok_backend
    RUNTIME DESTINATION blosc2_grok/plugins/j2k/grok
    LIBRARY DESTINATION blosc2_grok/plugins/j2k/grok
)

# Test program
if(NOT DEFINED ENV{DONT_BUILD_EXAMPLES})
    message(STATUS "DONT_BUILD_EXAMPLES not set-> Building examples")
    add_executable(test_grok test_grok.cpp blosc2_grok.cpp utils.cpp)
    target_include_directories(test_grok PRIVATE ${BLOSC2_INCLUDE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/plugin)
    add_executable(roundtrip roundtrip.cpp blosc2_grok.cpp utils.cpp)
    target_include_directories(roundtrip PRIVATE ${BLOSC2_INCLUDE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/plugin)
    if(MSVC OR MINGW)
        target_link_libraries(test_grok ${BLOSC2_LIBRARIES} grokj2k)
        target_link_libraries(roundtrip ${BLOSC2_LIBRARIES} grokj2k)
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
