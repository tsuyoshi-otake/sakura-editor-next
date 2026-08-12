# CMake script for diffutils
#
# requires
#   ${7ZIP_EXECUTABLE}

# Define the diffutils's variables
set(DIFF_VERSION "2.8.7-1")
set(DIFF_ZIP_FILE1 "${CMAKE_SOURCE_DIR}/externals/diffutils/diffutils-${DIFF_VERSION}-bin.zip")
set(DIFF_ZIP_FILE2 "${CMAKE_SOURCE_DIR}/externals/diffutils/diffutils-${DIFF_VERSION}-dep.zip")
set(DIFF_EXECUTABLE "${OUTPUT_DIRECTORY}/diff.exe")
set(DIFF_RUNTIME_FILES "${DIFF_EXECUTABLE}")
set(COPY_RUNTIME_ASSET_SCRIPT "${CMAKE_SOURCE_DIR}/src/main/cmake/copy_runtime_asset.cmake")
set(ARCHIVE_RUNTIME_SCRIPT "${CMAKE_SOURCE_DIR}/src/main/cmake/archive_runtime_if_needed.cmake")
string(SHA256 DIFFUTILS_BUILD_SIGNATURE
  "${DIFF_VERSION}")

# Which provider stages diff.exe.
#
#   auto    : the diff found on PATH, otherwise the committed archive (default)
#   system  : the diff found on PATH
#   archive : the committed archive
#   none    : stage nothing
#
# `none` exists for packaging builds. No released artifact carries diff.exe:
# neither sakura-common.iss nor zipArtifacts.bat copies it, and no test runs it.
# The archive provider additionally stages libintl3.dll and libiconv2.dll into
# the output directory, which zipArtifacts.bat would then pick up through its
# `*.dll` copy and ship without their license text. Selecting the provider by
# name keeps that out of the release ZIP instead of leaving it to whether the
# build machine happens to have diff on PATH.
if(DEFINED ENV{SAKURA_DIFFUTILS_SOURCE} AND NOT "$ENV{SAKURA_DIFFUTILS_SOURCE}" STREQUAL "")
  set(SAKURA_DIFFUTILS_SOURCE_DEFAULT "$ENV{SAKURA_DIFFUTILS_SOURCE}")
else()
  set(SAKURA_DIFFUTILS_SOURCE_DEFAULT "auto")
endif()
set(SAKURA_DIFFUTILS_SOURCE "${SAKURA_DIFFUTILS_SOURCE_DEFAULT}" CACHE STRING
  "Provider for the staged diff.exe: auto, system, archive, or none")
set_property(CACHE SAKURA_DIFFUTILS_SOURCE PROPERTY STRINGS auto system archive none)
if(NOT SAKURA_DIFFUTILS_SOURCE MATCHES "^(auto|system|archive|none)$")
  message(FATAL_ERROR
    "SAKURA_DIFFUTILS_SOURCE must be auto, system, archive, or none "
    "(got '${SAKURA_DIFFUTILS_SOURCE}')")
endif()

find_program(DIFF_SYSTEM_EXECUTABLE
  NAMES diff
)
if(DIFF_SYSTEM_EXECUTABLE)
  # WinGet shims may be symlinks. Declare and copy the resolved provider so the
  # real bytes, rather than only the zero-length link entry, own invalidation.
  get_filename_component(DIFF_SYSTEM_EXECUTABLE
    "${DIFF_SYSTEM_EXECUTABLE}" REALPATH)
endif()

set(DIFF_RESOLVED_SOURCE "${SAKURA_DIFFUTILS_SOURCE}")
if(DIFF_RESOLVED_SOURCE STREQUAL "auto")
  if(DIFF_SYSTEM_EXECUTABLE)
    set(DIFF_RESOLVED_SOURCE "system")
  else()
    set(DIFF_RESOLVED_SOURCE "archive")
  endif()
elseif(DIFF_RESOLVED_SOURCE STREQUAL "system" AND NOT DIFF_SYSTEM_EXECUTABLE)
  message(FATAL_ERROR "SAKURA_DIFFUTILS_SOURCE=system but no diff was found on PATH.")
endif()

if(DIFF_RESOLVED_SOURCE STREQUAL "none")
  set(DIFF_RUNTIME_FILES)
  add_custom_target(generate_diffutils ALL
    COMMAND ${CMAKE_COMMAND} -E true
    COMMENT "Skipping diff.exe staging (SAKURA_DIFFUTILS_SOURCE=none)"
    VERBATIM
  )
elseif(DIFF_RESOLVED_SOURCE STREQUAL "system")
  add_custom_target(generate_diffutils ALL
    COMMAND ${CMAKE_COMMAND}
      -DINPUT_FILE:FILEPATH=${DIFF_SYSTEM_EXECUTABLE}
      -DOUTPUT_FILE:FILEPATH=${DIFF_EXECUTABLE}
      -P ${COPY_RUNTIME_ASSET_SCRIPT}
    BYPRODUCTS "${DIFF_EXECUTABLE}"
    DEPENDS
      "${DIFF_SYSTEM_EXECUTABLE}"
      "${COPY_RUNTIME_ASSET_SCRIPT}"
    COMMENT "Ensuring the system diff.exe is staged"
    VERBATIM
  )
else()
  set(DIFF_RUNTIME_FILES
    "${DIFF_EXECUTABLE}"
    "${OUTPUT_DIRECTORY}/libintl3.dll"
    "${OUTPUT_DIRECTORY}/libiconv2.dll"
  )
  set(DIFFUTILS_STATE "${OUTPUT_DIRECTORY}/.diffutils-source-state")
  add_custom_target(generate_diffutils ALL
    COMMAND ${CMAKE_COMMAND}
      -DMODE:STRING=DIFFUTILS
      -DSEVEN_ZIP_EXECUTABLE:FILEPATH=${7ZIP_EXECUTABLE}
      -DOUTPUT_DIRECTORY:PATH=${OUTPUT_DIRECTORY}
      -DSTATE_FILE:FILEPATH=${DIFFUTILS_STATE}
      -DBUILD_SIGNATURE:STRING=${DIFFUTILS_BUILD_SIGNATURE}
      -DARCHIVE_FILE1:FILEPATH=${DIFF_ZIP_FILE1}
      -DARCHIVE_FILE2:FILEPATH=${DIFF_ZIP_FILE2}
      -DDIFF_OUTPUT:FILEPATH=${DIFF_EXECUTABLE}
      -DINTL_OUTPUT:FILEPATH=${OUTPUT_DIRECTORY}/libintl3.dll
      -DICONV_OUTPUT:FILEPATH=${OUTPUT_DIRECTORY}/libiconv2.dll
      -P ${ARCHIVE_RUNTIME_SCRIPT}
    BYPRODUCTS
      "${DIFF_EXECUTABLE}"
      "${OUTPUT_DIRECTORY}/libintl3.dll"
      "${OUTPUT_DIRECTORY}/libiconv2.dll"
      "${DIFFUTILS_STATE}"
    DEPENDS
      "${DIFF_ZIP_FILE1}"
      "${DIFF_ZIP_FILE2}"
      "${ARCHIVE_RUNTIME_SCRIPT}"
    COMMENT "Ensuring the archived diffutils runtime is staged"
    VERBATIM
  )
endif()
