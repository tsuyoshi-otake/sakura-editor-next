# Materialize runtime files from committed archives only when the archive
# content or extraction contract changes.  The state file is written only
# after every expected output exists, so failures remain observable and retryable.
#
# Parameters (passed via -D):
#   MODE                 : CTAGS or DIFFUTILS
#   SEVEN_ZIP_EXECUTABLE : path to 7z.exe
#   OUTPUT_DIRECTORY     : staging directory
#   STATE_FILE           : successful materialization state
#   BUILD_SIGNATURE      : hash of the extraction contract
#   CTAGS mode: ARCHIVE_FILE, OUTPUT_FILE
#   DIFFUTILS mode: ARCHIVE_FILE1, ARCHIVE_FILE2,
#                   DIFF_OUTPUT, INTL_OUTPUT, ICONV_OUTPUT

foreach(required IN ITEMS MODE SEVEN_ZIP_EXECUTABLE OUTPUT_DIRECTORY STATE_FILE BUILD_SIGNATURE)
  if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
    message(FATAL_ERROR "${required} is required")
  endif()
endforeach()

function(require_file variable_name)
  if(NOT DEFINED ${variable_name} OR "${${variable_name}}" STREQUAL "")
    message(FATAL_ERROR "${variable_name} is required for MODE=${MODE}")
  endif()
  if(NOT EXISTS "${${variable_name}}")
    message(FATAL_ERROR "Archive input does not exist: ${${variable_name}}")
  endif()
endfunction()

set(archive_files)
set(expected_outputs)
if(MODE STREQUAL "CTAGS")
  require_file(ARCHIVE_FILE)
  if(NOT DEFINED OUTPUT_FILE OR "${OUTPUT_FILE}" STREQUAL "")
    message(FATAL_ERROR "OUTPUT_FILE is required for MODE=CTAGS")
  endif()
  list(APPEND archive_files "${ARCHIVE_FILE}")
  list(APPEND expected_outputs "${OUTPUT_FILE}")
elseif(MODE STREQUAL "DIFFUTILS")
  require_file(ARCHIVE_FILE1)
  require_file(ARCHIVE_FILE2)
  foreach(output_variable IN ITEMS DIFF_OUTPUT INTL_OUTPUT ICONV_OUTPUT)
    if(NOT DEFINED ${output_variable} OR "${${output_variable}}" STREQUAL "")
      message(FATAL_ERROR "${output_variable} is required for MODE=DIFFUTILS")
    endif()
    list(APPEND expected_outputs "${${output_variable}}")
  endforeach()
  list(APPEND archive_files "${ARCHIVE_FILE1}" "${ARCHIVE_FILE2}")
else()
  message(FATAL_ERROR "Unsupported archive runtime MODE: ${MODE}")
endif()

file(SHA256 "${CMAKE_CURRENT_LIST_FILE}" runtime_script_hash)
set(expected_state_prefix "${MODE}\n${BUILD_SIGNATURE}\n${runtime_script_hash}\n")
foreach(archive IN LISTS archive_files)
  file(SHA256 "${archive}" archive_hash)
  string(APPEND expected_state_prefix "archive=${archive_hash}\n")
endforeach()

set(existing_state "")
if(EXISTS "${STATE_FILE}")
  file(READ "${STATE_FILE}" existing_state)
endif()
set(outputs_complete TRUE)
set(current_state "${expected_state_prefix}")
foreach(output IN LISTS expected_outputs)
  if(NOT EXISTS "${output}")
    set(outputs_complete FALSE)
  else()
    file(SHA256 "${output}" output_hash)
    string(APPEND current_state "output=${output_hash}\n")
  endif()
endforeach()
if(outputs_complete AND existing_state STREQUAL current_state)
  return()
endif()

file(MAKE_DIRECTORY "${OUTPUT_DIRECTORY}")
if(MODE STREQUAL "CTAGS")
  execute_process(
    COMMAND "${SEVEN_ZIP_EXECUTABLE}" e "${ARCHIVE_FILE}" "-o${OUTPUT_DIRECTORY}" -y ctags.exe
    RESULT_VARIABLE extract_result
    OUTPUT_VARIABLE extract_stdout
    ERROR_VARIABLE extract_stderr
  )
  if(NOT extract_result EQUAL 0)
    message(FATAL_ERROR
      "Could not extract ctags runtime: ${extract_stdout}\n${extract_stderr}")
  endif()
else()
  execute_process(
    COMMAND "${SEVEN_ZIP_EXECUTABLE}" e "${ARCHIVE_FILE1}" "-o${OUTPUT_DIRECTORY}" -y bin/diff.exe
    RESULT_VARIABLE binary_result
    OUTPUT_VARIABLE binary_stdout
    ERROR_VARIABLE binary_stderr
  )
  if(NOT binary_result EQUAL 0)
    message(FATAL_ERROR
      "Could not extract diff.exe: ${binary_stdout}\n${binary_stderr}")
  endif()
  execute_process(
    COMMAND "${SEVEN_ZIP_EXECUTABLE}" e "${ARCHIVE_FILE2}" "-o${OUTPUT_DIRECTORY}" -y bin/libintl3.dll bin/libiconv2.dll
    RESULT_VARIABLE dependency_result
    OUTPUT_VARIABLE dependency_stdout
    ERROR_VARIABLE dependency_stderr
  )
  if(NOT dependency_result EQUAL 0)
    message(FATAL_ERROR
      "Could not extract diffutils dependencies: ${dependency_stdout}\n${dependency_stderr}")
  endif()
endif()

foreach(output IN LISTS expected_outputs)
  if(NOT EXISTS "${output}")
    message(FATAL_ERROR "Archive extraction did not produce expected output: ${output}")
  endif()
endforeach()

set(published_state "${expected_state_prefix}")
foreach(output IN LISTS expected_outputs)
  file(SHA256 "${output}" output_hash)
  string(APPEND published_state "output=${output_hash}\n")
endforeach()

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
  message(FATAL_ERROR "Could not publish archive runtime state: ${STATE_FILE}")
endif()
