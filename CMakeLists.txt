cmake_minimum_required(VERSION 3.16)

project(nandprog VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(NANDPROG_BUILD_TESTS "Build nandprog tests" ON)

add_library(nandprog_core
    src/core/chip_db.cpp
    src/core/mibib.cpp
    src/core/nand_client.cpp
    src/core/protocol.cpp
    src/core/util.cpp
    src/qpic/qpic.cpp
)

if(WIN32)
    target_sources(nandprog_core PRIVATE src/transport/serial_win32.cpp)
    target_compile_definitions(nandprog_core PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(nandprog_core PRIVATE winmm)
else()
    target_sources(nandprog_core PRIVATE src/transport/serial_posix.cpp)
endif()

target_include_directories(nandprog_core PUBLIC include)

if(MSVC)
    target_compile_options(nandprog_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(nandprog_core PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_executable(nandprog
    src/main.cpp
    src/cli/commands.cpp
)
target_link_libraries(nandprog PRIVATE nandprog_core)

add_custom_command(TARGET nandprog POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/data/nando_parallel_chip_db.csv"
        "$<TARGET_FILE_DIR:nandprog>/nando_parallel_chip_db.csv"
)

include(GNUInstallDirs)
install(TARGETS nandprog RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/nando_parallel_chip_db.csv
    DESTINATION ${CMAKE_INSTALL_BINDIR})

if(NOT MSVC)
    target_link_options(nandprog PRIVATE -static-libgcc -static-libstdc++)
endif()

if(NANDPROG_BUILD_TESTS)
    enable_testing()
    add_executable(nandprog_tests tests/tests.cpp)
    target_link_libraries(nandprog_tests PRIVATE nandprog_core)
    if(NOT MSVC)
        target_link_options(nandprog_tests PRIVATE -static-libgcc -static-libstdc++)
    endif()
    target_compile_definitions(nandprog_tests PRIVATE
        NANDPROG_TEST_DB="${CMAKE_CURRENT_SOURCE_DIR}/data/nando_parallel_chip_db.csv")
    add_test(NAME nandprog_tests COMMAND nandprog_tests)
endif()
