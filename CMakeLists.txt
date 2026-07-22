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

FetchContent_Declare(
	rlImGui
	GIT_REPOSITORY https://github.com/raylib-extras/rlImGui.git
	GIT_TAG main
	GIT_SHALLOW TRUE
)
FetchContent_Declare(
	imgui
	GIT_REPOSITORY https://github.com/ocornut/imgui.git
	GIT_TAG master
	GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(rlImGui imgui)

add_executable(game
	src/main.cpp
	src/core/app.cpp
	src/core/window.cpp
	src/engine/input.cpp
	src/engine/networking.cpp
	src/engine/math/noise.cpp
	src/engine/jobs/thread_pool.cpp
	src/engine/terrain/chunk_manager.cpp
	src/game/game_app.cpp
	src/game/systems.cpp
	src/game/world/world_gen.cpp
	${rlimgui_SOURCE_DIR}/rlImGui.cpp
	${imgui_SOURCE_DIR}/imgui.cpp
	${imgui_SOURCE_DIR}/imgui_draw.cpp
	${imgui_SOURCE_DIR}/imgui_tables.cpp
	${imgui_SOURCE_DIR}/imgui_widgets.cpp)

target_include_directories(game PRIVATE
	include
	${CMAKE_SOURCE_DIR}/vendor/FastNoiseLite
	${rlimgui_SOURCE_DIR}
	${imgui_SOURCE_DIR})

# Threads (thread pool for background terrain generation)
find_package(Threads REQUIRED)
target_link_libraries(game PRIVATE Threads::Threads)

# ---------------------------------------------------------------------------
# Steamworks SDK
# Resolution order:
#   1. -DSTEAM_SDK_ROOT=... passed on the command line
#   2. vendor/steamworks/  inside the repo  (recommended)
#   3. Global install: %USERPROFILE%/steamworks_sdk_164/sdk  or  ~/steamworks_sdk_164/sdk
# ---------------------------------------------------------------------------
if(NOT DEFINED STEAM_SDK_ROOT)
	if(EXISTS "${CMAKE_SOURCE_DIR}/vendor/steamworks/public/steam/steam_api.h")
		set(STEAM_SDK_ROOT "${CMAKE_SOURCE_DIR}/vendor/steamworks")
	elseif(WIN32)
		set(STEAM_SDK_ROOT "$ENV{USERPROFILE}/steamworks_sdk_164/sdk")
	else()
		set(STEAM_SDK_ROOT "$ENV{HOME}/steamworks_sdk_164/sdk")
	endif()
endif()
message(STATUS "Steamworks SDK: ${STEAM_SDK_ROOT}")

target_include_directories(game PRIVATE ${STEAM_SDK_ROOT}/public)

if(WIN32)
	target_link_libraries(game PRIVATE raylib
		${STEAM_SDK_ROOT}/redistributable_bin/win64/steam_api64.lib)
	add_custom_command(TARGET game POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
		${STEAM_SDK_ROOT}/redistributable_bin/win64/steam_api64.dll
		$<TARGET_FILE_DIR:game>/steam_api64.dll)
else()
	target_link_libraries(game PRIVATE raylib
		${STEAM_SDK_ROOT}/redistributable_bin/linux64/libsteam_api.so)
	add_custom_command(TARGET game POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
		${STEAM_SDK_ROOT}/redistributable_bin/linux64/libsteam_api.so
		$<TARGET_FILE_DIR:game>/libsteam_api.so)
endif()

# steam_appid.txt (480 = Spacewar test app) — lets you run without a Steam client
add_custom_command(TARGET game POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
	${CMAKE_SOURCE_DIR}/steam_appid.txt
	$<TARGET_FILE_DIR:game>/steam_appid.txt)

# Copy assets into build directory so paths resolve at runtime
add_custom_command(TARGET game POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_directory
	${CMAKE_SOURCE_DIR}/assets
	$<TARGET_FILE_DIR:game>/assets)


