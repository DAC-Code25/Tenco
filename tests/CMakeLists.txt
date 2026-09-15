find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Test Network Sql)

qt_add_executable(tenco_ipc_client_probe ipc_client_probe.cpp ../controlsessioncoordinator.h ../controlsessioncoordinator.cpp
    ../externaleventcoordinator.h ../externaleventcoordinator.cpp)
set_target_properties(tenco_ipc_client_probe PROPERTIES WIN32_EXECUTABLE FALSE)
target_include_directories(tenco_ipc_client_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(tenco_ipc_client_probe PRIVATE tenco_network tenco_motion tenco_map_model)

function(tenco_add_test target_name)
    qt_add_executable(${target_name}
        ${ARGN}
    )
    target_include_directories(${target_name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/..
    )
    target_link_libraries(${target_name} PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME ${target_name} COMMAND ${target_name})
endfunction()

tenco_add_test(tenco_test_taskcompiler test_taskcompiler.cpp)
target_link_libraries(tenco_test_taskcompiler PRIVATE tenco_map_model)

tenco_add_test(tenco_test_ipc_clients test_ipc_clients.cpp ../externaleventcoordinator.h ../externaleventcoordinator.cpp)
set_target_properties(tenco_test_ipc_clients PROPERTIES WIN32_EXECUTABLE FALSE)
target_link_libraries(tenco_test_ipc_clients PRIVATE tenco_network Qt6::WebSockets)


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

tenco_add_test(tenco_test_rowmissiontypes
    test_rowmissiontypes.cpp
)
target_link_libraries(tenco_test_rowmissiontypes PRIVATE tenco_map_model)

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
