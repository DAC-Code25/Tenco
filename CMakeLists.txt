cmake_minimum_required(VERSION 3.16)

project(Tenco LANGUAGES CXX)

# Keep parity with Tenco.pro (CONFIG += c++17)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Be explicit: ensure .ui/.qrc/.moc are generated even if the Qt helper macros
# are not enabling them in a given environment/QtCreator setup.
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# Qt 6 is the primary supported build for this repository.
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network WebSockets Core5Compat)

qt_standard_project_setup()

qt_add_executable(Tenco
    main.cpp
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

    map.h
    map.cpp

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

    configmanager.h
    configmanager.cpp

    maintenance.h
    maintenance.cpp

    help.h
    help.cpp

    about.h
    about.cpp

    resources.qrc
)

if(WIN32)
    # App icon (same as RC_FILE += app_icon.rc in qmake).
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

# Keep the runtime output directory consistent with qmake:
# DESTDIR = $$PWD/../bin  ->  <repo>/../bin
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

qt_finalize_executable(Tenco)
