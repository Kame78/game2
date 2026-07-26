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

# Enable Radiance .hdr loading (stbi_loadf) for the equirectangular sky HDRI.
target_compile_definitions(raylib PUBLIC SUPPORT_FILEFORMAT_HDR)

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
	src/engine/data/json.cpp
	src/engine/math/noise.cpp
	src/engine/math/hydrology.cpp
	src/engine/jobs/thread_pool.cpp
	src/engine/terrain/chunk_manager.cpp
	src/engine/render/sky.cpp
	src/game/game_app.cpp
	src/game/enemy_model.cpp
	src/game/factories/entity_factory.cpp
	src/game/ui/menu_screens.cpp
	src/game/ui/hud.cpp
	src/game/systems/player_system.cpp
	src/game/systems/combat_system.cpp
	src/game/systems/spell_system.cpp
	src/game/systems/enemy_system.cpp
	src/game/systems/spawner_system.cpp
	src/game/systems/render_system.cpp
	src/game/character_visual.cpp
	src/game/systems/network_system.cpp
	src/game/systems/editor_system.cpp
	src/game/world/world_gen.cpp
	src/game/dungeon/dungeon_gen.cpp
	src/game/dungeon/dungeon_data.cpp
	src/game/dungeon/dungeon_session.cpp
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

# -----------------------------------------------------------------------------
# Necro-dagger viewmodel: raylib only supports 16-bit mesh indices, so the raw
# ~1M-vertex model.glb has to be decimated. If Blender is available, we build
# assets/models/necro-dagger/model_lowpoly.glb (~20k tris) at configure time.
# -----------------------------------------------------------------------------
set(DAGGER_DIR "${CMAKE_SOURCE_DIR}/assets/models/necro-dagger")
set(DAGGER_SRC "${DAGGER_DIR}/model.glb")
set(DAGGER_LOW "${DAGGER_DIR}/model_lowpoly.glb")
if(EXISTS "${DAGGER_SRC}" AND NOT EXISTS "${DAGGER_LOW}")
	find_program(BLENDER_EXECUTABLE blender)
	if(BLENDER_EXECUTABLE)
		message(STATUS "Decimating necro-dagger with ${BLENDER_EXECUTABLE}")
		execute_process(
			COMMAND ${BLENDER_EXECUTABLE} --background --python
				"${DAGGER_DIR}/decimate.py" -- "${DAGGER_SRC}" "${DAGGER_LOW}" 20000
			WORKING_DIRECTORY "${DAGGER_DIR}"
			RESULT_VARIABLE DAGGER_RC
			OUTPUT_QUIET)
		if(NOT DAGGER_RC EQUAL 0)
			message(WARNING "Dagger decimation failed (rc=${DAGGER_RC}); viewmodel will fall back to procedural sword")
		endif()
	else()
		message(WARNING "Blender not found; skipping dagger decimation. Run manually:\n"
			"  blender --background --python ${DAGGER_DIR}/decimate.py -- ${DAGGER_SRC} ${DAGGER_LOW} 20000")
	endif()
endif()

# Copy assets into build directory so paths resolve at runtime
add_custom_command(TARGET game POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_directory
	${CMAKE_SOURCE_DIR}/assets
	$<TARGET_FILE_DIR:game>/assets)


