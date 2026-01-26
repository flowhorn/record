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

# Download prebuilt opus/ogg libraries from TeamPyOgg
include(FetchContent)

FetchContent_Declare(
  xiph_libs
  URL https://github.com/TeamPyOgg/VS-XiphAudioBuild/releases/download/2020.09.19/x64-ReleaseStatic.zip
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_MakeAvailable(xiph_libs)

# Prebuilt package provides .lib files only
# Headers are bundled in include/opus/ and include/ogg/

# Create imported targets for opus and ogg
add_library(ogg STATIC IMPORTED GLOBAL)
set_target_properties(ogg PROPERTIES
  IMPORTED_LOCATION "${xiph_libs_SOURCE_DIR}/ogg.lib"
)

add_library(opus STATIC IMPORTED GLOBAL)
set_target_properties(opus PROPERTIES
  IMPORTED_LOCATION "${xiph_libs_SOURCE_DIR}/opus.lib"
)
# opus depends on ogg
set_target_properties(opus PROPERTIES
  INTERFACE_LINK_LIBRARIES ogg
)

# Any new source files that you add to the plugin should be added here.
list(APPEND PLUGIN_SOURCES
  "record_windows_plugin.cpp"
  "record_windows_plugin.h"
  "record_config.h"
  "record.h"
  "record.cpp"
  "ring_buffer.h"
  "encoder/encoders.h"
  "encoder/opus_encoder.h"
  "encoder/opus_encoder.cpp"
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
  NOMINMAX  # Prevent Windows.h from defining min/max macros
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
  "${CMAKE_CURRENT_SOURCE_DIR}/encoder"
)

target_link_libraries(${PLUGIN_NAME} PRIVATE flutter flutter_wrapper_plugin ogg opus)

# List of absolute paths to libraries that should be bundled with the plugin.
# This list could contain prebuilt libraries, or libraries created by an
# external build triggered from this build file.
set(record_windows_bundled_libraries
  ""
  PARENT_SCOPE
)
