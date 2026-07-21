# game2

First-person apocalyptic action fantasy RPG. Left 4 Dead-style horde combat with ARPG progression. Built with raylib + Steamworks SDK.

---

## Requirements

- **CMake** 3.20+ — https://cmake.org/download/
- **Visual Studio 2022** with the "Desktop development with C++" workload (includes MSVC and Windows SDK)
  - Or **MinGW-w64** / **LLVM** if you prefer GCC/Clang on Windows
- **Git** — https://git-scm.com/
- **Steamworks SDK 1.64** — download from https://partner.steamgames.com/ (requires a free Steam developer account)
- **Steam client** must be running when you launch the game

---

## Setup

### 1. Clone the repo

```
git clone https://github.com/EarlyEWR/game2.git
cd game2
```

### 2. Extract the Steamworks SDK

Extract the Steamworks SDK so the folder structure looks like:

```
%USERPROFILE%\steamworks_sdk_164\sdk\public\steam\steam_api.h   ← this file must exist
%USERPROFILE%\steamworks_sdk_164\sdk\redistributable_bin\win64\steam_api64.lib
%USERPROFILE%\steamworks_sdk_164\sdk\redistributable_bin\win64\steam_api64.dll
```

`%USERPROFILE%` is typically `C:\Users\YourName`.

If you want to put the SDK somewhere else, pass `-DSTEAM_SDK_ROOT=C:\path\to\sdk` to CMake in the next step.

### 3. Configure

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
```

With a custom SDK path:

```
cmake -B build -DCMAKE_BUILD_TYPE=Release -DSTEAM_SDK_ROOT="C:\SteamSDK\sdk"
```

### 4. Build

```
cmake --build build --config Release
```

### 5. Run

Make sure Steam is open, then:

```
cd build\Release
game.exe
```

> The first time you run, Steam may ask you to select which game to launch — pick **Spacewar** (app 480, the test app). This is normal for development builds.

---

## Multiplayer (Steam lobby)

One player hosts, the other joins via Steam overlay invite or the in-game join flow. Both players must have Steam running.

---

## Project structure

```
include/   — headers (core / engine / game)
src/       — source files
assets/    — game assets (copied into build dir automatically)
CMakeLists.txt
```

Raylib is fetched automatically by CMake — no manual download needed.
