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
# CF-24: the signature fuzzer links the REAL dbusconnection.cpp TU
# (firstCompleteType/isStrictSignature/writeBySignature) instead of a
# TU-local copy — the old "identical by construction" copy could drift
# from the crashing function without any test noticing. The TU drags
# the adaptor/catalog/dispatcher link deps; that is the price of
# fuzzing the real symbol.

add_executable(fuzz_introspection_xml fuzz_introspection_xml.cpp
               ../dbusintrospection.cpp
               ../dbusconnection.cpp
               ../dbusmessage.cpp
               ../dbuspendingreply.cpp
               ../dbuscatalog.cpp
               ../dbuspathdispatcher.cpp
               ../dbustypes.cpp
               ../dbusadaptor.cpp
               ../dbusutils.cpp
               ../dbusheldreply.cpp
               ../dbuserror.h
               ../dbusmessage.h)
target_include_directories(fuzz_introspection_xml PRIVATE ..)
target_link_libraries(fuzz_introspection_xml PRIVATE Qt6::Core Qt6::DBus Qt6::Qml)

add_executable(fuzz_signature fuzz_signature.cpp
               ../dbusconnection.cpp
               ../dbusmessage.cpp
               ../dbuspendingreply.cpp
               ../dbuscatalog.cpp
               ../dbuspathdispatcher.cpp
               ../dbustypes.cpp
               ../dbusadaptor.cpp
               ../dbusutils.cpp
               ../dbusheldreply.cpp
               ../dbusintrospection.cpp
               ../dbuserror.h
               ../dbusmessage.h)
target_include_directories(fuzz_signature PRIVATE ..)
target_link_libraries(fuzz_signature PRIVATE Qt6::Core Qt6::DBus Qt6::Qml)
