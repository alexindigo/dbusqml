cmake_minimum_required(VERSION 3.16)
project(dbusqml LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Qml DBus)
qt_policy(SET QTP0001 NEW)

include(GNUInstallDirs)

# Production sources
set(DBUSQML_SOURCES
    dbusadaptor.cpp
    dbusadaptor.h
    dbuscatalog.cpp
    dbuscatalog.h
    dbusconnection.cpp
    dbusconnection.h
    dbuserror.h
    dbusmessage.cpp
    dbusmessage.h
    dbus.cpp
    dbus.h
    dbuspendingreply.cpp
    dbuspendingreply.h
    dbustypes.h
    dbustypes.cpp
)

# ==================== QML MODULE ====================
qt_add_qml_module(dbusqml
    URI DBus
    VERSION 1.0
    OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/DBus
    SOURCES
        ${DBUSQML_SOURCES}
        dbusplugin.cpp
)

target_link_libraries(dbusqml PRIVATE
    Qt6::Core Qt6::Qml Qt6::DBus
)

qt_add_resources(dbusqml "catalog"
    PREFIX "/dbusqml"
    FILES
        types/org.mpris.MediaPlayer2.xml
        types/org.mpris.MediaPlayer2.Player.xml
        types/org.freedesktop.Notifications.xml
        types/org.freedesktop.ScreenSaver.xml
        types/org.freedesktop.login1.Manager.xml
        types/org.freedesktop.portal.Settings.xml
        types/org.freedesktop.portal.NetworkMonitor.xml
        types/org.freedesktop.UPower.xml
)

# ==================== INSTALLATION ====================
install(TARGETS dbusqml
    DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/DBus/
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/qt6/qml/DBus
)

install(FILES
    dbus.h
    dbusconnection.h
    dbusmessage.h
    dbuserror.h
    dbuspendingreply.h
    dbustypes.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/dbusqml
)

install(DIRECTORY types/
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/dbusqml/types/
        FILES_MATCHING PATTERN "*.xml")

# ==================== TESTS ====================
option(BUILD_TEST_MODE "Build the C++ test executables" ON)

if(BUILD_TEST_MODE)
    find_package(Qt6 REQUIRED COMPONENTS Test)

    add_executable(test_types
        tests/test_dbustypes.cpp
        dbusmessage.cpp
        dbuserror.h
        dbustypes.h
        dbustypes.cpp
    )
    target_include_directories(test_types PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(test_types PRIVATE Qt6::Core Qt6::Qml Qt6::Test Qt6::DBus)

    add_executable(test_dbus
        tests/test_dbusconnection.cpp
        dbusmessage.cpp
        dbusconnection.cpp
        dbuspendingreply.cpp
        dbustypes.cpp
    )
    target_include_directories(test_dbus PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(test_dbus PRIVATE Qt6::Core Qt6::Qml Qt6::Test Qt6::DBus dbusqml)

    enable_testing()
    add_test(NAME test_types COMMAND test_types)
    add_test(NAME test_dbus COMMAND dbus-run-session -- $<TARGET_FILE:test_dbus>)
    set_tests_properties(test_dbus PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )
endif()

option(ENABLE_COVERAGE "Instrument targets for code coverage collection" OFF)

if(ENABLE_COVERAGE)
    message(STATUS "Enabling code coverage flags")

    target_compile_options(dbusqml PRIVATE --coverage -fprofile-abs-path)
    target_link_options(dbusqml PRIVATE --coverage -fprofile-abs-path)

    if(TARGET test_types)
        target_compile_options(test_types PRIVATE --coverage -fprofile-abs-path)
        target_link_options(test_types PRIVATE --coverage -fprofile-abs-path)
        target_link_libraries(test_types PRIVATE dbusqml)
    endif()
    if(TARGET test_dbus)
        target_compile_options(test_dbus PRIVATE --coverage -fprofile-abs-path)
        target_link_options(test_dbus PRIVATE --coverage -fprofile-abs-path)
        target_link_libraries(test_dbus PRIVATE dbusqml)
    endif()
endif()


