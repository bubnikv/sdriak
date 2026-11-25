cmake_minimum_required(VERSION 3.16...3.24)
project(ubersdr_source)

file(GLOB SRC "src/*.cpp")

include(${SDRPP_MODULE_CMAKE})

target_include_directories(ubersdr_source PRIVATE "src/")

# Parser tests for the spectrum socket (no network). Like core's tests they
# sit beside sdrpp_core's shared library, which they load.
if (OPT_BUILD_TESTS AND NOT ANDROID)
    add_executable(ubersdr_spectrum_tests tests/ubersdr_spectrum_tests.cpp src/ubersdr_client.cpp)
    target_include_directories(ubersdr_spectrum_tests PRIVATE "src/")
    target_link_libraries(ubersdr_spectrum_tests PRIVATE sdrpp_core)
    if (SDRPP_RUNTIME_OUTPUT_DIRECTORY)
        set_target_properties(ubersdr_spectrum_tests PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${SDRPP_RUNTIME_OUTPUT_DIRECTORY}")
    endif ()
    add_test(NAME ubersdr_spectrum_tests COMMAND ubersdr_spectrum_tests)
endif ()
