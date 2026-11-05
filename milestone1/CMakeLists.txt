cmake_minimum_required(VERSION 3.10)
project(FeelingLoopy)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(SDL3 REQUIRED)
find_package(SDL3_image REQUIRED)

add_library(engine STATIC
    engine/src/Engine.cpp
    engine/src/Entity.cpp
    engine/src/Physics.cpp
    engine/src/Input.cpp
    engine/src/Collision.cpp
    engine/src/Scaling.cpp
)
target_include_directories(engine PUBLIC engine/include)
target_link_libraries(engine PUBLIC SDL3::SDL3 SDL3_image::SDL3_image)

foreach(GAME neon_grave_escape)
    add_executable(${GAME} game/${GAME}/main.cpp)
    target_link_libraries(${GAME} PRIVATE engine)
endforeach()
