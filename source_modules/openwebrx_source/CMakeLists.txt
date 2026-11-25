cmake_minimum_required(VERSION 3.16...3.24)
project(openwebrx_source)

file(GLOB SRC "src/*.cpp")

include(${SDRPP_MODULE_CMAKE})

target_include_directories(openwebrx_source PRIVATE "src/")

# Decoder and message tests (no network), plus a live smoke run with
# --live <url>. Like core's tests they sit beside sdrpp_core's shared
# library, which they load.
if (OPT_BUILD_TESTS AND NOT ANDROID)
    add_executable(owrx_client_tests tests/owrx_client_tests.cpp src/owrx_client.cpp)
    target_include_directories(owrx_client_tests PRIVATE "src/")
    target_link_libraries(owrx_client_tests PRIVATE sdrpp_core)
    if (SDRPP_RUNTIME_OUTPUT_DIRECTORY)
        set_target_properties(owrx_client_tests PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${SDRPP_RUNTIME_OUTPUT_DIRECTORY}")
    endif ()
    add_test(NAME owrx_client_tests COMMAND owrx_client_tests)
endif ()
