find_package(Qt6 REQUIRED COMPONENTS Core Test Network Sql)

qt_add_executable(tenco_test_routefollower
    test_routefollower.cpp
    ../routefollower.h
    ../routefollower.cpp
)
target_include_directories(tenco_test_routefollower PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_link_libraries(tenco_test_routefollower PRIVATE
    Qt6::Core
    Qt6::Test
)
add_test(NAME tenco_test_routefollower COMMAND tenco_test_routefollower)

qt_add_executable(tenco_test_motioncommandarbiter
    test_motioncommandarbiter.cpp
    ../motioncommandarbiter.h
    ../motioncommandarbiter.cpp
)
target_include_directories(tenco_test_motioncommandarbiter PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_link_libraries(tenco_test_motioncommandarbiter PRIVATE
    Qt6::Core
    Qt6::Test
)
add_test(NAME tenco_test_motioncommandarbiter COMMAND tenco_test_motioncommandarbiter)

qt_add_executable(tenco_test_routepathfinder
    test_routepathfinder.cpp
    ../routepathfinder.h
    ../routepathfinder.cpp
)
target_include_directories(tenco_test_routepathfinder PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_link_libraries(tenco_test_routepathfinder PRIVATE
    Qt6::Core
    Qt6::Test
)
add_test(NAME tenco_test_routepathfinder COMMAND tenco_test_routepathfinder)

qt_add_executable(tenco_test_mapdocument
    test_mapdocument.cpp
    ../mapdocument.h
    ../mapdocument.cpp
    ../rowmissiontypes.h
    ../rowmissiontypes.cpp
    ../rowworktypes.h
    ../rowworktypes.cpp
)
target_include_directories(tenco_test_mapdocument PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_link_libraries(tenco_test_mapdocument PRIVATE
    Qt6::Core
    Qt6::Test
)
add_test(NAME tenco_test_mapdocument COMMAND tenco_test_mapdocument)

qt_add_executable(tenco_test_rowworktypes
    test_rowworktypes.cpp
    ../rowworktypes.h
    ../rowworktypes.cpp
)
target_include_directories(tenco_test_rowworktypes PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_link_libraries(tenco_test_rowworktypes PRIVATE
    Qt6::Core
    Qt6::Test
)
add_test(NAME tenco_test_rowworktypes COMMAND tenco_test_rowworktypes)

qt_add_executable(tenco_test_rowmissiontypes
    test_rowmissiontypes.cpp
    ../rowmissiontypes.h
    ../rowmissiontypes.cpp
    ../rowworktypes.h
    ../rowworktypes.cpp
)
target_include_directories(tenco_test_rowmissiontypes PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_link_libraries(tenco_test_rowmissiontypes PRIVATE
    Qt6::Core
    Qt6::Test
)
add_test(NAME tenco_test_rowmissiontypes COMMAND tenco_test_rowmissiontypes)

qt_add_executable(tenco_test_configmanager
    test_configmanager.cpp
    ../configmanager.h
    ../configmanager.cpp
)
target_include_directories(tenco_test_configmanager PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_link_libraries(tenco_test_configmanager PRIVATE
    Qt6::Core
    Qt6::Test
)
add_test(NAME tenco_test_configmanager COMMAND tenco_test_configmanager)

qt_add_executable(tenco_test_databasemanager
    test_databasemanager.cpp
    ../databasemanager.h
    ../databasemanager.cpp
    ../configmanager.h
    ../configmanager.cpp
)
target_include_directories(tenco_test_databasemanager PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_link_libraries(tenco_test_databasemanager PRIVATE
    Qt6::Core
    Qt6::Sql
    Qt6::Test
)
add_test(NAME tenco_test_databasemanager COMMAND tenco_test_databasemanager)

qt_add_executable(tenco_test_statusprotocol
    test_statusprotocol.cpp
    ../statusprotocol.h
    ../statusprotocol.cpp
)
target_include_directories(tenco_test_statusprotocol PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_link_libraries(tenco_test_statusprotocol PRIVATE
    Qt6::Core
    Qt6::Test
)
add_test(NAME tenco_test_statusprotocol COMMAND tenco_test_statusprotocol)

qt_add_executable(tenco_test_videoframeworker
    test_videoframeworker.cpp
    ../videoframeworker.h
    ../videoframeworker.cpp
)
target_include_directories(tenco_test_videoframeworker PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_link_libraries(tenco_test_videoframeworker PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Test
)
add_test(NAME tenco_test_videoframeworker COMMAND tenco_test_videoframeworker)

qt_add_executable(tenco_test_map_route_integration
    test_map_route_integration.cpp
    ../map.h
    ../map.cpp
    ../rowmissiontypes.h
    ../rowmissiontypes.cpp
    ../rowworktypes.h
    ../rowworktypes.cpp
    ../rowworkclient.h
    ../rowworkclient.cpp
    ../mapgraphicsview.h
    ../mapgraphicsview.cpp
    ../mapdocument.h
    ../mapdocument.cpp
    ../routepathfinder.h
    ../routepathfinder.cpp
    ../configmanager.h
    ../configmanager.cpp
    ../loggingmanager.h
    ../loggingmanager.cpp
    ../battery.h
    ../battery.cpp
    ../imageswitch.h
    ../imageswitch.cpp
    ../mainwindow.ui
)
set_property(TARGET tenco_test_map_route_integration PROPERTY
    AUTOUIC_SEARCH_PATHS "${CMAKE_CURRENT_SOURCE_DIR}/.."
)
target_include_directories(tenco_test_map_route_integration PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_link_libraries(tenco_test_map_route_integration PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    Qt6::Test
)
add_test(NAME tenco_test_map_route_integration COMMAND tenco_test_map_route_integration)
