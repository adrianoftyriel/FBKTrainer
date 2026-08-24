cmake_minimum_required(VERSION 3.22)

project(FBKTrainer VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(FBKT_BUILD_APP   "Build the Windows application (requires JUCE)" ON)
option(FBKT_BUILD_TESTS "Build the core unit tests" ON)

# FBKSuppressor supplies fbk_dsp, which the analysis and simulation stages link
# against directly - the simulator has to run the exact code that ships or a
# result in the simulator means nothing about the product. It is a sibling
# checkout rather than a fetch so that the version under test is whatever is on
# the disk, which is what you want while both repositories are moving together.
set(FBKT_SUPPRESSOR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../FBKSuppressor"
    CACHE PATH "Path to an FBKSuppressor checkout supplying fbk_dsp")

if(EXISTS "${FBKT_SUPPRESSOR_DIR}/src/dsp/CMakeLists.txt")
    add_subdirectory("${FBKT_SUPPRESSOR_DIR}/src/dsp" "${CMAKE_BINARY_DIR}/fbk_dsp")
    set(FBKT_HAVE_DSP TRUE)
    message(STATUS "FBKTrainer: fbk_dsp from ${FBKT_SUPPRESSOR_DIR}")
else()
    set(FBKT_HAVE_DSP FALSE)
    message(STATUS "FBKTrainer: fbk_dsp not found at ${FBKT_SUPPRESSOR_DIR} - "
                   "analysis stages will be skipped. Set FBKT_SUPPRESSOR_DIR to enable them.")
endif()

if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Minimum macOS version")
endif()

add_subdirectory(src/core)

if(FBKT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(FBKT_BUILD_APP)
    add_subdirectory(src/app)
endif()
