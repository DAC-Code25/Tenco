find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Test Network Sql)


qt_add_executable(tenco_ipc_client_probe ipc_client_probe.cpp navigation_defaults_probe.cpp ../controlsessioncoordinator.h ../controlsessioncoordinator.cpp
    ../externaleventcoordinator.h ../externaleventcoordinator.cpp
    ../map.h ../map.cpp ../mapgraphicsview.h ../mapgraphicsview.cpp
    ../battery.h ../battery.cpp ../imageswitch.h ../imageswitch.cpp ../mainwindow.ui)
set_property(TARGET tenco_ipc_client_probe PROPERTY AUTOUIC_SEARCH_PATHS "${CMAKE_CURRENT_SOURCE_DIR}/..")
set_target_properties(tenco_ipc_client_probe PROPERTIES WIN32_EXECUTABLE FALSE)
target_include_directories(tenco_ipc_client_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(tenco_ipc_client_probe PRIVATE Qt6::Widgets tenco_network tenco_motion tenco_map_model tenco_logging tenco_config)

function(tenco_add_test target_name)
    qt_add_executable(${target_name}
        ${ARGN}
    )
    target_include_directories(${target_name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/..
    )
    target_link_libraries(${target_name} PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME ${target_name} COMMAND ${CMAKE_COMMAND}
        "-DTEST_EXECUTABLE=$<TARGET_FILE:${target_name}>"
        "-DTEST_REPORT=${CMAKE_CURRENT_BINARY_DIR}/${target_name}.txt"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run_qt_test.cmake")
endfunction()

tenco_add_test(tenco_test_taskcompiler test_taskcompiler.cpp)
target_link_libraries(tenco_test_taskcompiler PRIVATE tenco_map_model)

tenco_add_test(tenco_test_ipc_clients test_ipc_clients.cpp ../externaleventcoordinator.h ../externaleventcoordinator.cpp
    ../controlsessioncoordinator.h ../controlsessioncoordinator.cpp)
set_target_properties(tenco_test_ipc_clients PROPERTIES WIN32_EXECUTABLE FALSE)
target_link_libraries(tenco_test_ipc_clients PRIVATE tenco_network Qt6::WebSockets)

tenco_add_test(tenco_test_jsonhttpclient test_jsonhttpclient.cpp)
target_link_libraries(tenco_test_jsonhttpclient PRIVATE tenco_network Qt6::Network)

tenco_add_test(tenco_test_homenetworkworker test_homenetworkworker.cpp)
target_link_libraries(tenco_test_homenetworkworker PRIVATE tenco_network Qt6::Network)

tenco_add_test(tenco_test_pose_wireless test_pose_wireless.cpp)
target_link_libraries(tenco_test_pose_wireless PRIVATE tenco_network Qt6::Network)

tenco_add_test(tenco_test_chassis_queue test_chassis_queue.cpp)
target_link_libraries(tenco_test_chassis_queue PRIVATE tenco_network tenco_motion Qt6::WebSockets)


tenco_add_test(tenco_test_motioncommandarbiter
    test_motioncommandarbiter.cpp
)
target_link_libraries(tenco_test_motioncommandarbiter PRIVATE tenco_motion)

tenco_add_test(tenco_test_routepathfinder
    test_routepathfinder.cpp
)
target_link_libraries(tenco_test_routepathfinder PRIVATE tenco_motion)

tenco_add_test(tenco_test_maprouteplanner
    test_maprouteplanner.cpp
)
target_link_libraries(tenco_test_maprouteplanner PRIVATE tenco_map_model)

tenco_add_test(tenco_test_mapgeometry
    test_mapgeometry.cpp
)
target_link_libraries(tenco_test_mapgeometry PRIVATE tenco_map_model)

tenco_add_test(tenco_test_mapdocument
    test_mapdocument.cpp
)
target_link_libraries(tenco_test_mapdocument PRIVATE tenco_map_model)

tenco_add_test(tenco_test_rowworktypes
    test_rowworktypes.cpp
)
target_link_libraries(tenco_test_rowworktypes PRIVATE tenco_map_model)

tenco_add_test(tenco_test_homevideopresenter
    test_homevideopresenter.cpp
    ../homevideopresenter.cpp
)
target_link_libraries(tenco_test_homevideopresenter PRIVATE Qt6::Widgets)

tenco_add_test(tenco_test_configmanager
    test_configmanager.cpp
)
target_link_libraries(tenco_test_configmanager PRIVATE tenco_config)

tenco_add_test(tenco_test_databasemanager
    test_databasemanager.cpp
)
target_link_libraries(tenco_test_databasemanager PRIVATE Qt6::Sql tenco_storage)

tenco_add_test(tenco_test_statusprotocol
    test_statusprotocol.cpp
)
target_link_libraries(tenco_test_statusprotocol PRIVATE tenco_network)

tenco_add_test(tenco_test_videoframeworker
    test_videoframeworker.cpp
)
target_link_libraries(tenco_test_videoframeworker PRIVATE Qt6::Gui tenco_video)

tenco_add_test(tenco_test_homeinputstate
    test_homeinputstate.cpp
    ../homegimbalkeystate.h
    ../homegimbalkeystate.cpp
    ../homemanualinputstate.h
    ../homemanualinputstate.cpp
)

tenco_add_test(tenco_test_map_route_integration
    test_map_route_integration.cpp
    ../controlsessioncoordinator.h
    ../controlsessioncoordinator.cpp
    ../map.h
    ../map.cpp
    ../mapgraphicsview.h
    ../mapgraphicsview.cpp
    ../battery.h
    ../battery.cpp
    ../imageswitch.h
    ../imageswitch.cpp
    ../mainwindow.ui
)
set_property(TARGET tenco_test_map_route_integration PROPERTY
    AUTOUIC_SEARCH_PATHS "${CMAKE_CURRENT_SOURCE_DIR}/.."
)
target_link_libraries(tenco_test_map_route_integration PRIVATE
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    tenco_config
    tenco_logging
    tenco_motion
    tenco_map_model
    tenco_network
)

tenco_add_test(tenco_test_pose_map_chain
    test_pose_map_chain.cpp
    ../controlsessioncoordinator.h ../controlsessioncoordinator.cpp
    ../map.h ../map.cpp ../mapgraphicsview.h ../mapgraphicsview.cpp
    ../battery.h ../battery.cpp ../imageswitch.h ../imageswitch.cpp ../mainwindow.ui)
set_property(TARGET tenco_test_pose_map_chain PROPERTY AUTOUIC_SEARCH_PATHS "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(tenco_test_pose_map_chain PRIVATE Qt6::Widgets Qt6::Network
    tenco_config tenco_logging tenco_motion tenco_map_model tenco_network)

tenco_add_test(tenco_test_camera_settings test_camera_settings.cpp)
target_link_libraries(tenco_test_camera_settings PRIVATE tenco_camera_settings Qt6::Widgets)

tenco_add_test(tenco_test_navigation_diagnostics test_navigation_diagnostics.cpp)
target_link_libraries(tenco_test_navigation_diagnostics PRIVATE tenco_navigation_diagnostics Qt6::Widgets)

tenco_add_test(tenco_test_ipc_management test_ipc_management.cpp)
target_link_libraries(tenco_test_ipc_management PRIVATE tenco_ipc_management Qt6::Widgets)

tenco_add_test(tenco_test_loggingmanager test_loggingmanager.cpp)
target_link_libraries(tenco_test_loggingmanager PRIVATE tenco_logging)

tenco_add_test(tenco_test_parameterwheelguard test_parameterwheelguard.cpp)
target_link_libraries(tenco_test_parameterwheelguard PRIVATE Qt6::Widgets)

tenco_add_test(tenco_test_buttonfocusstyle test_buttonfocusstyle.cpp)
target_link_libraries(tenco_test_buttonfocusstyle PRIVATE Qt6::Widgets)

add_test(NAME tenco_test_runtime_config_preservation COMMAND ${CMAKE_COMMAND}
    "-DTENCO_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/.."
    "-DTENCO_BUILD_DIR=${CMAKE_BINARY_DIR}"
    "-DTENCO_TEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/runtime-config-regression"
    "-DTENCO_INSTALL_BINDIR=${CMAKE_INSTALL_BINDIR}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/test_runtime_config_preservation.cmake")

tenco_add_test(tenco_test_homepreview
    test_homepreview.cpp
    ../home.h ../home.cpp
    ../homegimbalkeystate.h ../homegimbalkeystate.cpp
    ../homemanualinputstate.h ../homemanualinputstate.cpp
    ../home_status_presenter.h ../home_status_presenter.cpp
    ../homevideopresenter.h ../homevideopresenter.cpp
    ../battery.h ../battery.cpp ../imageswitch.h ../imageswitch.cpp ../mainwindow.ui)
set_property(TARGET tenco_test_homepreview PROPERTY AUTOUIC_SEARCH_PATHS "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(tenco_test_homepreview PRIVATE Qt6::Widgets tenco_video tenco_network
    tenco_motion tenco_logging tenco_config)
