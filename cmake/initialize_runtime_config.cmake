# Runtime configuration belongs to the operator. Builds and installations may
# seed a new directory, but must retain an existing configuration byte for byte.
if(NOT DEFINED TENCO_CONFIG_TEMPLATE OR NOT DEFINED TENCO_RUNTIME_CONFIG OR
   TENCO_CONFIG_TEMPLATE STREQUAL "" OR TENCO_RUNTIME_CONFIG STREQUAL "")
    message(FATAL_ERROR "Runtime configuration initialization requires source and destination paths")
endif()

if(IS_DIRECTORY "${TENCO_RUNTIME_CONFIG}")
    message(FATAL_ERROR "Runtime configuration destination is a directory")
endif()
if(EXISTS "${TENCO_RUNTIME_CONFIG}" OR IS_SYMLINK "${TENCO_RUNTIME_CONFIG}")
    return()
endif()
if(NOT EXISTS "${TENCO_CONFIG_TEMPLATE}" OR IS_DIRECTORY "${TENCO_CONFIG_TEMPLATE}")
    message(FATAL_ERROR "Runtime configuration template is missing or invalid")
endif()

get_filename_component(_tenco_config_directory "${TENCO_RUNTIME_CONFIG}" DIRECTORY)
file(MAKE_DIRECTORY "${_tenco_config_directory}")
configure_file("${TENCO_CONFIG_TEMPLATE}" "${TENCO_RUNTIME_CONFIG}" COPYONLY)
