# Capture the expected commit of one gitlink without using the submodule
# worktree as an incremental-build sentinel.
#
# Parameters (passed via -D):
#   GIT_EXECUTABLE  : path to git.exe
#   REPO_ROOT       : parent repository root
#   SUBMODULE_PATH  : gitlink path relative to REPO_ROOT
#   OUTPUT_FILE     : content-stable build-tree state file

foreach(required IN ITEMS GIT_EXECUTABLE REPO_ROOT SUBMODULE_PATH OUTPUT_FILE)
  if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
    message(FATAL_ERROR "${required} is required")
  endif()
endforeach()

execute_process(
  COMMAND "${GIT_EXECUTABLE}" rev-parse ":${SUBMODULE_PATH}"
  WORKING_DIRECTORY "${REPO_ROOT}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE expected_commit
  ERROR_VARIABLE stderr
  OUTPUT_STRIP_TRAILING_WHITESPACE
)

string(LENGTH "${expected_commit}" expected_commit_length)
if(NOT result EQUAL 0 OR
   NOT expected_commit MATCHES "^[0-9A-Fa-f]+$" OR
   NOT expected_commit_length EQUAL 40)
  message(FATAL_ERROR
    "Could not resolve the gitlink for ${SUBMODULE_PATH}: ${stderr}")
endif()

string(TOLOWER "${expected_commit}" expected_commit)
set(expected_content "${expected_commit}\n")
set(existing_content "")
if(EXISTS "${OUTPUT_FILE}")
  file(READ "${OUTPUT_FILE}" existing_content)
endif()

if(NOT existing_content STREQUAL expected_content)
  get_filename_component(output_directory "${OUTPUT_FILE}" DIRECTORY)
  file(MAKE_DIRECTORY "${output_directory}")
  file(WRITE "${OUTPUT_FILE}" "${expected_content}")
endif()
