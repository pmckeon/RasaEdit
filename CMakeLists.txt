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
find_package(Freetype REQUIRED)               # UI text (UiCanvas): hinted glyphs like the game's GDI ones
# SQLite for the server (World) database: vcpkg's port, or a system package elsewhere.
find_package(unofficial-sqlite3 CONFIG QUIET)
if (TARGET unofficial::sqlite3::sqlite3)
    set(RASAEDIT_SQLITE unofficial::sqlite3::sqlite3)
else()
    find_package(SQLite3 REQUIRED)
    set(RASAEDIT_SQLITE SQLite::SQLite3)
endif()
# MySQL / MariaDB World databases (Rasa.NET's other provider): MariaDB Connector/C, which talks to both servers.
find_package(unofficial-libmariadb CONFIG REQUIRED)
list(APPEND RASAEDIT_SQLITE unofficial::libmariadb)

# Tabula Rasa format library (needs zlib, also from vcpkg); its command-line tools are not built here.
add_subdirectory(external/trformats)

add_executable (RasaEdit
    "RasaEdit.cpp" "RasaEdit.h"
    "Editor.cpp" "Editor.h"
    "EditorContext.cpp" "EditorContext.h"
    "Workspace.h"
    "ClientWorkspace.cpp" "ClientWorkspace.h"
    "MissionEditor.cpp" "MissionEditor.h" "Lookups.cpp" "Lookups.h" "ModelExchange.cpp" "ModelExchange.h" "ModelImport.cpp" "ModelMath.h" "BlenderPanel.cpp" "BlenderPanel.h"
    "MissionValidation.cpp" "MissionValidation.h"
    "Json.cpp" "Json.h"
    "LootEditor.cpp" "LootEditor.h"
    "ModuleEditor.cpp" "ModuleEditor.h"
    "RecipeEditor.cpp" "RecipeEditor.h"
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
    "DbConnection.cpp" "DbConnection.h"
    "Avatar.cpp" "Avatar.h"
    "UiCanvas.cpp" "UiCanvas.h"
    "UiSchema.cpp" "UiSchema.h"
    "UiWorkspace.cpp" "UiWorkspace.h"
    "UiDefinitions.cpp"
)

# Link everything together
target_link_libraries(RasaEdit PRIVATE
    SDL3::SDL3
    SDL3_mixer::SDL3_mixer
    imgui::imgui
    Freetype::Freetype
    trformats
    ${RASAEDIT_SQLITE}
)

set_property(TARGET RasaEdit PROPERTY CXX_STANDARD 20)

# MariaDB Connector/C loads its authentication plugins (MySQL 8's default caching_sha2_password is one) at run time from a
# folder: copy the vcpkg port's plugins to <exe dir>/plugins/libmariadb, where the editor points the connector (DbConnection).
function(rasaedit_copy_mariadb_plugins target)
    if (DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
        set(plugins "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/$<$<CONFIG:Debug>:debug/>plugins/libmariadb")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${plugins}" "$<TARGET_FILE_DIR:${target}>/plugins/libmariadb")
    endif()
endfunction()
rasaedit_copy_mariadb_plugins(RasaEdit)

# Headless rendering test (renders a map offscreen and writes raw RGBA; works on any Vulkan driver).
option(RASAEDIT_BUILD_TESTS "Build the headless viewport test" OFF)
if (RASAEDIT_BUILD_TESTS)
    add_executable(viewport_test tests/viewport_test.cpp Renderer.cpp SceneView.cpp ParticleSim.cpp GpuTexture.cpp WorldDatabase.cpp DbConnection.cpp Avatar.cpp UiCanvas.cpp UiSchema.cpp MissionValidation.cpp Json.cpp ModelExchange.cpp ModelImport.cpp)
    target_include_directories(viewport_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(viewport_test PRIVATE SDL3::SDL3 Freetype::Freetype trformats ${RASAEDIT_SQLITE})

    # Whole-editor smoke test: real ImGui frames and both workspaces, headless.
    add_executable(ui_smoke_test tests/ui_smoke_test.cpp Editor.cpp EditorContext.cpp ClientWorkspace.cpp ServerWorkspace.cpp MissionEditor.cpp Lookups.cpp ModelExchange.cpp ModelImport.cpp BlenderPanel.cpp MissionValidation.cpp Json.cpp LootEditor.cpp ModuleEditor.cpp RecipeEditor.cpp Panels.cpp AssetPreview.cpp SoundPlayer.cpp
                   Renderer.cpp SceneView.cpp ParticleSim.cpp GpuTexture.cpp WorldDatabase.cpp DbConnection.cpp Avatar.cpp TerrainOverview.cpp RadarMap.cpp UiCanvas.cpp UiSchema.cpp UiWorkspace.cpp UiDefinitions.cpp)
    target_include_directories(ui_smoke_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(ui_smoke_test PRIVATE SDL3::SDL3 SDL3_mixer::SDL3_mixer imgui::imgui Freetype::Freetype trformats ${RASAEDIT_SQLITE})
    set_property(TARGET ui_smoke_test PROPERTY CXX_STANDARD 20)
    set_property(TARGET viewport_test PROPERTY CXX_STANDARD 20)
    rasaedit_copy_mariadb_plugins(viewport_test)
    rasaedit_copy_mariadb_plugins(ui_smoke_test)
endif()
