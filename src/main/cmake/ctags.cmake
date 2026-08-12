# CMake script for ctags
#
# requires
#   ${7ZIP_EXECUTABLE}
#   ${ARCH}
#   ${CMAKE_GENERATOR_PLATFORM}
#   ${CMAKE_GENERATOR}
#   ${CMD_VS_DEV}
#   ${GIT_EXECUTABLE}
#   ${HOST_ARCH}
#   ${OUTPUT_DIRECTORY}

# Define the ctags's path
set(CTAGS_BUILD_DIR "${CMAKE_BINARY_DIR}/ctags")
set(CTAGS_INPUT_DIR "${CMAKE_BINARY_DIR}/ctags-input")
set(CTAGS_VERSION "v6.1.0")
set(CTAGS_ZIP_FILE "${CMAKE_SOURCE_DIR}/installer/externals/universal-ctags/ctags-${CTAGS_VERSION}-${ARCH}.zip")
set(CTAGS_GENERATED "${CTAGS_BUILD_DIR}/ctags.exe")
set(CTAGS_EXECUTABLE "${OUTPUT_DIRECTORY}/ctags.exe")
set(CTAGS_EXPECTED_COMMIT "${CTAGS_INPUT_DIR}/expected-commit.txt")
set(CTAGS_SOURCE_ARCHIVE "${CTAGS_INPUT_DIR}/ctags-source.zip")
set(CTAGS_BUILD_STATE "${CTAGS_INPUT_DIR}/built-state.txt")
set(CTAGS_BUILD_SCRIPT "${CMAKE_SOURCE_DIR}/src/main/cmake/ctags_build_if_needed.cmake")
set(CTAGS_GITLINK_SCRIPT "${CMAKE_SOURCE_DIR}/src/main/cmake/gitlink_state.cmake")
set(CTAGS_ARCHIVE_SCRIPT "${CMAKE_SOURCE_DIR}/src/main/cmake/git_submodule_update_locked.cmake")
set(COPY_RUNTIME_ASSET_SCRIPT "${CMAKE_SOURCE_DIR}/src/main/cmake/copy_runtime_asset.cmake")
set(ARCHIVE_RUNTIME_SCRIPT "${CMAKE_SOURCE_DIR}/src/main/cmake/archive_runtime_if_needed.cmake")

string(SHA256 CTAGS_BUILD_SIGNATURE
  "${CMAKE_GENERATOR}|${CMAKE_GENERATOR_TOOLSET}|${CMAKE_VS_PLATFORM_TOOLSET}|${CMAKE_CXX_COMPILER_VERSION}|${CMD_VS_DEV}|${HOST_ARCH}|externals/ctags")
string(SHA256 CTAGS_ARCHIVE_SIGNATURE
  "${CTAGS_VERSION}|${ARCH}")

# The released artifacts document exactly one ctags build. build-installer.bat
# and zipArtifacts.bat both extract license/ and docs/ from
# installer/externals/universal-ctags/ctags-${CTAGS_VERSION}-<arch>.zip, so a
# ctags taken from PATH (Chocolatey, MSYS2, WinGet) or built from the
# externals/ctags gitlink ships a different version under that archive's
# license set. Stage the committed archive by default and keep the other
# providers reachable only when they are requested by name.
#
#   archive   : extract the committed, version-pinned archive (default)
#   system    : copy the ctags found on PATH
#   submodule : build the externals/ctags gitlink commit under a lock
#
# No archive is committed for every ARCH. When this ARCH has none, fall back to
# the system binary and then to the submodule build so the target still
# produces ctags.exe.
if(DEFINED ENV{SAKURA_CTAGS_SOURCE} AND NOT "$ENV{SAKURA_CTAGS_SOURCE}" STREQUAL "")
  set(SAKURA_CTAGS_SOURCE_DEFAULT "$ENV{SAKURA_CTAGS_SOURCE}")
else()
  set(SAKURA_CTAGS_SOURCE_DEFAULT "archive")
endif()
set(SAKURA_CTAGS_SOURCE "${SAKURA_CTAGS_SOURCE_DEFAULT}" CACHE STRING
  "Provider for the staged ctags.exe: archive, system, or submodule")
set_property(CACHE SAKURA_CTAGS_SOURCE PROPERTY STRINGS archive system submodule)
if(NOT SAKURA_CTAGS_SOURCE MATCHES "^(archive|system|submodule)$")
  message(FATAL_ERROR
    "SAKURA_CTAGS_SOURCE must be archive, system, or submodule "
    "(got '${SAKURA_CTAGS_SOURCE}')")
endif()

find_program(CTAGS_SYSTEM_EXECUTABLE
  NAMES ctags
)
if(CTAGS_SYSTEM_EXECUTABLE)
  get_filename_component(CTAGS_SYSTEM_EXECUTABLE
    "${CTAGS_SYSTEM_EXECUTABLE}" REALPATH)
endif()

# 実績のあるものだけビルド対象にする。
set(CTAGS_SUBMODULE_BUILDABLE FALSE)
if(CMAKE_GENERATOR MATCHES "^Visual Studio" AND
   NOT CMAKE_GENERATOR_PLATFORM STREQUAL "Win32" AND
   EXISTS "${CMAKE_SOURCE_DIR}/.git")
  set(CTAGS_SUBMODULE_BUILDABLE TRUE)
endif()

set(CTAGS_RESOLVED_SOURCE "${SAKURA_CTAGS_SOURCE}")
if(CTAGS_RESOLVED_SOURCE STREQUAL "archive" AND NOT EXISTS "${CTAGS_ZIP_FILE}")
  if(CTAGS_SYSTEM_EXECUTABLE)
    set(CTAGS_RESOLVED_SOURCE "system")
  elseif(CTAGS_SUBMODULE_BUILDABLE)
    set(CTAGS_RESOLVED_SOURCE "submodule")
  else()
    message(FATAL_ERROR
      "No ctags provider is available: '${CTAGS_ZIP_FILE}' does not exist, "
      "no ctags was found on PATH, and the submodule build is unavailable.")
  endif()
  message(STATUS
    "No committed ctags archive for ${ARCH}; "
    "staging ctags from '${CTAGS_RESOLVED_SOURCE}'.")
elseif(CTAGS_RESOLVED_SOURCE STREQUAL "system" AND NOT CTAGS_SYSTEM_EXECUTABLE)
  message(FATAL_ERROR "SAKURA_CTAGS_SOURCE=system but no ctags was found on PATH.")
elseif(CTAGS_RESOLVED_SOURCE STREQUAL "submodule" AND NOT CTAGS_SUBMODULE_BUILDABLE)
  message(FATAL_ERROR
    "SAKURA_CTAGS_SOURCE=submodule requires a Visual Studio generator, a "
    "non-Win32 platform, and a Git worktree.")
endif()

if(CTAGS_RESOLVED_SOURCE STREQUAL "system")
  add_custom_target(generate_ctags ALL
    COMMAND ${CMAKE_COMMAND}
      -DINPUT_FILE:FILEPATH=${CTAGS_SYSTEM_EXECUTABLE}
      -DOUTPUT_FILE:FILEPATH=${CTAGS_EXECUTABLE}
      -P ${COPY_RUNTIME_ASSET_SCRIPT}
    BYPRODUCTS "${CTAGS_EXECUTABLE}"
    DEPENDS
      "${CTAGS_SYSTEM_EXECUTABLE}"
      "${COPY_RUNTIME_ASSET_SCRIPT}"
    COMMENT "Ensuring the system ctags.exe is staged"
    VERBATIM
  )
elseif(CTAGS_RESOLVED_SOURCE STREQUAL "submodule")
  # A Visual Studio custom target is always out of date. Keep the observation
  # lightweight and let the script perform extraction/nmake only when the
  # parent gitlink or the build contract changes.
  add_custom_target(generate_ctags ALL
    COMMAND ${CMAKE_COMMAND}
      -DGIT_EXECUTABLE:FILEPATH=${GIT_EXECUTABLE}
      -DREPO_ROOT:PATH=${CMAKE_SOURCE_DIR}
      -DSUBMODULE_PATH:STRING=externals/ctags
      -DLOCK_PATH:FILEPATH=${CMAKE_BINARY_DIR}/ctags-build.lock
      -DGITLINK_SCRIPT:FILEPATH=${CTAGS_GITLINK_SCRIPT}
      -DARCHIVE_SCRIPT:FILEPATH=${CTAGS_ARCHIVE_SCRIPT}
      -DEXPECTED_COMMIT_FILE:FILEPATH=${CTAGS_EXPECTED_COMMIT}
      -DARCHIVE_FILE:FILEPATH=${CTAGS_SOURCE_ARCHIVE}
      -DBUILD_DIR:PATH=${CTAGS_BUILD_DIR}
      -DGENERATED_FILE:FILEPATH=${CTAGS_GENERATED}
      -DOUTPUT_FILE:FILEPATH=${CTAGS_EXECUTABLE}
      -DSTATE_FILE:FILEPATH=${CTAGS_BUILD_STATE}
      -DSEVEN_ZIP_EXECUTABLE:FILEPATH=${7ZIP_EXECUTABLE}
      -DCMD_VS_DEV:FILEPATH=${CMD_VS_DEV}
      -DHOST_ARCH:STRING=${HOST_ARCH}
      -DBUILD_SIGNATURE:STRING=${CTAGS_BUILD_SIGNATURE}
      -P ${CTAGS_BUILD_SCRIPT}
    BYPRODUCTS
      "${CTAGS_EXPECTED_COMMIT}"
      "${CTAGS_SOURCE_ARCHIVE}"
      "${CTAGS_GENERATED}"
      "${CTAGS_EXECUTABLE}"
      "${CTAGS_BUILD_STATE}"
    DEPENDS
      "${CMAKE_SOURCE_DIR}/.gitmodules"
      "${CTAGS_BUILD_SCRIPT}"
      "${CTAGS_GITLINK_SCRIPT}"
      "${CTAGS_ARCHIVE_SCRIPT}"
    COMMENT "Ensuring ctags matches the committed gitlink"
    VERBATIM
  )
else()
  set(CTAGS_ARCHIVE_STATE "${OUTPUT_DIRECTORY}/.ctags-source-state")
  add_custom_target(generate_ctags ALL
    COMMAND ${CMAKE_COMMAND}
      -DMODE:STRING=CTAGS
      -DSEVEN_ZIP_EXECUTABLE:FILEPATH=${7ZIP_EXECUTABLE}
      -DOUTPUT_DIRECTORY:PATH=${OUTPUT_DIRECTORY}
      -DSTATE_FILE:FILEPATH=${CTAGS_ARCHIVE_STATE}
      -DBUILD_SIGNATURE:STRING=${CTAGS_ARCHIVE_SIGNATURE}
      -DARCHIVE_FILE:FILEPATH=${CTAGS_ZIP_FILE}
      -DOUTPUT_FILE:FILEPATH=${CTAGS_EXECUTABLE}
      -P ${ARCHIVE_RUNTIME_SCRIPT}
    BYPRODUCTS
      "${CTAGS_EXECUTABLE}"
      "${CTAGS_ARCHIVE_STATE}"
    DEPENDS
      "${CTAGS_ZIP_FILE}"
      "${ARCHIVE_RUNTIME_SCRIPT}"
    COMMENT "Ensuring the archived ctags.exe is staged"
    VERBATIM
  )
endif()
