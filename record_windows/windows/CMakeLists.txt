# The Flutter tooling requires that developers have a version of Visual Studio
# installed that includes CMake 3.14 or later. You should not increase this
# version, as doing so will cause the plugin to fail to compile for some
# customers of the plugin.
cmake_minimum_required(VERSION 3.23)

# Allow old cmake_minimum_required from dependencies (ogg uses an old version)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)

# Project-level configuration.
set(PROJECT_NAME "record_windows")
project(${PROJECT_NAME} LANGUAGES CXX C)

# This value is used when generating builds using this plugin, so it must
# not be changed
set(PLUGIN_NAME "record_windows_plugin")

# Fetch opus and ogg from GitHub
include(FetchContent)

FetchContent_Declare(
  ogg
  GIT_REPOSITORY https://github.com/xiph/ogg.git
  GIT_TAG v1.3.5
)

FetchContent_Declare(
  opus
  GIT_REPOSITORY https://github.com/xiph/opus.git
  GIT_TAG v1.4
)

# Configure ogg build options
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(INSTALL_DOCS OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(ogg opus)

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
  "${ogg_SOURCE_DIR}/include"
  "${ogg_BINARY_DIR}/include"
  "${opus_SOURCE_DIR}/include"
)

target_link_libraries(${PLUGIN_NAME} PRIVATE flutter flutter_wrapper_plugin ogg opus)

# List of absolute paths to libraries that should be bundled with the plugin.
# This list could contain prebuilt libraries, or libraries created by an
# external build triggered from this build file.
set(record_windows_bundled_libraries
  ""
  PARENT_SCOPE
)
