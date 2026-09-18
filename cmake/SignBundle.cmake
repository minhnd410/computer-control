# Signs the app bundle. Run as a script (cmake -P), so it only sees the
# variables passed on the command line.
#
# An ad-hoc signature is keyed on the code hash, so every rebuild produces a
# new identity and macOS drops the Accessibility grant. That is merely annoying
# in development and unacceptable for anything distributed, hence the warning.

if(NOT IDENTITY OR IDENTITY STREQUAL "")
  set(IDENTITY "-")
  message(STATUS
    "computer-control: signing ${BUNDLE_ID} ad-hoc.\n"
    "   The Accessibility grant is tied to the code hash and will be lost on every\n"
    "   rebuild. Configure with -DCC_CODESIGN_IDENTITY=\"Developer ID Application: ...\"\n"
    "   (or a self-signed certificate) to keep it across builds.")
endif()

execute_process(
  COMMAND codesign --force --sign "${IDENTITY}" --identifier "${BUNDLE_ID}"
          --options runtime --timestamp=none "${BUNDLE}"
  RESULT_VARIABLE rc
  ERROR_VARIABLE err)

if(NOT rc EQUAL 0)
  # --options runtime needs a real certificate; retry without hardened runtime
  # so an ad-hoc development build still produces a usable bundle.
  execute_process(
    COMMAND codesign --force --sign "${IDENTITY}" --identifier "${BUNDLE_ID}" "${BUNDLE}"
    RESULT_VARIABLE rc2
    ERROR_VARIABLE err2)
  if(NOT rc2 EQUAL 0)
    message(FATAL_ERROR "codesign failed: ${err}${err2}")
  endif()
endif()

message(STATUS "computer-control: signed ${BUNDLE} as ${BUNDLE_ID}")
