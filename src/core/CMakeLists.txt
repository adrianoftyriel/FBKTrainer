# The safety-critical half of the program, deliberately free of JUCE, of the
# audio stack and of fbk_dsp.
#
# Everything that decides whether gain may go up lives here, and it lives here
# precisely so that it can be compiled and exercised on a CI runner with no audio
# hardware, no console on the network and no plugin framework fetched. A safety
# envelope that can only be tested in the room is one that will only be tested
# once.
add_library(fbkt_core STATIC
    RigConfig.cpp
    SafetySupervisor.cpp
    WingProtocol.cpp
    GainRamp.cpp
)

target_include_directories(fbkt_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(fbkt_core PUBLIC cxx_std_20)
set_target_properties(fbkt_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(MSVC)
    target_compile_options(fbkt_core PRIVATE /W4)
else()
    target_compile_options(fbkt_core PRIVATE -Wall -Wextra -Wpedantic)
endif()
