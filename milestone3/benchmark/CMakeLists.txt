cmake_minimum_required(VERSION 3.16)
project(BenchmarkRerun LANGUAGES CXX)

# The rerun benchmark harness. The team engine and the game are compiled from
# their own folders, unmodified; nothing here edits their sources.
#
#   cmake -S benchmark -B benchmark/build -DCMAKE_BUILD_TYPE=Release
#   cmake --build benchmark/build -j 8

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(ENGINE_EXTRAS OFF CACHE BOOL "" FORCE)
set(ENGINE_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../engine engine)

set(GAME_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../game/clockwork_dash)
add_library(clockwork_game_lib STATIC ${GAME_DIR}/Game.cpp ${GAME_DIR}/Draw.cpp ${GAME_DIR}/PeerRace.cpp)
target_include_directories(clockwork_game_lib PUBLIC ${GAME_DIR})
target_link_libraries(clockwork_game_lib PUBLIC engine)

add_library(harness STATIC
    harness/Common.cpp
    harness/Frames.cpp
    harness/Canonical.cpp
    harness/Scenario.cpp
    harness/Endpoint.cpp
    harness/Coordinator.cpp
    harness/ServerRole.cpp
    harness/ClientRole.cpp
    harness/PeerRole.cpp
    harness/Oracle.cpp
)
target_include_directories(harness PUBLIC harness)
target_link_libraries(harness PUBLIC engine clockwork_game_lib)
target_compile_options(harness PRIVATE -Wall -Wextra)

add_executable(rerun_bench harness/main.cpp)
target_link_libraries(rerun_bench PRIVATE harness)

add_executable(rerun_faults harness/faults.cpp)
target_link_libraries(rerun_faults PRIVATE harness)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
