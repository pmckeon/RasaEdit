cmake_minimum_required (VERSION 3.16)

# Enable Hot Reload for MSVC compilers if supported.
if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

project ("RasaEdit")

# Find the dependencies from vcpkg
find_package(SDL3 CONFIG REQUIRED)
find_package(SDL3_mixer CONFIG REQUIRED)       # asset preview sounds: WAV, Ogg Vorbis (libvorbis), MP3 (mpg123)
find_package(imgui CONFIG REQUIRED)
# SQLite for the server (World) database: vcpkg's port, or a system package elsewhere.
find_package(unofficial-sqlite3 CONFIG QUIET)
if (TARGET unofficial::sqlite3::sqlite3)
    set(RASAEDIT_SQLITE unofficial::sqlite3::sqlite3)
else()
    find_package(SQLite3 REQUIRED)
    set(RASAEDIT_SQLITE SQLite::SQLite3)
endif()

# Tabula Rasa format library (needs zlib, also from vcpkg); its command-line tools are not built here.
add_subdirectory(external/trformats)

add_executable (RasaEdit
    "RasaEdit.cpp" "RasaEdit.h"
    "Editor.cpp" "Editor.h"
    "EditorContext.cpp" "EditorContext.h"
    "Workspace.h"
    "ClientWorkspace.cpp" "ClientWorkspace.h"
    "MissionEditor.cpp" "MissionEditor.h"
    "ServerWorkspace.cpp" "ServerWorkspace.h"
    "Panels.cpp" "Panels.h"
    "AssetPreview.cpp" "AssetPreview.h"
    "SoundPlayer.cpp" "SoundPlayer.h"
    "TerrainOverview.cpp" "TerrainOverview.h"
    "RadarMap.cpp" "RadarMap.h"
    "GpuTexture.cpp" "GpuTexture.h"
    "Renderer.cpp" "Renderer.h"
    "SceneView.cpp" "SceneView.h"
    "ParticleSim.cpp" "ParticleSim.h"
    "GpuMath.h" "MarkerColours.h" "ShaderBytecode.h"
    "WorldDatabase.cpp" "WorldDatabase.h"
    "Avatar.cpp" "Avatar.h"
)

# Link everything together
target_link_libraries(RasaEdit PRIVATE
    SDL3::SDL3
    SDL3_mixer::SDL3_mixer
    imgui::imgui
    trformats
    ${RASAEDIT_SQLITE}
)

set_property(TARGET RasaEdit PROPERTY CXX_STANDARD 20)

# Headless rendering test (renders a map offscreen and writes raw RGBA; works on any Vulkan driver).
option(RASAEDIT_BUILD_TESTS "Build the headless viewport test" OFF)
if (RASAEDIT_BUILD_TESTS)
    add_executable(viewport_test tests/viewport_test.cpp Renderer.cpp SceneView.cpp ParticleSim.cpp GpuTexture.cpp WorldDatabase.cpp Avatar.cpp)
    target_include_directories(viewport_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(viewport_test PRIVATE SDL3::SDL3 trformats ${RASAEDIT_SQLITE})

    # Whole-editor smoke test: real ImGui frames and both workspaces, headless.
    add_executable(ui_smoke_test tests/ui_smoke_test.cpp Editor.cpp EditorContext.cpp ClientWorkspace.cpp ServerWorkspace.cpp MissionEditor.cpp Panels.cpp AssetPreview.cpp SoundPlayer.cpp
                   Renderer.cpp SceneView.cpp ParticleSim.cpp GpuTexture.cpp WorldDatabase.cpp Avatar.cpp TerrainOverview.cpp RadarMap.cpp)
    target_include_directories(ui_smoke_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(ui_smoke_test PRIVATE SDL3::SDL3 SDL3_mixer::SDL3_mixer imgui::imgui trformats ${RASAEDIT_SQLITE})
    set_property(TARGET ui_smoke_test PROPERTY CXX_STANDARD 20)
    set_property(TARGET viewport_test PROPERTY CXX_STANDARD 20)
endif()
