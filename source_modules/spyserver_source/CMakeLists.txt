cmake_minimum_required(VERSION 3.16...3.24)
project(spyserver_source)

file(GLOB SRC "src/*.cpp")

include(${SDRPP_MODULE_CMAKE})

target_include_directories(spyserver_source PRIVATE "src/")

# Loopback tests of the client. Like core's tests they sit beside
# sdrpp_core's shared library, which they load.
if (OPT_BUILD_TESTS AND NOT ANDROID)
    add_executable(spyserver_client_tests tests/spyserver_client_tests.cpp src/spyserver_client.cpp)
    target_include_directories(spyserver_client_tests PRIVATE "src/")
    target_link_libraries(spyserver_client_tests PRIVATE sdrpp_core)
    if (SDRPP_RUNTIME_OUTPUT_DIRECTORY)
        set_target_properties(spyserver_client_tests PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${SDRPP_RUNTIME_OUTPUT_DIRECTORY}")
    endif ()
    add_test(NAME spyserver_client_tests COMMAND spyserver_client_tests)
    set_tests_properties(spyserver_client_tests PROPERTIES TIMEOUT 60)
endif ()
