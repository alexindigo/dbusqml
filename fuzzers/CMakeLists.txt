cmake_minimum_required(VERSION 3.16)
project(dbusqml-fuzzers LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core DBus Qml)

# Local/CI smoke builds define DBUSQML_FUZZ_MAIN (seed-corpus mains, no
# libFuzzer needed — GCC has no -fsanitize=fuzzer). The CI fuzz job
# builds WITHOUT it under clang with -fsanitize=fuzzer (libFuzzer
# provides main + LLVMFuzzerTestOneInput linkage).
if(NOT DEFINED DBUSQML_FUZZ_MAIN)
    set(DBUSQML_FUZZ_MAIN ON)
endif()
if(DBUSQML_FUZZ_MAIN)
    add_compile_definitions(DBUSQML_FUZZ_MAIN)
endif()

# libFuzzer targets over the genuinely remote-facing parsers ONLY (D10):
# the remote-introspection XML parser + signature strings extracted from
# it. The demarshal path sits behind libdbus validation (all seats);
# catalog XML is local trust (malformed-input qtests only).
#
# firstCompleteType lives in dbusconnection.cpp, which drags the whole
# connection TU (moc, pending-reply, catalog...). Instead of linking the
# TU, the signature fuzzer compiles a TU-local copy (see below) — the
# function is 60 lines of pure string walking with no dependencies.

add_executable(fuzz_introspection_xml fuzz_introspection_xml.cpp
               ../dbusintrospection.cpp)
target_include_directories(fuzz_introspection_xml PRIVATE ..)
target_link_libraries(fuzz_introspection_xml PRIVATE Qt6::Core Qt6::DBus Qt6::Qml)

add_executable(fuzz_signature fuzz_signature.cpp)
target_link_libraries(fuzz_signature PRIVATE Qt6::Core)
