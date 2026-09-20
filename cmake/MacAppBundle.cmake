# Builds computer-control.app, a minimal LSUIElement bundle wrapping the MCP
# server.
#
# Why a bundle at all, for a command-line binary: macOS attributes a TCC grant to
# the *responsible process*. A binary started from a terminal is attributed to
# the terminal, so it never appears in System Settings > Accessibility, the
# permission prompt never fires (the OS considers the request already answered
# by the parent), and the inherited grant turns out to be partial - windows
# come back as placeholders. There is nothing for the user to enable.
#
# A bundle launched as the service executable is its own responsible process
# with a stable bundle identifier, so it prompts properly, appears in the list
# with a real name, and keeps its grant across rebuilds *provided* the signing
# identity is stable. Ad-hoc signatures are keyed on the code hash, which
# changes every build, so the grant is lost each time - set
# CC_CODESIGN_IDENTITY to a real certificate to avoid that.

if(NOT APPLE)
  return()
endif()

set(CC_BUNDLE_DIR "${CMAKE_BINARY_DIR}/computer-control.app")
set(CC_BUNDLE_ID "dev.computercontrol.mcp" CACHE STRING "Bundle identifier for the macOS app")

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in"
               "${CMAKE_BINARY_DIR}/generated/Info.plist" @ONLY)

add_custom_target(macos_bundle
  DEPENDS cc_mcp
  COMMENT "Building computer-control.app"

  COMMAND ${CMAKE_COMMAND} -E make_directory "${CC_BUNDLE_DIR}/Contents/MacOS"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CC_BUNDLE_DIR}/Contents/Resources"
  COMMAND ${CMAKE_COMMAND} -E copy
          "${CMAKE_BINARY_DIR}/generated/Info.plist" "${CC_BUNDLE_DIR}/Contents/Info.plist"
  COMMAND ${CMAKE_COMMAND} -E copy
          "$<TARGET_FILE:cc_mcp>" "${CC_BUNDLE_DIR}/Contents/MacOS/computer-control-mcp"

  # Sign last, after every payload file is in place: signing the bundle first
  # and copying into it afterwards invalidates the signature, and macOS then
  # refuses the grant with a misleading "damaged application" error.
  COMMAND ${CMAKE_COMMAND}
          -DBUNDLE=${CC_BUNDLE_DIR}
          -DIDENTITY=${CC_CODESIGN_IDENTITY}
          -DBUNDLE_ID=${CC_BUNDLE_ID}
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/SignBundle.cmake"
  VERBATIM)
