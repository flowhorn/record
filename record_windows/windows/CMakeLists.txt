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

# Use bundled prebuilt fdk-aac binaries.
if(CMAKE_VS_PLATFORM_NAME STREQUAL "ARM64" OR CMAKE_SYSTEM_PROCESSOR STREQUAL "ARM64")
  message(FATAL_ERROR "No prebuilt fdk-aac package for platform: ARM64")
elseif(CMAKE_VS_PLATFORM_NAME STREQUAL "x64" OR CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(FDK_AAC_PREBUILT_TRIPLET "win-x64")
else()
  message(FATAL_ERROR "No prebuilt fdk-aac package for this platform")
endif()

set(FDK_AAC_PREBUILT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/prebuilt/fdk-aac/${FDK_AAC_PREBUILT_TRIPLET}")
set(FDK_AAC_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/prebuilt/fdk-aac/include")
set(FDK_AAC_DLL "${FDK_AAC_PREBUILT_DIR}/bin/fdk-aac.dll")
set(FDK_AAC_IMPLIB "${FDK_AAC_PREBUILT_DIR}/lib/fdk-aac.lib")

if(NOT EXISTS "${FDK_AAC_DLL}")
  message(FATAL_ERROR "Missing prebuilt fdk-aac DLL: ${FDK_AAC_DLL}")
endif()
if(NOT EXISTS "${FDK_AAC_IMPLIB}")
  message(FATAL_ERROR "Missing prebuilt fdk-aac import library: ${FDK_AAC_IMPLIB}")
endif()

add_library(fdk-aac SHARED IMPORTED GLOBAL)
set_target_properties(fdk-aac PROPERTIES
  IMPORTED_LOCATION "${FDK_AAC_DLL}"
  IMPORTED_IMPLIB "${FDK_AAC_IMPLIB}"
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
  "encoder/aac_encoder.h"
  "encoder/aac_encoder.cpp"
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
set_source_files_properties("record.cpp" PROPERTIES COMPILE_DEFINITIONS MINIAUDIO_IMPLEMENTATION)

target_compile_definitions(${PLUGIN_NAME} PRIVATE
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
  "${FDK_AAC_INCLUDE_DIR}"
)

if(MSVC)
  target_link_options(${PLUGIN_NAME} PRIVATE "/NODEFAULTLIB:LIBCMT" "/IGNORE:4099" "/IGNORE:4098" "/IGNORE:4075")
endif()

target_link_libraries(${PLUGIN_NAME} PRIVATE flutter flutter_wrapper_plugin fdk-aac)

# List of absolute paths to libraries that should be bundled with the plugin.
# This list could contain prebuilt libraries, or libraries created by an
# external build triggered from this build file.
set(record_windows_bundled_libraries
  "${FDK_AAC_DLL}"
  PARENT_SCOPE
)
