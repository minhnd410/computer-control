# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.20)

project(computer_control
        VERSION 0.1.0
        DESCRIPTION "Cross-platform desktop and mobile-simulator automation core"
        LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(CC_BUILD_SHARED   "Build the shared library exposing the C ABI" ON)
option(CC_BUILD_CLI      "Build the cc command-line tool"              ON)
option(CC_BUILD_MCP      "Build the MCP server"                        ON)
option(CC_BUILD_TESTS    "Build tests"                                 ON)
option(CC_ENABLE_LTO     "Link-time optimization in Release"           ON)
option(CC_ENABLE_NATIVE  "Tune for the building machine's CPU"         OFF)
option(CC_USE_SYSTEM_ZLIB "Use system zlib when available"             ON)

# ---------------------------------------------------------------------------
# Platform detection
# ---------------------------------------------------------------------------
if(APPLE)
  set(CC_PLATFORM "macos")
elseif(WIN32)
  set(CC_PLATFORM "windows")
elseif(UNIX)
  set(CC_PLATFORM "linux")
else()
  message(FATAL_ERROR "Unsupported platform")
endif()
message(STATUS "computer-control: platform = ${CC_PLATFORM}")

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
set(CC_CORE_SOURCES
  src/core/types.cpp
  src/core/json.cpp
  src/core/display.cpp
  src/core/keys.cpp
  src/core/motion.cpp
  src/core/deflate.cpp
  src/core/image.cpp
  src/core/image_decode.cpp
  src/core/a11y_common.cpp
  src/core/fuzzy.cpp
  src/core/session.cpp
  src/devices/device_manager.cpp
  src/devices/device_discovery.cpp
  src/devices/device_onscreen.cpp
  src/devices/device_ios.cpp
  src/devices/device_android.cpp
  src/capi/capi.cpp
  src/capi/actions.cpp
  src/capi/actions_device.cpp
)

if(CC_PLATFORM STREQUAL "macos")
  set(CC_PLATFORM_SOURCES
    src/platform/macos/display_macos.cpp
    src/platform/macos/input_macos.cpp
    src/platform/macos/screen_macos.mm
    src/platform/macos/window_macos.mm
    src/platform/macos/a11y_macos.mm
    src/platform/macos/system_macos.mm
  )
elseif(CC_PLATFORM STREQUAL "windows")
  set(CC_PLATFORM_SOURCES
    src/platform/windows/display_windows.cpp
    src/platform/windows/input_windows.cpp
    src/platform/windows/screen_windows.cpp
    src/platform/windows/window_windows.cpp
    src/platform/windows/a11y_windows.cpp
    src/platform/windows/system_windows.cpp
  )
else()
  set(CC_PLATFORM_SOURCES
    src/platform/linux/display_linux.cpp
    src/platform/linux/input_linux.cpp
    src/platform/linux/screen_linux.cpp
    src/platform/linux/window_linux.cpp
    src/platform/linux/a11y_linux.cpp
    src/platform/linux/system_linux.cpp
  )
endif()

# ---------------------------------------------------------------------------
# Core object library (compiled once, reused by static, shared, CLI and MCP)
# ---------------------------------------------------------------------------
add_library(cc_objects OBJECT ${CC_CORE_SOURCES} ${CC_PLATFORM_SOURCES})

target_include_directories(cc_objects PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
  $<INSTALL_INTERFACE:include>)

target_compile_definitions(cc_objects PUBLIC
  CC_VERSION="${PROJECT_VERSION}"
  CC_PLATFORM_NAME="${CC_PLATFORM}")

if(CC_USE_SYSTEM_ZLIB)
  find_package(ZLIB QUIET)
  if(ZLIB_FOUND)
    target_compile_definitions(cc_objects PRIVATE CC_HAVE_ZLIB=1)
    target_link_libraries(cc_objects PUBLIC ZLIB::ZLIB)
    message(STATUS "computer-control: using system zlib")
  else()
    message(STATUS "computer-control: system zlib not found, using the bundled deflate")
  endif()
endif()

# --- warnings and optimisation ---
if(MSVC)
  target_compile_options(cc_objects PRIVATE /W4 /permissive- /Zc:__cplusplus /utf-8 /bigobj)
  target_compile_definitions(cc_objects PRIVATE
    NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
  target_compile_options(cc_objects PRIVATE $<$<CONFIG:Release>:/O2 /Oi /Gy>)
else()
  target_compile_options(cc_objects PRIVATE
    -Wall -Wextra -Wpedantic -Wno-unused-parameter -Wno-missing-field-initializers)
  target_compile_options(cc_objects PRIVATE $<$<CONFIG:Release>:-O3 -fno-math-errno>)
  if(CC_ENABLE_NATIVE)
    target_compile_options(cc_objects PRIVATE -march=native)
  endif()
endif()

if(CC_ENABLE_LTO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT CC_IPO_OK OUTPUT CC_IPO_MSG)
  if(CC_IPO_OK)
    set_property(TARGET cc_objects PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
  endif()
endif()

# --- platform libraries ---
if(CC_PLATFORM STREQUAL "macos")
  enable_language(OBJCXX)
  set(CMAKE_OBJCXX_STANDARD 20)
  set_source_files_properties(
    src/platform/macos/screen_macos.mm
    src/platform/macos/window_macos.mm
    src/platform/macos/a11y_macos.mm
    src/platform/macos/system_macos.mm
    PROPERTIES COMPILE_FLAGS "-fobjc-arc")
  foreach(fw CoreGraphics CoreFoundation ApplicationServices Foundation AppKit ScreenCaptureKit CoreMedia CoreVideo IOKit)
    find_library(CC_FW_${fw} ${fw} REQUIRED)
    target_link_libraries(cc_objects PUBLIC ${CC_FW_${fw}})
  endforeach()
elseif(CC_PLATFORM STREQUAL "windows")
  target_link_libraries(cc_objects PUBLIC
    user32 gdi32 shcore dwmapi ole32 oleaut32 uuid shell32 advapi32
    windowsapp comctl32 psapi version)
else()
  find_package(X11)
  if(X11_FOUND)
    target_compile_definitions(cc_objects PRIVATE CC_HAVE_X11=1)
    target_include_directories(cc_objects PRIVATE ${X11_INCLUDE_DIR})
    target_link_libraries(cc_objects PUBLIC ${X11_LIBRARIES})
    if(X11_XTest_LIB)
      target_compile_definitions(cc_objects PRIVATE CC_HAVE_XTEST=1)
      target_link_libraries(cc_objects PUBLIC ${X11_XTest_LIB})
    endif()
    if(X11_Xrandr_LIB)
      target_compile_definitions(cc_objects PRIVATE CC_HAVE_XRANDR=1)
      target_link_libraries(cc_objects PUBLIC ${X11_Xrandr_LIB})
    endif()
    if(X11_Xfixes_LIB)
      target_compile_definitions(cc_objects PRIVATE CC_HAVE_XFIXES=1)
      target_link_libraries(cc_objects PUBLIC ${X11_Xfixes_LIB})
    endif()
  else()
    message(WARNING "X11 not found: the Linux build will only support the uinput and "
                    "portal paths, and screen capture will be unavailable.")
  endif()
  find_package(Threads REQUIRED)
  target_link_libraries(cc_objects PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
endif()

# ---------------------------------------------------------------------------
# Libraries
# ---------------------------------------------------------------------------
add_library(cc_static STATIC $<TARGET_OBJECTS:cc_objects>)
target_link_libraries(cc_static PUBLIC cc_objects)
set_target_properties(cc_static PROPERTIES OUTPUT_NAME computer_control)
add_library(cc::static ALIAS cc_static)

if(CC_BUILD_SHARED)
  add_library(cc_shared SHARED $<TARGET_OBJECTS:cc_objects>)
  target_link_libraries(cc_shared PUBLIC cc_objects)
  set_target_properties(cc_shared PROPERTIES
    OUTPUT_NAME computer_control
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
  target_compile_definitions(cc_objects PRIVATE CC_BUILDING_SHARED=1)
  add_library(cc::shared ALIAS cc_shared)
endif()

# ---------------------------------------------------------------------------
# Executables
# ---------------------------------------------------------------------------
if(CC_BUILD_CLI)
  add_executable(cc_cli src/cli/main.cpp)
  target_link_libraries(cc_cli PRIVATE cc_static)
  set_target_properties(cc_cli PROPERTIES OUTPUT_NAME cc)
endif()

if(CC_BUILD_MCP)
  add_executable(cc_mcp
    src/mcp/main.cpp
    src/mcp/server.cpp
    src/mcp/tools.cpp
    src/mcp/transport.cpp)
  target_link_libraries(cc_mcp PRIVATE cc_static)
  set_target_properties(cc_mcp PROPERTIES OUTPUT_NAME computer-control-mcp)
endif()

if(CC_BUILD_TESTS)
  enable_testing()
  add_executable(cc_tests
    tests/test_main.cpp
    tests/test_json.cpp
    tests/test_keys.cpp
    tests/test_motion.cpp
    tests/test_display.cpp
    tests/test_image.cpp
    tests/test_device.cpp)
  target_include_directories(cc_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  target_link_libraries(cc_tests PRIVATE cc_static)
  add_test(NAME unit COMMAND cc_tests)
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(TARGETS cc_static ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
if(CC_BUILD_SHARED)
  install(TARGETS cc_shared LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
                            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
if(CC_BUILD_CLI)
  install(TARGETS cc_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
if(CC_BUILD_MCP)
  install(TARGETS cc_mcp RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
