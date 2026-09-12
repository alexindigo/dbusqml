cmake_minimum_required(VERSION 3.16)
project(dbusqml
    VERSION 0.9.1
    DESCRIPTION "Standalone D-Bus binding for QML"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Qml DBus)
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
    dbusheldreply.cpp
    dbusheldreply.h
    dbusmessage.cpp
    dbusmessage.h
    dbuspathdispatcher.cpp
    dbuspathdispatcher.h
    dbus.cpp
    dbus.h
    dbusintrospection.cpp
    dbusintrospection.h
    dbuspendingreply.cpp
    dbuspendingreply.h
    dbustypes.h
    dbustypes.cpp
    dbusutils.h
    dbusutils.cpp
)

# ==================== QML MODULE ====================
qt_add_qml_module(dbusqml
    URI DBus
    VERSION 1.0
    OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/DBus
    SOURCES
        ${DBUSQML_SOURCES}
        dbusplugin.cpp
        dbussignalwatcher.cpp
        dbussignalwatcher.h
        dbusservicewatcher.cpp
        dbusservicewatcher.h
        dbusobjectmanager.cpp
        dbusobjectmanager.h
)

target_link_libraries(dbusqml PRIVATE
    Qt6::Core Qt6::Qml Qt6::DBus
)

qt_add_resources(dbusqml "catalog"
    PREFIX "/dbusqml"
    FILES
        types/org.freedesktop.NetworkManager.xml
        types/org.freedesktop.NetworkManager.AccessPoint.xml
        types/org.freedesktop.NetworkManager.Connection.Active.xml
        types/org.freedesktop.NetworkManager.Device.xml
        types/org.freedesktop.NetworkManager.Device.Wired.xml
        types/org.freedesktop.NetworkManager.Device.Wireless.xml
        types/org.freedesktop.NetworkManager.IP4Config.xml
        types/org.freedesktop.NetworkManager.IP6Config.xml
        types/org.freedesktop.NetworkManager.Settings.xml
        types/org.freedesktop.NetworkManager.Settings.Connection.xml
        types/org.mpris.MediaPlayer2.xml
        types/org.mpris.MediaPlayer2.Player.xml
        types/org.freedesktop.Notifications.xml
        types/org.freedesktop.ScreenSaver.xml
        types/org.freedesktop.login1.Manager.xml
        types/org.freedesktop.portal.Settings.xml
        types/org.freedesktop.impl.portal.Settings.xml
        types/org.freedesktop.impl.portal.FileChooser.xml
        types/org.freedesktop.impl.portal.Inhibit.xml
        types/org.freedesktop.impl.portal.Request.xml
        types/org.freedesktop.portal.NetworkMonitor.xml
        types/org.freedesktop.UPower.xml
)

# ==================== INSTALLATION ====================
install(TARGETS dbusqml
    EXPORT dbusqmlTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
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
    dbusutils.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/dbusqml
)

install(DIRECTORY types/
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/dbusqml/types/
        FILES_MATCHING PATTERN "*.xml")

# ==================== CMAKE PACKAGE CONFIG ====================
# Lets downstream projects do:
#   find_package(dbusqml 0.1 REQUIRED)
#   target_link_libraries(app PRIVATE dbusqml::dbusqml)

include(CMakePackageConfigHelpers)

set(DBUSQML_CMAKE_INSTALL_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/dbusqml)

install(EXPORT dbusqmlTargets
    FILE dbusqmlTargets.cmake
    NAMESPACE dbusqml::
    DESTINATION ${DBUSQML_CMAKE_INSTALL_DIR}
)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/dbusqmlConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/dbusqmlConfig.cmake
    INSTALL_DESTINATION ${DBUSQML_CMAKE_INSTALL_DIR}
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/dbusqmlConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMinorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/dbusqmlConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/dbusqmlConfigVersion.cmake
    DESTINATION ${DBUSQML_CMAKE_INSTALL_DIR}
)

# ==================== TESTS ====================
option(BUILD_TEST_MODE "Build the C++ test executables" ON)

# L2 (ledger-zero): test-link hygiene. Test binaries must reference each
# library symbol exactly once — compiling library .cpps into the test
# target while ALSO linking libdbusqml.so produces duplicate
# staticMetaObject definitions (ASan ODR-violation, previously masked
# with detect_odr_violation=0). Tests therefore link the library and
# include only headers; the single exception is test_types, which
# exercises header-adjacent marshal helpers without the QML module.
if(BUILD_TEST_MODE)
    find_package(Qt6 6.8 REQUIRED COMPONENTS Test)

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
    )
    target_include_directories(test_dbus PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(test_dbus PRIVATE Qt6::Core Qt6::Qml Qt6::Test Qt6::DBus dbusqml)

    add_executable(test_introspect
        tests/test_introspection.cpp
        dbusintrospection.cpp
    )
    target_include_directories(test_introspect PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(test_introspect PRIVATE Qt6::Core Qt6::Test)

    add_executable(test_adaptor
        tests/test_dbusadaptor.cpp
    )
    target_include_directories(test_adaptor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(test_adaptor PRIVATE Qt6::Core Qt6::Qml Qt6::Test Qt6::DBus dbusqml)

    enable_testing()
    add_executable(fd_helper_service tests/fd_helper_service.cpp
    )
    target_include_directories(fd_helper_service PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(fd_helper_service PRIVATE Qt6::Core Qt6::Qml Qt6::DBus dbusqml)

    add_executable(payload_echo_service tests/payload_echo_service.cpp
    )
    target_include_directories(payload_echo_service PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(payload_echo_service PRIVATE Qt6::Core Qt6::Qml Qt6::DBus dbusqml)

    # TSan canary (road-to-one C3): deliberately racy binary the sanitizer
    # CI job runs FIRST, requiring the detection grep to find its warning.
    # Non-default target — built on demand, never part of ALL.
    add_executable(tsan_canary EXCLUDE_FROM_ALL tests/tsan_canary.cpp)
    set_target_properties(tsan_canary PROPERTIES AUTOMOC OFF)
    find_package(Threads REQUIRED)
    target_link_libraries(tsan_canary PRIVATE Threads::Threads)

    # F-amended gate canary battery (tsan-classifier-gate cycle): the
    # classifier owns the verdict; each canary asserts its EXPECTED
    # verdict through it. All EXCLUDE_FROM_ALL — built on demand only.
    #   tsan_canary_mixed:  instrumented TU races the helper DSO's
    #     intercepted memmove (the spike's P2a shape — the race the
    #     rejected ignore_noninstrumented_modules=1 flag swallowed).
    #     Expected verdict: OURS -> FAIL.
    #   tsan_canary_foreign: bare QDBusConnection::connectToBus
    #     worker-spawn (the registered bus-bind class) on pure QtDBus —
    #     NO libdbusqml, so no family variant can put our frames in an
    #     attribution stack (question-1 ruling, tsan-register-tightening).
    #     Expected verdict: FOREIGN-registered, counted, non-failing.
    add_library(tsan_canary_uninst SHARED EXCLUDE_FROM_ALL tests/tsan_canary_uninst.cpp)
    set_target_properties(tsan_canary_uninst PROPERTIES AUTOMOC OFF)
    # The helper DSO plays the uninstrumented-Qt role: never instrument it,
    # even inside sanitizer builds (same discipline as the wire oracle).
    target_compile_options(tsan_canary_uninst PRIVATE -fno-sanitize=all)
    target_link_options(tsan_canary_uninst PRIVATE -fno-sanitize=all)
    add_executable(tsan_canary_mixed EXCLUDE_FROM_ALL tests/tsan_canary_mixed.cpp)
    set_target_properties(tsan_canary_mixed PROPERTIES AUTOMOC OFF)
    target_link_libraries(tsan_canary_mixed PRIVATE tsan_canary_uninst Threads::Threads)
    add_executable(tsan_canary_foreign EXCLUDE_FROM_ALL tests/tsan_canary_foreign.cpp)
    target_link_libraries(tsan_canary_foreign PRIVATE Qt6::Core Qt6::DBus Qt6::Test)

    # Wire oracle (council CF-7): the raw-libdbus peer the exactly-one-reply
    # pins assert through. Built only when libdbus-1 headers are present;
    # the pins QSKIP when the binary is absent (CI images carry
    # libdbus-1-dev — tests/wire-oracle/README.md).
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(DBUS1 QUIET dbus-1)
        if(DBUS1_FOUND)
            enable_language(C)
            add_executable(oracle tests/wire-oracle/oracle.c)
            set_target_properties(oracle PROPERTIES AUTOMOC OFF)
            # The oracle is the measurement peer, not the code under test:
            # never sanitizer-instrument it, even inside sanitizer builds.
            target_compile_options(oracle PRIVATE -fno-sanitize=all)
            target_link_options(oracle PRIVATE -fno-sanitize=all)
            target_include_directories(oracle PRIVATE ${DBUS1_INCLUDE_DIRS})
            target_link_libraries(oracle PRIVATE ${DBUS1_LIBRARIES})
        endif()
    endif()

    add_test(NAME test_types COMMAND test_types)
    add_test(NAME test_introspect COMMAND test_introspect)
    add_test(NAME test_adaptor COMMAND dbus-run-session -- $<TARGET_FILE:test_adaptor>)
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


