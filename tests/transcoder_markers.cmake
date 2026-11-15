# Transcode INPUT and fail unless the output holds each byte sequence of HEX (marker segments
# the transcoder must carry over).  Invoked via:
#   cmake -DTRANS=<open_htj2k_transcode> -DINPUT=<file> -DWORK=<dir> "-DHEX=<hex>[;<hex>...]" -P transcoder_markers.cmake
file(MAKE_DIRECTORY "${WORK}")
execute_process(COMMAND ${TRANS} -i ${INPUT} -o ${WORK}/out.j2c RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "the transcode of ${INPUT} failed")
endif()
execute_process(COMMAND ${CMAKE_COMMAND} -DFILE=${WORK}/out.j2c "-DHEX=${HEX}"
                -P ${CMAKE_CURRENT_LIST_DIR}/check_file_hex.cmake RESULT_VARIABLE rc2)
if(NOT rc2 EQUAL 0)
  message(FATAL_ERROR "a marker segment of the input is missing from the output")
endif()
