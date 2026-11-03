# Desktop launch must work without Qt Creator or a developer PATH. Use the
# selected kit and let windeployqt inspect the executable's actual Qt imports;
# a Debug MinGW application can still link Qt DLLs without a debug suffix.
foreach(required TENCO_RUNTIME_EXECUTABLE TENCO_QT_BIN TENCO_COMPILER_BIN TENCO_QML_DIRECTORY)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "Windows runtime deployment requires ${required}")
    endif()
endforeach()
if(NOT EXISTS "${TENCO_RUNTIME_EXECUTABLE}" OR
   NOT EXISTS "${TENCO_QT_BIN}/windeployqt.exe" OR
   NOT EXISTS "${TENCO_QT_BIN}/qtpaths.exe" OR
   NOT IS_DIRECTORY "${TENCO_QML_DIRECTORY}")
    message(FATAL_ERROR "Windows runtime deployment input is missing")
endif()
set(ENV{PATH} "${TENCO_QT_BIN};${TENCO_COMPILER_BIN};$ENV{PATH}")
execute_process(
    COMMAND "${TENCO_QT_BIN}/windeployqt.exe"
        --qtpaths "${TENCO_QT_BIN}/qtpaths.exe"
        --qmldir "${TENCO_QML_DIRECTORY}"
        --compiler-runtime --no-translations --verbose 0
        "${TENCO_RUNTIME_EXECUTABLE}"
    RESULT_VARIABLE deploy_result
)
if(NOT deploy_result EQUAL 0)
    message(FATAL_ERROR "Windows runtime deployment failed: ${deploy_result}")
endif()
