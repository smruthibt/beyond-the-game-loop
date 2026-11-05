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
)
target_compile_features(engine PUBLIC cxx_std_17)
target_include_directories(engine PUBLIC include)

# zmq.hpp is included only by Network.cpp - Network.hpp is pimpl'd - so the
# header path is private. libzmq itself is PUBLIC because anything linking
# this static library still has to resolve its symbols at final link.
target_include_directories(engine PRIVATE ${CPPZMQ_INCLUDE_DIR})
target_link_libraries(engine PUBLIC SDL3::SDL3 SDL3_image::SDL3_image PkgConfig::ZMQ)

option(BUILD_TOWERFALL "Build Towerfall and its tests" ON)
if(BUILD_TOWERFALL)
    enable_testing()
    add_subdirectory(game)
endif()
