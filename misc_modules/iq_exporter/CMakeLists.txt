cmake_minimum_required(VERSION 3.16...3.24)
project(iq_exporter)

file(GLOB SRC "src/*.cpp")

include(${SDRPP_MODULE_CMAKE})

if (OPT_BUILD_TESTS AND NOT ANDROID)
    add_executable(iq_exporter_session_tests tests/export_session_tests.cpp)
    target_include_directories(iq_exporter_session_tests PRIVATE "src/")
    target_link_libraries(iq_exporter_session_tests PRIVATE sdrpp_core)
    add_test(NAME iq_exporter_session_tests COMMAND iq_exporter_session_tests)
endif ()
