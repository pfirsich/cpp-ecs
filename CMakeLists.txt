cmake_minimum_required(VERSION 3.0.0)
project(ecs)

set(CMAKE_CXX_STANDARD 17)

include_directories(ecs/include)
add_library(ecs ecs/ecs.cpp)

add_executable(test ecs/main.cpp)
target_link_libraries(test ecs)

#set(SFML_STATIC_LIBRARIES TRUE)
find_package(SFML 2.5 COMPONENTS graphics window system REQUIRED)

find_package(GLM REQUIRED)

add_executable(asteroids ecs/asteroids.cpp)
target_link_libraries(asteroids ecs sfml-graphics sfml-window sfml-system)
