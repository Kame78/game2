cmake_minimum_required(VERSION 3.20)
project(game LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

FetchContent_Declare(
	raylib
	GIT_REPOSITORY https://github.com/raysan5/raylib.git
	GIT_TAG 5.5
	GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(raylib)

add_executable(game 
src/main.cpp
src/core/app.cpp
src/game/game_app.cpp)

target_include_directories(game PRIVATE include)

target_link_libraries(game PRIVATE raylib)


