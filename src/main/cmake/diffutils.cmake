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

find_program(DIFF_SYSTEM_EXECUTABLE
  NAMES diff
)
if(DIFF_SYSTEM_EXECUTABLE)
  # WinGet shims may be symlinks. Declare and copy the resolved provider so the
  # real bytes, rather than only the zero-length link entry, own invalidation.
  get_filename_component(DIFF_SYSTEM_EXECUTABLE
    "${DIFF_SYSTEM_EXECUTABLE}" REALPATH)
endif()

if(DIFF_SYSTEM_EXECUTABLE)
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
