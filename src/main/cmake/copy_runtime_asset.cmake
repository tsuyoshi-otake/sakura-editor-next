# Copy one runtime asset without changing the destination timestamp when the
# bytes are already identical.
#
# Parameters (passed via -D):
#   INPUT_FILE  : source asset
#   OUTPUT_FILE : staged asset

foreach(required IN ITEMS INPUT_FILE OUTPUT_FILE)
  if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
    message(FATAL_ERROR "${required} is required")
  endif()
endforeach()

if(NOT EXISTS "${INPUT_FILE}" OR IS_DIRECTORY "${INPUT_FILE}")
  message(FATAL_ERROR "Runtime asset input does not exist: ${INPUT_FILE}")
endif()
get_filename_component(resolved_input_file "${INPUT_FILE}" REALPATH)
if(NOT EXISTS "${resolved_input_file}" OR IS_DIRECTORY "${resolved_input_file}")
  message(FATAL_ERROR "Runtime asset input does not resolve to a file: ${INPUT_FILE}")
endif()

get_filename_component(output_directory "${OUTPUT_FILE}" DIRECTORY)
file(MAKE_DIRECTORY "${output_directory}")
file(SHA256 "${resolved_input_file}" input_hash)
set(copy_required TRUE)
if(EXISTS "${OUTPUT_FILE}")
  file(SHA256 "${OUTPUT_FILE}" output_hash)
  if(input_hash STREQUAL output_hash)
    set(copy_required FALSE)
  endif()
endif()
if(copy_required)
  set(output_temporary "${OUTPUT_FILE}.tmp")
  file(REMOVE "${output_temporary}")
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E copy "${resolved_input_file}" "${output_temporary}"
    RESULT_VARIABLE copy_result
    ERROR_VARIABLE copy_stderr
  )
  if(NOT copy_result EQUAL 0 OR NOT EXISTS "${output_temporary}")
    file(REMOVE "${output_temporary}")
    message(FATAL_ERROR
      "Could not stage runtime asset '${INPUT_FILE}' as '${OUTPUT_FILE}': ${copy_stderr}")
  endif()
  file(RENAME "${output_temporary}" "${OUTPUT_FILE}" RESULT rename_result)
  if(NOT rename_result STREQUAL "0")
    file(REMOVE "${output_temporary}")
    message(FATAL_ERROR
      "Could not publish runtime asset '${OUTPUT_FILE}': ${rename_result}")
  endif()
  file(SHA256 "${OUTPUT_FILE}" published_hash)
  if(NOT published_hash STREQUAL input_hash)
    message(FATAL_ERROR "Published runtime asset failed content verification: ${OUTPUT_FILE}")
  endif()
endif()
