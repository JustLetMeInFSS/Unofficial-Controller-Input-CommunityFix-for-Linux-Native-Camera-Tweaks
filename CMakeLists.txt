cmake_minimum_required(VERSION 3.16)
project(linux_native_camera_tweaks)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(SDL2 REQUIRED)

file(GLOB_RECURSE SOURCES "src/*.c" "src/*.h")
add_library(${PROJECT_NAME} SHARED ${SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE src ${SDL2_INCLUDE_DIRS})
target_compile_definitions(${PROJECT_NAME} PRIVATE _GNU_SOURCE)
target_link_libraries(${PROJECT_NAME} PRIVATE ${CMAKE_DL_LIBS} m)

include(CTest)
if(BUILD_TESTING)
    add_executable(controller_input_tests tests/controller_input_test.c)
    target_include_directories(controller_input_tests PRIVATE src)
    target_link_libraries(controller_input_tests PRIVATE m)
    add_test(NAME controller_input_tests COMMAND controller_input_tests)

    add_executable(config_tests tests/config_test.c src/config.c)
    target_include_directories(config_tests PRIVATE src)
    target_link_libraries(config_tests PRIVATE m)
    add_test(NAME config_tests COMMAND config_tests)
    set_tests_properties(config_tests PROPERTIES
        ENVIRONMENT "XDG_CONFIG_HOME=${CMAKE_BINARY_DIR}/test-config")

    add_executable(camera_state_tests tests/camera_state_test.c)
    target_include_directories(camera_state_tests PRIVATE src)
    target_link_libraries(camera_state_tests PRIVATE m)
    add_test(NAME camera_state_tests COMMAND camera_state_tests)

    add_executable(portable_paths_tests tests/portable_paths_test.c src/portable_paths.c)
    target_include_directories(portable_paths_tests PRIVATE src)
    add_test(NAME portable_paths_tests COMMAND portable_paths_tests)
endif()
