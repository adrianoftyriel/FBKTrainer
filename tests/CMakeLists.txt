add_executable(fbkt_tests TestMain.cpp)

target_link_libraries(fbkt_tests PRIVATE fbkt_core)
target_include_directories(fbkt_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(MSVC)
    target_compile_options(fbkt_tests PRIVATE /W4)
else()
    target_compile_options(fbkt_tests PRIVATE -Wall -Wextra)
endif()

add_test(NAME core COMMAND fbkt_tests)
