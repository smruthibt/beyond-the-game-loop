# The Team 16 engine as one CMake target. From any project:
#     add_subdirectory(engine)
#     target_link_libraries(my_game PRIVATE engine)
cmake_minimum_required(VERSION 3.16)
project(Team16Engine CXX)

find_package(SDL3 REQUIRED)
find_package(SDL3_image REQUIRED)

# ZeroMQ: libzmq is the library, cppzmq its header-only C++ binding.
find_package(PkgConfig REQUIRED)
pkg_check_modules(ZMQ REQUIRED IMPORTED_TARGET libzmq)
find_path(CPPZMQ_INCLUDE_DIR zmq.hpp)
if(NOT CPPZMQ_INCLUDE_DIR)
    message(FATAL_ERROR "cppzmq (zmq.hpp) not found - brew install cppzmq")
endif()

add_library(engine STATIC
    src/Engine.cpp
    src/Entity.cpp
    src/Physics.cpp
    src/Input.cpp
    src/Collision.cpp
    src/Scaling.cpp
    src/Timeline.cpp
    src/Network.cpp
    src/JobSystem.cpp
    src/Server.cpp
    src/Client.cpp
    src/Peer.cpp
    src/World.cpp
    src/Serialize.cpp
    src/Components.cpp
    src/Systems.cpp
    src/Snapshot.cpp
    src/NetScene.cpp
)
target_compile_features(engine PUBLIC cxx_std_17)
target_include_directories(engine PUBLIC include)

# zmq.hpp is included only by Network.cpp - Network.hpp is pimpl'd - so the
# header path is private. libzmq itself is PUBLIC because anything linking
# this static library still has to resolve its symbols at final link.
target_include_directories(engine PRIVATE ${CPPZMQ_INCLUDE_DIR})
target_link_libraries(engine PUBLIC SDL3::SDL3 SDL3_image::SDL3_image PkgConfig::ZMQ)

# The engine's own programs, shipped with it so a teammate who copies this
# folder gets them too:
#   scene_demo   the worked example: a small networked platformer to copy from
#   net_bench    the performance benchmark that tools/net_experiments.py runs
# They land in the top-level build folder, beside the games.
option(ENGINE_EXTRAS "Build scene_demo and net_bench" ON)
if(ENGINE_EXTRAS)
    add_executable(scene_demo examples/scene_demo.cpp)
    add_executable(net_bench bench/net_bench.cpp)
    foreach(EXTRA scene_demo net_bench)
        target_link_libraries(${EXTRA} PRIVATE engine)
        set_target_properties(${EXTRA} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    endforeach()
endif()

# The engine's own tests: one headless program per claim, no game needed
# (see "Verifying" in engine/README.md). Off by default when
# the engine is a subproject, so they never clash with a game's own targets;
# turn them on with -DENGINE_TESTS=ON. They land in the top-level build folder.
option(ENGINE_TESTS "Build the engine's tests (engine/tests/)" OFF)
if(ENGINE_TESTS)
    foreach(TEST timeline_test integration_test net_test jobsystem_test collision_test
                 engine_server_test engine_client_test engine_peer_test frametime_test iteration_test
                 object_model_test systems_test snapshot_test netscene_test)
        add_executable(${TEST} tests/${TEST}.cpp)
        target_link_libraries(${TEST} PRIVATE engine)
        set_target_properties(${TEST} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    endforeach()
endif()
