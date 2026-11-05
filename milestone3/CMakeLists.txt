cmake_minimum_required(VERSION 3.16)
project(ClockworkDash LANGUAGES CXX)

add_subdirectory(engine)
include(CTest)
add_subdirectory(game/clockwork_dash)
