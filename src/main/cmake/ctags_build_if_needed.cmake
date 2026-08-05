# Build the committed ctags gitlink only when its commit or build contract
# changes.  A successful state is published last; all failure branches leave a
# retryable, non-terminal build state.
#
# Parameters (passed via -D):
#   GIT_EXECUTABLE, REPO_ROOT, SUBMODULE_PATH, LOCK_PATH
#   GITLINK_SCRIPT, ARCHIVE_SCRIPT
#   EXPECTED_COMMIT_FILE, ARCHIVE_FILE
#   BUILD_DIR, GENERATED_FILE, OUTPUT_FILE, STATE_FILE
#   SEVEN_ZIP_EXECUTABLE, CMD_VS_DEV, HOST_ARCH, BUILD_SIGNATURE

foreach(required IN ITEMS
    GIT_EXECUTABLE REPO_ROOT SUBMODULE_PATH LOCK_PATH GITLINK_SCRIPT ARCHIVE_SCRIPT
    EXPECTED_COMMIT_FILE ARCHIVE_FILE BUILD_DIR GENERATED_FILE OUTPUT_FILE STATE_FILE
    SEVEN_ZIP_EXECUTABLE CMD_VS_DEV HOST_ARCH BUILD_SIGNATURE)
  if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
    message(FATAL_ERROR "${required} is required")
  endif()
endforeach()

get_filename_component(lock_directory "${LOCK_PATH}" DIRECTORY)
file(MAKE_DIRECTORY "${lock_directory}")
file(LOCK "${LOCK_PATH}" TIMEOUT 600)

execute_process(
  COMMAND "${CMAKE_COMMAND}"
    "-DGIT_EXECUTABLE:FILEPATH=${GIT_EXECUTABLE}"
    "-DREPO_ROOT:PATH=${REPO_ROOT}"
    "-DSUBMODULE_PATH:STRING=${SUBMODULE_PATH}"
    "-DOUTPUT_FILE:FILEPATH=${EXPECTED_COMMIT_FILE}"
    -P "${GITLINK_SCRIPT}"
  RESULT_VARIABLE gitlink_result
  OUTPUT_VARIABLE gitlink_stdout
  ERROR_VARIABLE gitlink_stderr
)
if(NOT gitlink_result EQUAL 0)
  message(FATAL_ERROR
    "Could not observe the ctags gitlink: ${gitlink_stdout}\n${gitlink_stderr}")
endif()
file(READ "${EXPECTED_COMMIT_FILE}" expected_commit)
string(STRIP "${expected_commit}" expected_commit)
file(SHA256 "${CMAKE_CURRENT_LIST_FILE}" build_script_hash)
file(SHA256 "${GITLINK_SCRIPT}" gitlink_script_hash)
file(SHA256 "${ARCHIVE_SCRIPT}" archive_script_hash)
set(expected_state_prefix
  "${expected_commit}\n${BUILD_SIGNATURE}\n${build_script_hash}\n${gitlink_script_hash}\n${archive_script_hash}\n")

set(existing_state "")
if(EXISTS "${STATE_FILE}")
  file(READ "${STATE_FILE}" existing_state)
endif()
set(generated_state "")
if(EXISTS "${GENERATED_FILE}")
  file(SHA256 "${GENERATED_FILE}" generated_hash)
  set(generated_state "${expected_state_prefix}generated=${generated_hash}\n")
endif()
if(NOT generated_state STREQUAL "" AND existing_state STREQUAL generated_state)
  get_filename_component(output_directory "${OUTPUT_FILE}" DIRECTORY)
  file(MAKE_DIRECTORY "${output_directory}")
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${GENERATED_FILE}" "${OUTPUT_FILE}"
    RESULT_VARIABLE stage_result
    ERROR_VARIABLE stage_stderr
  )
  if(NOT stage_result EQUAL 0 OR NOT EXISTS "${OUTPUT_FILE}")
    message(FATAL_ERROR "Could not stage cached ctags.exe: ${stage_stderr}")
  endif()
  return()
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}"
    "-DGIT_EXECUTABLE:FILEPATH=${GIT_EXECUTABLE}"
    "-DREPO_ROOT:PATH=${REPO_ROOT}"
    "-DSUBMODULE_PATH:STRING=${SUBMODULE_PATH}"
    "-DLOCK_PATH:FILEPATH=${LOCK_PATH}.submodule"
    "-DEXPECTED_COMMIT_FILE:FILEPATH=${EXPECTED_COMMIT_FILE}"
    "-DARCHIVE_FILE:FILEPATH=${ARCHIVE_FILE}"
    -P "${ARCHIVE_SCRIPT}"
  RESULT_VARIABLE archive_result
  OUTPUT_VARIABLE archive_stdout
  ERROR_VARIABLE archive_stderr
)
if(NOT archive_result EQUAL 0)
  message(FATAL_ERROR
    "Could not materialize the committed ctags source: ${archive_stdout}\n${archive_stderr}")
endif()

file(REMOVE_RECURSE "${BUILD_DIR}")
file(MAKE_DIRECTORY "${BUILD_DIR}")
execute_process(
  COMMAND "${SEVEN_ZIP_EXECUTABLE}" x "${ARCHIVE_FILE}" "-o${BUILD_DIR}" -y
  RESULT_VARIABLE extract_result
  OUTPUT_VARIABLE extract_stdout
  ERROR_VARIABLE extract_stderr
)
if(NOT extract_result EQUAL 0 OR NOT EXISTS "${BUILD_DIR}/mk_mvc.mak")
  message(FATAL_ERROR
    "Could not extract the committed ctags source: ${extract_stdout}\n${extract_stderr}")
endif()

execute_process(
  COMMAND cmd.exe /d /c call "${CMD_VS_DEV}"
    "-host_arch=${HOST_ARCH}" "-arch=${HOST_ARCH}"
    && nmake -f mk_mvc.mak
  WORKING_DIRECTORY "${BUILD_DIR}"
  RESULT_VARIABLE build_result
  OUTPUT_VARIABLE build_stdout
  ERROR_VARIABLE build_stderr
)
if(NOT build_result EQUAL 0 OR NOT EXISTS "${GENERATED_FILE}")
  message(FATAL_ERROR "Could not build ctags: ${build_stdout}\n${build_stderr}")
endif()

get_filename_component(output_directory "${OUTPUT_FILE}" DIRECTORY)
file(MAKE_DIRECTORY "${output_directory}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${GENERATED_FILE}" "${OUTPUT_FILE}"
  RESULT_VARIABLE stage_result
  ERROR_VARIABLE stage_stderr
)
if(NOT stage_result EQUAL 0 OR NOT EXISTS "${OUTPUT_FILE}")
  message(FATAL_ERROR "Could not stage newly built ctags.exe: ${stage_stderr}")
endif()
file(SHA256 "${GENERATED_FILE}" generated_hash)
set(published_state "${expected_state_prefix}generated=${generated_hash}\n")

get_filename_component(state_directory "${STATE_FILE}" DIRECTORY)
file(MAKE_DIRECTORY "${state_directory}")
set(state_temporary "${STATE_FILE}.tmp")
file(WRITE "${state_temporary}" "${published_state}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${state_temporary}" "${STATE_FILE}"
  RESULT_VARIABLE state_result
)
file(REMOVE "${state_temporary}")
if(NOT state_result EQUAL 0)
  message(FATAL_ERROR "Could not publish ctags build state: ${STATE_FILE}")
endif()
