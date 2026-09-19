# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.20)

project(computer_control
        VERSION 0.7.2
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
option(CC_BUILD_MCP      "Build the MCP server"                        ON)
option(CC_BUILD_TESTS    "Build tests"                                 ON)
option(CC_ENABLE_LTO     "Link-time optimization in Release"           ON)
option(CC_ENABLE_NATIVE  "Tune for the building machine's CPU"         OFF)
option(CC_USE_SYSTEM_ZLIB "Use system zlib when available"             ON)
option(CC_BUILD_APP_BUNDLE "Build computer-control.app on macOS"       ON)
# A stable signing identity is what makes a macOS permission grant survive a
# rebuild; ad-hoc signatures change with the code hash.
set(CC_CODESIGN_IDENTITY "" CACHE STRING "codesign identity for the macOS bundle")

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
  src/core/permissions_common.cpp
  src/core/system_ui_common.cpp
  src/core/text.cpp
  src/core/fuzzy.cpp
  src/core/session.cpp
  src/devices/device_manager.cpp
  src/devices/device_discovery.cpp
  src/devices/device_onscreen.cpp
  src/devices/device_ios.cpp
  src/devices/device_android.cpp
  src/actions/actions.cpp
  src/actions/actions_device.cpp
)

if(CC_PLATFORM STREQUAL "macos")
  set(CC_PLATFORM_SOURCES
    src/platform/macos/display_macos.cpp
    src/platform/macos/input_macos.cpp
    src/platform/macos/screen_macos.mm
    src/platform/macos/window_macos.mm
    src/platform/macos/a11y_macos.mm
    src/platform/macos/system_macos.mm
    src/platform/macos/permissions_macos.mm
    src/platform/macos/system_ui_macos.mm
  )
elseif(CC_PLATFORM STREQUAL "windows")
  set(CC_PLATFORM_SOURCES
    src/platform/windows/display_windows.cpp
    src/platform/windows/input_windows.cpp
    src/platform/windows/screen_windows.cpp
    src/platform/windows/window_windows.cpp
    src/platform/windows/a11y_windows.cpp
    src/platform/windows/system_windows.cpp
    src/platform/windows/permissions_windows.cpp
    src/platform/windows/system_ui_windows.cpp
  )
else()
  set(CC_PLATFORM_SOURCES
    src/platform/linux/display_linux.cpp
    src/platform/linux/input_linux.cpp
    src/platform/linux/screen_linux.cpp
    src/platform/linux/window_linux.cpp
    src/platform/linux/a11y_linux.cpp
    src/platform/linux/system_linux.cpp
    src/platform/linux/permissions_linux.cpp
    src/platform/linux/system_ui_linux.cpp
  )
endif()

# ---------------------------------------------------------------------------
# Shared build settings
# ---------------------------------------------------------------------------
# An INTERFACE target carries the include paths, definitions, warnings and
# system libraries, and the real targets compile the sources directly.
#
# This deliberately compiles the sources twice rather than sharing an OBJECT
# library. $<TARGET_OBJECTS:...> is subtly generator-dependent - on the Visual
# Studio generator a static library built only from an object expression links
# to an empty archive, which shows up much later as dozens of unresolved
# externals with no compile error to explain them. A second compile costs about
# a minute; that failure mode costs an afternoon.
add_library(cc_config INTERFACE)

target_include_directories(cc_config INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
  $<INSTALL_INTERFACE:include>)

target_compile_definitions(cc_config INTERFACE
  CC_VERSION="${PROJECT_VERSION}"
  CC_PLATFORM_NAME="${CC_PLATFORM}")

if(CC_USE_SYSTEM_ZLIB)
  find_package(ZLIB QUIET)
  if(ZLIB_FOUND)
    target_compile_definitions(cc_config INTERFACE CC_HAVE_ZLIB=1)
    target_link_libraries(cc_config INTERFACE ZLIB::ZLIB)
    message(STATUS "computer-control: using system zlib")
  else()
    message(STATUS "computer-control: system zlib not found, using the bundled deflate")
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
    src/platform/macos/permissions_macos.mm
    src/platform/macos/system_ui_macos.mm
    PROPERTIES COMPILE_FLAGS "-fobjc-arc")
  foreach(fw CoreGraphics CoreFoundation ApplicationServices Foundation AppKit
             ScreenCaptureKit CoreMedia CoreVideo IOKit)
    find_library(CC_FW_${fw} ${fw} REQUIRED)
    target_link_libraries(cc_config INTERFACE ${CC_FW_${fw}})
  endforeach()
elseif(CC_PLATFORM STREQUAL "windows")
  target_link_libraries(cc_config INTERFACE
    user32 gdi32 shcore dwmapi ole32 oleaut32 uuid shell32 advapi32
    windowsapp comctl32 psapi version ws2_32)
  target_compile_definitions(cc_config INTERFACE
    NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
else()
  find_package(X11)
  if(X11_FOUND)
    target_compile_definitions(cc_config INTERFACE CC_HAVE_X11=1)
    target_include_directories(cc_config INTERFACE ${X11_INCLUDE_DIR})
    target_link_libraries(cc_config INTERFACE ${X11_LIBRARIES})
    if(X11_XTest_LIB)
      target_compile_definitions(cc_config INTERFACE CC_HAVE_XTEST=1)
      target_link_libraries(cc_config INTERFACE ${X11_XTest_LIB})
    endif()
    if(X11_Xrandr_LIB)
      target_compile_definitions(cc_config INTERFACE CC_HAVE_XRANDR=1)
      target_link_libraries(cc_config INTERFACE ${X11_Xrandr_LIB})
    endif()
    if(X11_Xfixes_LIB)
      target_compile_definitions(cc_config INTERFACE CC_HAVE_XFIXES=1)
      target_link_libraries(cc_config INTERFACE ${X11_Xfixes_LIB})
    endif()
  else()
    message(WARNING "X11 not found: the Linux build will only support the uinput and "
                    "portal paths, and screen capture will be unavailable.")
  endif()
  find_package(Threads REQUIRED)
  target_link_libraries(cc_config INTERFACE Threads::Threads ${CMAKE_DL_LIBS})
endif()

# Applies the warning and optimisation settings to a real (non-interface)
# target. Warnings cannot live on cc_config because INTERFACE compile options
# would also be forced on anything that links the library.
function(cc_apply_build_flags target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:__cplusplus /utf-8 /bigobj)
    target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:/O2 /Oi /Gy>)
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Wno-unused-parameter -Wno-missing-field-initializers)
    target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:-O3 -fno-math-errno>)
    if(CC_ENABLE_NATIVE)
      target_compile_options(${target} PRIVATE -march=native)
    endif()
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Libraries
# ---------------------------------------------------------------------------
set(CC_ALL_SOURCES ${CC_CORE_SOURCES} ${CC_PLATFORM_SOURCES})

add_library(cc_static STATIC ${CC_ALL_SOURCES})
target_link_libraries(cc_static PUBLIC cc_config)
cc_apply_build_flags(cc_static)
set_target_properties(cc_static PROPERTIES OUTPUT_NAME computer_control)
add_library(cc::static ALIAS cc_static)

# Link-time optimization.
#
# check_ipo_supported() is necessary but not sufficient: it compiles a trivial
# program, which can succeed while a real link fails. The common breakage is
# Clang on Linux emitting bitcode newer than the system linker's LLVM plugin
# understands ("Unknown attribute kind", because ld.bfd's gold plugin is built
# against a different LLVM than the clang in PATH). LTO here buys a few percent
# on a library whose cost is dominated by OS calls and image encoding, so a
# broken default build is a much worse trade than the lost speed - detect the
# hazard and step down instead of failing.
if(CC_ENABLE_LTO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT CC_IPO_OK OUTPUT CC_IPO_MSG)

  if(CC_IPO_OK AND CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT APPLE)
    find_program(CC_LLD_PROGRAM NAMES lld ld.lld)
    if(CC_LLD_PROGRAM)
      target_link_options(cc_static PUBLIC -fuse-ld=lld)
      message(STATUS "computer-control: LTO enabled, linking with lld")
    else()
      set(CC_IPO_OK OFF)
      message(STATUS "computer-control: LTO disabled - Clang on Linux needs lld to link "
                     "its bitcode reliably, and lld was not found. Install lld, or pass "
                     "-DCC_ENABLE_LTO=OFF to silence this.")
    endif()
  endif()

  if(CC_IPO_OK)
    set_property(TARGET cc_static PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
  else()
    message(STATUS "computer-control: building without LTO")
  endif()
endif()

# ---------------------------------------------------------------------------
# Executables
# ---------------------------------------------------------------------------
if(CC_BUILD_MCP)
  add_executable(cc_mcp
    src/mcp/main.cpp
    src/mcp/server.cpp
    src/mcp/tools.cpp
    src/mcp/transport.cpp
    src/mcp/protocol.cpp
    src/mcp/setup.cpp
    src/mcp/bridge.cpp)
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
    tests/test_device.cpp
    tests/test_text.cpp
    tests/test_mcp.cpp
    tests/test_docs.cpp
    tests/test_setup.cpp
    # The protocol layer is exercised through the real server dispatch rather
    # than a reimplementation of it, so the tests compile the MCP front-end
    # too. main.cpp is excluded: it is the only thing here with a main().
    src/mcp/protocol.cpp
    src/mcp/server.cpp
    src/mcp/tools.cpp
    src/mcp/transport.cpp
    src/mcp/setup.cpp
    src/mcp/bridge.cpp)
  target_include_directories(cc_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  # test_docs.cpp checks documented counts against the registry, which means
  # reading the repository from a build tree that can be anywhere.
  target_compile_definitions(cc_tests PRIVATE
    CC_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(cc_tests PRIVATE cc_static)
  add_test(NAME unit COMMAND cc_tests)
endif()

# ---------------------------------------------------------------------------
# macOS app bundle
# ---------------------------------------------------------------------------
if(APPLE AND CC_BUILD_APP_BUNDLE AND CC_BUILD_MCP)
  include(cmake/MacAppBundle.cmake)
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(TARGETS cc_static ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
if(CC_BUILD_MCP)
  install(TARGETS cc_mcp RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
