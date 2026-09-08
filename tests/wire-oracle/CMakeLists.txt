cmake_minimum_required(VERSION 3.16)
project(dbusqml-wire-oracle C)

find_package(PkgConfig REQUIRED)
pkg_check_modules(DBUS REQUIRED dbus-1)

add_executable(oracle oracle.c)
target_include_directories(oracle PRIVATE ${DBUS_INCLUDE_DIRS})
target_link_libraries(oracle PRIVATE ${DBUS_LIBRARIES})
