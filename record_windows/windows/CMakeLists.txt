# The Flutter tooling requires that developers have a version of Visual Studio
# installed that includes CMake 3.14 or later. You should not increase this
# version, as doing so will cause the plugin to fail to compile for some
# customers of the plugin.
cmake_minimum_required(VERSION 3.23)

# Project-level configuration.
set(PROJECT_NAME "record_windows")
project(${PROJECT_NAME} LANGUAGES CXX C)

# This value is used when generating builds using this plugin, so it must
# not be changed
set(PLUGIN_NAME "record_windows_plugin")

# Download and extract prebuilt opus and ogg libraries
include(FetchContent)

# Use prebuilt binaries from xiph.org
set(OPUS_VERSION "1.4")
set(OGG_VERSION "1.3.6")

# Download opus source
FetchContent_Declare(
  opus_src
  URL https://downloads.xiph.org/releases/opus/opus-${OPUS_VERSION}.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

# Download ogg source (using zip for Windows compatibility)
FetchContent_Declare(
  ogg_src
  URL https://downloads.xiph.org/releases/ogg/libogg-${OGG_VERSION}.zip
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_GetProperties(opus_src)
if(NOT opus_src_POPULATED)
  FetchContent_Populate(opus_src)
endif()

FetchContent_GetProperties(ogg_src)
if(NOT ogg_src_POPULATED)
  FetchContent_Populate(ogg_src)
endif()

# Build ogg as a static library from source files
set(OGG_SOURCES
  ${ogg_src_SOURCE_DIR}/src/framing.c
  ${ogg_src_SOURCE_DIR}/src/bitwise.c
)

add_library(ogg_static STATIC ${OGG_SOURCES})
target_include_directories(ogg_static PUBLIC 
  ${ogg_src_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/include/ogg  # For config_types.h
)

# Build opus as a static library from source files (CELT + SILK)
file(GLOB OPUS_CELT_SOURCES ${opus_src_SOURCE_DIR}/celt/*.c)
file(GLOB OPUS_SILK_SOURCES ${opus_src_SOURCE_DIR}/silk/*.c)
file(GLOB OPUS_SILK_FIXED_SOURCES ${opus_src_SOURCE_DIR}/silk/fixed/*.c)
file(GLOB OPUS_SRC_SOURCES ${opus_src_SOURCE_DIR}/src/*.c)

# Remove demo/test files
list(FILTER OPUS_SRC_SOURCES EXCLUDE REGEX ".*demo.*")
list(FILTER OPUS_SRC_SOURCES EXCLUDE REGEX ".*test.*")
list(FILTER OPUS_CELT_SOURCES EXCLUDE REGEX ".*demo.*")
list(FILTER OPUS_CELT_SOURCES EXCLUDE REGEX ".*test.*")

add_library(opus_static STATIC 
  ${OPUS_CELT_SOURCES}
  ${OPUS_SILK_SOURCES}
  ${OPUS_SILK_FIXED_SOURCES}
  ${OPUS_SRC_SOURCES}
)

target_include_directories(opus_static PUBLIC 
  ${opus_src_SOURCE_DIR}/include
)
target_include_directories(opus_static PRIVATE
  ${opus_src_SOURCE_DIR}
  ${opus_src_SOURCE_DIR}/celt
  ${opus_src_SOURCE_DIR}/silk
  ${opus_src_SOURCE_DIR}/silk/fixed
  ${ogg_src_SOURCE_DIR}/include
)

target_compile_definitions(opus_static PRIVATE
  OPUS_BUILD
  USE_ALLOCA
  HAVE_LRINT
  HAVE_LRINTF
  FIXED_POINT
)

# Any new source files that you add to the plugin should be added here.
list(APPEND PLUGIN_SOURCES
  "record_windows_plugin.cpp"
  "record_windows_plugin.h"
  "record_config.h"
  "record.h"
  "record.cpp"
  "ring_buffer.h"
  "opus_encoder.h"
  "opus_encoder.cpp"
  "utils.h"
  "event_stream_handler.h"
)

# Define the plugin library target. Its name must not be changed (see comment
# on PLUGIN_NAME above).
add_library(${PLUGIN_NAME} SHARED
  "include/record_windows/record_windows_plugin_c_api.h"
  "record_windows_plugin_c_api.cpp"
  ${PLUGIN_SOURCES}
)

# Define MINIAUDIO_IMPLEMENTATION in one source file
target_compile_definitions(${PLUGIN_NAME} PRIVATE
  MINIAUDIO_IMPLEMENTATION
  MA_NO_DECODING
  MA_NO_ENCODING
  MA_NO_GENERATION
  MA_NO_ENGINE
  MA_NO_NODE_GRAPH
  MA_NO_RESOURCE_MANAGER
)

# Apply a standard set of build settings that are configured in the
# application-level CMakeLists.txt. This can be removed for plugins that want
# full control over build settings.
apply_standard_settings(${PLUGIN_NAME})

# Symbols are hidden by default to reduce the chance of accidental conflicts
# between plugins. This should not be removed; any symbols that should be
# exported should be explicitly exported with the FLUTTER_PLUGIN_EXPORT macro.
set_target_properties(${PLUGIN_NAME} PROPERTIES
  CXX_VISIBILITY_PRESET hidden)
target_compile_definitions(${PLUGIN_NAME} PRIVATE FLUTTER_PLUGIN_IMPL)

# Source include directories and library dependencies. Add any plugin-specific
# dependencies here.
target_include_directories(${PLUGIN_NAME} INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_include_directories(${PLUGIN_NAME} PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  ${ogg_src_SOURCE_DIR}/include
  ${opus_src_SOURCE_DIR}/include
  "${CMAKE_CURRENT_SOURCE_DIR}/include/ogg"
)

target_link_libraries(${PLUGIN_NAME} PRIVATE flutter flutter_wrapper_plugin ogg_static opus_static)

# List of absolute paths to libraries that should be bundled with the plugin.
# This list could contain prebuilt libraries, or libraries created by an
# external build triggered from this build file.
set(record_windows_bundled_libraries
  ""
  PARENT_SCOPE
)
