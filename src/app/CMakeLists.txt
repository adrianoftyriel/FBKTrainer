# JUCE is fetched rather than vendored and pinned to a release tag, matching
# FBKSuppressor. The two must agree: they share fbk_dsp, and a JUCE version skew
# between the program that trains a model and the plugin that runs it is a
# difference nobody would think to look for.
include(FetchContent)

set(FBKT_JUCE_TAG "8.0.15" CACHE STRING "JUCE release tag to build against")

FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG ${FBKT_JUCE_TAG}
    GIT_SHALLOW TRUE
    GIT_PROGRESS TRUE
)
FetchContent_MakeAvailable(JUCE)

juce_add_gui_app(FBKTrainer
    PRODUCT_NAME    "FBKTrainer"
    COMPANY_NAME    "FBKSuppressor"
    BUNDLE_ID       "org.canops.fbktrainer"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "FBKTrainer needs microphone access to measure the room."

    # The rig fetches its own speech corpus over HTTPS. Windows goes through
    # WinINet and macOS through NSURLSession, but on Linux JUCE needs libcurl -
    # without it juce::URL cannot open an https stream at all, and the fetcher
    # would fail on the portability build for a reason that looks like a network
    # fault. NEEDS_CURL both sets the flag and links the library; defining
    # JUCE_USE_CURL by hand sets the flag twice with two different values.
    NEEDS_CURL TRUE
)

juce_generate_juce_header(FBKTrainer)

target_sources(FBKTrainer PRIVATE
    Main.cpp
    MainComponent.cpp
    ConfigStore.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../rig/OscTransport.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../rig/WingConsole.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../rig/AudioEngine.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../rig/RoutingSelfTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../rig/RunController.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../rig/SpeechFetcher.cpp
)

target_include_directories(FBKTrainer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../rig
)

target_compile_definitions(FBKTrainer PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    JUCE_MODAL_LOOPS_PERMITTED=0

    # ASIO on Windows. The rig runs everything through one device - in practice
    # the console's own card - and ASIO is how it is reached at a buffer size that
    # does not put the computer's own latency inside the feedback loop. From JUCE
    # 8 the required headers are bundled, so no external SDK is needed; enabling
    # it does put the binary under the Steinberg ASIO SDK licence terms.
    #
    # WASAPI is on by default and remains the fallback. It is usable for setting a
    # rig up and not for running one.
    $<$<PLATFORM_ID:Windows>:JUCE_ASIO=1>
    $<$<PLATFORM_ID:Linux>:JUCE_JACK=1>
)

target_link_libraries(FBKTrainer PRIVATE
    fbkt_core
    juce::juce_audio_utils
    juce::juce_audio_devices
    juce::juce_dsp
    juce::juce_osc
    juce::juce_gui_extra
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
)

# The analysis and simulation stages link the shipping DSP directly. Milestone 1
# does not use them yet; the link is wired now so that the dependency is proved by
# the build rather than discovered later.
if(FBKT_HAVE_DSP)
    target_link_libraries(FBKTrainer PRIVATE fbk_dsp)
    target_compile_definitions(FBKTrainer PRIVATE FBKT_HAVE_DSP=1)
endif()
