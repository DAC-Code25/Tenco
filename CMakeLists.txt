cmake_minimum_required(VERSION 3.16)

project(Tenco LANGUAGES CXX)
include(CTest)

# Project-wide C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Be explicit: ensure .ui/.qrc/.moc are generated even if the Qt helper macros
# are not enabling them in a given environment/QtCreator setup.
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

option(TENCO_ENABLE_WARNINGS "Enable stricter compiler warnings" ON)
option(TENCO_BUILD_TESTS "Build unit tests" ON)
option(TENCO_ENABLE_OAK_CAMERA "Enable DepthAI/OpenCV OAK camera backend" OFF)
option(TENCO_ENABLE_ABOUT_3D "Enable the interactive 3D model viewer on the About page when Qt Quick3D is available" ON)

# Qt 6 is the primary supported build for this repository.
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network WebSockets Sql)
if(TENCO_ENABLE_OAK_CAMERA)
    find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs videoio)
    find_package(depthai CONFIG REQUIRED)
endif()

set(TENCO_ABOUT_3D_AVAILABLE OFF)
if(TENCO_ENABLE_ABOUT_3D)
    find_package(Qt6 QUIET COMPONENTS Quick QuickWidgets Quick3D Quick3DAssetUtils)
    if(TARGET Qt6::Quick AND TARGET Qt6::QuickWidgets AND TARGET Qt6::Quick3D AND TARGET Qt6::Quick3DAssetUtils)
        set(TENCO_ABOUT_3D_AVAILABLE ON)
    endif()
endif()

qt_standard_project_setup()

function(tenco_configure_target target_name)
    target_include_directories(${target_name} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    if(TENCO_ENABLE_WARNINGS)
        if(MSVC)
            target_compile_options(${target_name} PRIVATE /W4 /permissive- /wd4100 /wd4244 /wd4267)
        else()
            target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wpedantic)
        endif()
    endif()
endfunction()

qt_add_library(tenco_config STATIC
    configmanager.h
    configmanager.cpp
)
target_link_libraries(tenco_config PUBLIC Qt6::Core)
tenco_configure_target(tenco_config)

qt_add_library(tenco_logging STATIC
    loggingmanager.h
    loggingmanager.cpp
)
target_link_libraries(tenco_logging PUBLIC Qt6::Core tenco_config)
tenco_configure_target(tenco_logging)

qt_add_library(tenco_storage STATIC
    databasemanager.h
    databasemanager.cpp
)
target_link_libraries(tenco_storage PUBLIC Qt6::Core Qt6::Sql tenco_config)
tenco_configure_target(tenco_storage)

qt_add_library(tenco_motion STATIC
    routefollower.h
    routefollower.cpp
    motioncommandarbiter.h
    motioncommandarbiter.cpp
    routepathfinder.h
    routepathfinder.cpp
)
target_link_libraries(tenco_motion PUBLIC Qt6::Core)
tenco_configure_target(tenco_motion)

qt_add_library(tenco_map_model STATIC
    rowworktypes.h
    rowworktypes.cpp
    rowmissiontypes.h
    rowmissiontypes.cpp
    mapdocument.h
    mapdocument.cpp
    maprouteplanner.h
    maprouteplanner.cpp
)
target_link_libraries(tenco_map_model PUBLIC Qt6::Core tenco_config tenco_motion)
tenco_configure_target(tenco_map_model)

qt_add_library(tenco_network STATIC
    networkpolicy.h
    networkpolicy.cpp
    cameracontrolclient.h
    cameracontrolclient.cpp
    gimbalcontrolclient.h
    gimbalcontrolclient.cpp
    rowworkclient.h
    rowworkclient.cpp
    homenetworkworker.h
    homenetworkworker.cpp
    statusclient.h
    statusclient.cpp
    chassisclient.h
    chassisclient.cpp
    statusprotocol.h
    statusprotocol.cpp
)
target_link_libraries(tenco_network PUBLIC
    Qt6::Core
    Qt6::Network
    Qt6::WebSockets
    tenco_map_model
)
tenco_configure_target(tenco_network)

qt_add_library(tenco_video STATIC
    abstractvideosource.h
    abstractvideosource.cpp
    mjpegvideosource.h
    mjpegvideosource.cpp
    oakcameravideosource.h
    oakcameravideosource.cpp
    videoclient.h
    videoclient.cpp
    videoframeworker.h
    videoframeworker.cpp
)
target_link_libraries(tenco_video PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    tenco_config
)
tenco_configure_target(tenco_video)

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
    homecontrolcoordinator.h
    homecontrolcoordinator.cpp
    home_status_presenter.h
    home_status_presenter.cpp
    homevideopresenter.h
    homevideopresenter.cpp

    map.h
    map.cpp
    mapgraphicsview.h
    mapgraphicsview.cpp

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
    Qt6::Sql
    tenco_config
    tenco_logging
    tenco_storage
    tenco_motion
    tenco_map_model
    tenco_network
    tenco_video
)

if(TENCO_ENABLE_OAK_CAMERA)
    target_compile_definitions(tenco_video PUBLIC TENCO_ENABLE_OAK_CAMERA)
    target_link_libraries(tenco_video PUBLIC
        ${OpenCV_LIBS}
        depthai::core
    )
endif()

if(TENCO_ABOUT_3D_AVAILABLE)
    target_compile_definitions(Tenco PRIVATE TENCO_ENABLE_ABOUT_3D)
    target_link_libraries(Tenco PRIVATE
        Qt6::Quick
        Qt6::QuickWidgets
        Qt6::Quick3D
        Qt6::Quick3DAssetUtils
    )
endif()

tenco_configure_target(Tenco)

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

    add_custom_command(TARGET Tenco POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:Tenco>/models"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/src/carmodel_about_centered.glb"
                "$<TARGET_FILE_DIR:Tenco>/models/carmodel_about_centered.glb"
        COMMENT "Copy about-page 3D model next to the executable"
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
