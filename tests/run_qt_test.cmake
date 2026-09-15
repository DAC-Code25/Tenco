# Qt tests can lose console output when built as Windows GUI executables.
# Keep the original exit status and always expose the authoritative QtTest report.
execute_process(COMMAND "${TEST_EXECUTABLE}" -o "${TEST_REPORT},txt"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(EXISTS "${TEST_REPORT}")
    file(READ "${TEST_REPORT}" report)
    message("${report}")
endif()
if(NOT result EQUAL 0)
    message(FATAL_ERROR "QtTest exited ${result}\n${output}\n${errors}")
endif()
