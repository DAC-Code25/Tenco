cmake_minimum_required(VERSION 3.16)

project(Tenco LANGUAGES CXX)
include(CTest)

# Project-wide C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Be explicit: ensure .ui/.qrc/.moc are generated even if the Qt helper macros
# are not enabling them in a given environment/QtCreator setup.
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

option(TENCO_ENABLE_WARNINGS "Enable stricter compiler warnings" ON)
option(TENCO_BUILD_TESTS "Build unit tests" ON)

# Qt 6 is the primary supported build for this repository.
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network WebSockets Core5Compat)

qt_standard_project_setup()

qt_add_executable(Tenco
    main.cpp
    loggingmanager.h
    loggingmanager.cpp
    UI_Design.cpp

    mainwindow.h
    mainwindow.cpp
    mainwindow.ui

    battery.h
    battery.cpp

    imageswitch.h
    imageswitch.cpp

    home.h
    home.cpp

    home_status_presenter.h
    home_status_presenter.cpp

    map.h
    map.cpp

    mapdocument.h
    mapdocument.cpp

    mapgraphicsview.h
    mapgraphicsview.cpp

    homenetworkworker.h
    homenetworkworker.cpp

    statusclient.h
    statusclient.cpp

    chassisclient.h
    chassisclient.cpp

    videoclient.h
    videoclient.cpp

    routefollower.h
    routefollower.cpp

    routepathfinder.h
    routepathfinder.cpp

    configmanager.h
    configmanager.cpp

    statusprotocol.h
    statusprotocol.cpp

    maintenance.h
    maintenance.cpp

    help.h
    help.cpp

    about.h
    about.cpp

    resources.qrc
)

if(WIN32)
    # App icon resource for Windows builds.
    target_sources(Tenco PRIVATE app_icon.rc)
    set_target_properties(Tenco PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

target_link_libraries(Tenco PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    Qt6::WebSockets
    Qt6::Core5Compat
)

target_include_directories(Tenco PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(TENCO_ENABLE_WARNINGS)
    if(MSVC)
        target_compile_options(Tenco PRIVATE /W4 /permissive- /wd4100 /wd4244 /wd4267)
    else()
        target_compile_options(Tenco PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

# Keep a stable runtime output directory for local runs and deployment scripts.
# Output path: <repo>/../bin
set(_tenco_bin_dir "${CMAKE_CURRENT_SOURCE_DIR}/../bin")
set_target_properties(Tenco PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${_tenco_bin_dir}"
)
foreach(_cfg Debug Release RelWithDebInfo MinSizeRel)
    set_target_properties(Tenco PROPERTIES
        "RUNTIME_OUTPUT_DIRECTORY_${_cfg}" "${_tenco_bin_dir}"
    )
endforeach()

# Convenience for dev runs: ensure config.json is discoverable via
# QCoreApplication::applicationDirPath() (see ConfigManager::defaultConfigPath()).
add_custom_command(TARGET Tenco POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/config.json"
            "$<TARGET_FILE_DIR:Tenco>/config.json"
    COMMENT "Copy config.json next to the executable"
)

include(GNUInstallDirs)
install(TARGETS Tenco
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/config.json"
    DESTINATION ${CMAKE_INSTALL_BINDIR}
)

set(CPACK_PACKAGE_NAME "Tenco")
set(CPACK_PACKAGE_VENDOR "Tenco")
set(CPACK_PACKAGE_VERSION "1.0.0")
set(CPACK_GENERATOR "ZIP")
include(CPack)

if(BUILD_TESTING AND TENCO_BUILD_TESTS)
    add_subdirectory(tests)
endif()
