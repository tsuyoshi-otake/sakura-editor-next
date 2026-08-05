# Helper script to update a git submodule with a cross-target lock.
#
# Parameters (passed via -D):
#   GIT_EXECUTABLE  : path to git.exe
#   REPO_ROOT       : repository root directory
#   SUBMODULE_PATH  : submodule path (e.g. externals/miniz-cpp)
#   LOCK_PATH       : lock file path (optional)
#   EXPECTED_COMMIT_FILE : expected gitlink commit file (optional)
#   ARCHIVE_FILE    : source archive output (optional; requires
#                     EXPECTED_COMMIT_FILE)

if(NOT DEFINED GIT_EXECUTABLE)
  message(FATAL_ERROR "GIT_EXECUTABLE is required")
endif()

if(NOT DEFINED REPO_ROOT)
  message(FATAL_ERROR "REPO_ROOT is required")
endif()

if(NOT DEFINED SUBMODULE_PATH)
  message(FATAL_ERROR "SUBMODULE_PATH is required")
endif()

if(NOT DEFINED LOCK_PATH)
  set(LOCK_PATH "${REPO_ROOT}/.git/cmake-submodule-update.lock")
endif()

if(DEFINED ARCHIVE_FILE AND NOT DEFINED EXPECTED_COMMIT_FILE)
  message(FATAL_ERROR "EXPECTED_COMMIT_FILE is required when ARCHIVE_FILE is set")
endif()

# Serialize concurrent submodule updates triggered by parallel builds.
file(LOCK "${LOCK_PATH}" TIMEOUT 600)

execute_process(
  COMMAND "${GIT_EXECUTABLE}" submodule update --init --recursive --depth 1 --recommend-shallow "${SUBMODULE_PATH}"
  WORKING_DIRECTORY "${REPO_ROOT}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)

if(NOT result EQUAL 0)
  set(hint "")
  if(EXISTS "${REPO_ROOT}/.git/config.lock")
    set(hint "\nHint: A lock file exists: ${REPO_ROOT}/.git/config.lock\nIf no git process is running, delete it and retry.")
  endif()
  message(FATAL_ERROR "FAILED: [code=${result}] ${REPO_ROOT}/${SUBMODULE_PATH}\n${stdout}\n${stderr}${hint}")
endif()

if(DEFINED EXPECTED_COMMIT_FILE)
  if(NOT EXISTS "${EXPECTED_COMMIT_FILE}")
    message(FATAL_ERROR "Expected commit file does not exist: ${EXPECTED_COMMIT_FILE}")
  endif()
  file(READ "${EXPECTED_COMMIT_FILE}" expected_commit)
  string(STRIP "${expected_commit}" expected_commit)

  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${REPO_ROOT}/${SUBMODULE_PATH}" rev-parse HEAD
    RESULT_VARIABLE head_result
    OUTPUT_VARIABLE actual_commit
    ERROR_VARIABLE head_stderr
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  if(NOT head_result EQUAL 0 OR NOT actual_commit STREQUAL expected_commit)
    message(FATAL_ERROR
      "Submodule ${SUBMODULE_PATH} is not at expected gitlink ${expected_commit}; "
      "actual='${actual_commit}': ${head_stderr}")
  endif()
endif()

if(DEFINED ARCHIVE_FILE)
  get_filename_component(archive_directory "${ARCHIVE_FILE}" DIRECTORY)
  file(MAKE_DIRECTORY "${archive_directory}")
  set(archive_temporary "${ARCHIVE_FILE}.tmp")
  file(REMOVE "${archive_temporary}")

  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${REPO_ROOT}/${SUBMODULE_PATH}"
      archive --format=zip "--output=${archive_temporary}" "${expected_commit}"
    RESULT_VARIABLE archive_result
    OUTPUT_VARIABLE archive_stdout
    ERROR_VARIABLE archive_stderr
  )
  if(NOT archive_result EQUAL 0)
    file(REMOVE "${archive_temporary}")
    message(FATAL_ERROR
      "Could not archive ${SUBMODULE_PATH} at ${expected_commit}: "
      "${archive_stdout}\n${archive_stderr}")
  endif()

  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "${archive_temporary}" "${ARCHIVE_FILE}"
    RESULT_VARIABLE copy_result
  )
  file(REMOVE "${archive_temporary}")
  if(NOT copy_result EQUAL 0)
    message(FATAL_ERROR "Could not publish submodule archive: ${ARCHIVE_FILE}")
  endif()
endif()
