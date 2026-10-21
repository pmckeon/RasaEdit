cmake_minimum_required (VERSION 3.16)

# Enable Hot Reload for MSVC compilers if supported.
if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

# Linux: static vcpkg libraries except the LGPL ones (triplets/x64-linux-rasaedit.cmake). Set before project(), where vcpkg's
# toolchain reads it; a VCPKG_TARGET_TRIPLET given on the command line wins.
if (CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND NOT DEFINED VCPKG_TARGET_TRIPLET)
    set(VCPKG_TARGET_TRIPLET "x64-linux-rasaedit" CACHE STRING "vcpkg triplet")
    set(VCPKG_OVERLAY_TRIPLETS "${CMAKE_CURRENT_SOURCE_DIR}/triplets" CACHE STRING "vcpkg triplet overlays")
endif()

project ("RasaEdit" VERSION 0.1.0)

# Find the dependencies from vcpkg
find_package(SDL3 CONFIG REQUIRED)
find_package(SDL3_mixer CONFIG REQUIRED)       # asset preview sounds: WAV, Ogg Vorbis (libvorbis), MP3 (mpg123)
find_package(imgui CONFIG REQUIRED)
find_package(Freetype REQUIRED)               # UI text (UiCanvas): hinted glyphs like the game's GDI ones
# stb_image (header-only): the asset preview's .tga / .jpg images
find_path(STB_INCLUDE_DIRS "stb_image.h" REQUIRED)
include_directories(${STB_INCLUDE_DIRS})
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
    "src/RasaEdit.cpp" "src/RasaEdit.h"
    "src/Editor.cpp" "src/Editor.h"
    "src/EditorContext.cpp" "src/EditorContext.h"
    "src/Workspace.h"
    "src/ClientWorkspace.cpp" "src/ClientWorkspace.h"
    "src/MissionEditor.cpp" "src/MissionEditor.h" "src/Lookups.cpp" "src/Lookups.h" "src/ModelExchange.cpp" "src/ModelExchange.h" "src/ModelImport.cpp" "src/ModelMath.h" "src/BlenderPanel.cpp" "src/BlenderPanel.h" "src/AssetsWorkspace.cpp" "src/AssetsWorkspace.h" "src/ModelAnimations.cpp" "src/ModelAnimations.h"
    "src/MissionValidation.cpp" "src/MissionValidation.h" "src/Migrations.cpp" "src/Migrations.h"
    "src/Json.cpp" "src/Json.h"
    "src/LootEditor.cpp" "src/LootEditor.h"
    "src/ModuleEditor.cpp" "src/ModuleEditor.h"
    "src/RecipeEditor.cpp" "src/RecipeEditor.h"
    "src/ServerWorkspace.cpp" "src/ServerWorkspace.h"
    "src/Panels.cpp" "src/Panels.h"
    "src/AssetPreview.cpp" "src/AssetPreview.h"
    "src/SoundPlayer.cpp" "src/SoundPlayer.h"
    "src/TerrainOverview.cpp" "src/TerrainOverview.h"
    "src/RadarMap.cpp" "src/RadarMap.h"
    "src/GpuTexture.cpp" "src/GpuTexture.h"
    "src/Renderer.cpp" "src/Renderer.h"
    "src/SceneView.cpp" "src/SceneView.h"
    "src/PvsBuilder.cpp" "src/PvsBuilder.h"
    "src/ClientPvs.cpp"
    "src/ClientCollision.cpp" "src/Collision.cpp" "src/Collision.h" "src/GameConfig.cpp" "src/GameConfig.h" "src/RasaNetMapUsables.cpp" "src/RasaNetMapUsables.h"
    "src/ParticleSim.cpp" "src/ParticleSim.h" "src/Beams.cpp" "src/Beams.h"
    "src/GpuMath.h" "src/MarkerColours.h" "src/ShaderBytecode.h"
    "src/WorldDatabase.cpp" "src/WorldDatabase.h"
    "src/DbConnection.cpp" "src/DbConnection.h"
    "src/Avatar.cpp" "src/Avatar.h"
    "src/UiCanvas.cpp" "src/UiCanvas.h"
    "src/UiSchema.cpp" "src/UiSchema.h"
    "src/UiWorkspace.cpp" "src/UiWorkspace.h" "src/TextWorkspace.cpp" "src/TextWorkspace.h"
    "src/UiDefinitions.cpp"
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
# Linux: the shared LGPL libraries (libmariadb, libmpg123) are looked for beside the executable once installed.
if (UNIX AND NOT APPLE)
    set_target_properties(RasaEdit PROPERTIES INSTALL_RPATH "$ORIGIN")
endif()

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
    add_executable(viewport_test tests/viewport_test.cpp src/Renderer.cpp src/SceneView.cpp src/PvsBuilder.cpp src/ParticleSim.cpp src/Beams.cpp src/GpuTexture.cpp src/WorldDatabase.cpp src/Migrations.cpp src/DbConnection.cpp src/Avatar.cpp src/UiCanvas.cpp src/UiSchema.cpp src/MissionValidation.cpp src/Json.cpp src/ModelExchange.cpp src/ModelImport.cpp src/GameConfig.cpp src/RasaNetMapUsables.cpp)
    target_include_directories(viewport_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(viewport_test PRIVATE SDL3::SDL3 Freetype::Freetype trformats ${RASAEDIT_SQLITE})

    # Whole-editor smoke test: real ImGui frames and both workspaces, headless.
    add_executable(ui_smoke_test tests/ui_smoke_test.cpp src/Editor.cpp src/EditorContext.cpp src/ClientWorkspace.cpp src/ClientPvs.cpp src/ClientCollision.cpp src/Collision.cpp src/GameConfig.cpp src/RasaNetMapUsables.cpp src/ServerWorkspace.cpp src/MissionEditor.cpp src/Lookups.cpp src/ModelExchange.cpp src/ModelImport.cpp src/BlenderPanel.cpp src/AssetsWorkspace.cpp src/ModelAnimations.cpp src/MissionValidation.cpp src/Json.cpp src/LootEditor.cpp src/ModuleEditor.cpp src/RecipeEditor.cpp src/Panels.cpp src/AssetPreview.cpp src/SoundPlayer.cpp
                   src/Renderer.cpp src/SceneView.cpp src/PvsBuilder.cpp src/ParticleSim.cpp src/Beams.cpp src/GpuTexture.cpp src/WorldDatabase.cpp src/Migrations.cpp src/DbConnection.cpp src/Avatar.cpp src/TerrainOverview.cpp src/RadarMap.cpp src/UiCanvas.cpp src/UiSchema.cpp src/UiWorkspace.cpp src/UiDefinitions.cpp src/TextWorkspace.cpp)
    target_include_directories(ui_smoke_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(ui_smoke_test PRIVATE SDL3::SDL3 SDL3_mixer::SDL3_mixer imgui::imgui Freetype::Freetype trformats ${RASAEDIT_SQLITE})
    set_property(TARGET ui_smoke_test PROPERTY CXX_STANDARD 20)
    set_property(TARGET viewport_test PROPERTY CXX_STANDARD 20)
    rasaedit_copy_mariadb_plugins(viewport_test)
    rasaedit_copy_mariadb_plugins(ui_smoke_test)
endif()

# ------------------------------------------------------------------------------------------
# Distribution: only RasaEdit and the Blender add-on (no tests, tools or scripts).
#   cmake --install out/build/release --prefix <folder>      the distributable folder
#   cmake --build out/build/release --target package         the same as RasaEdit-<version>-<platform>.zip / .tar.gz
# Build the release preset first; the package takes the vcpkg release libraries.
# ------------------------------------------------------------------------------------------
if (DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    set(RASAEDIT_VCPKG "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
endif()
if (WIN32 AND RASAEDIT_VCPKG AND NOT CMAKE_VERSION VERSION_LESS 3.21)
    # The executable with the DLLs it loads, found through its imports in vcpkg's bin folder (SDL3_mixer's codecs included);
    # Windows' own DLLs and the API sets stay out.
    install(TARGETS RasaEdit
            RUNTIME_DEPENDENCIES
                PRE_EXCLUDE_REGEXES "^api-ms-" "^ext-ms-"
                POST_EXCLUDE_REGEXES ".*[Ss][Yy][Ss][Tt][Ee][Mm]32.*" ".*[Ss][Yy][Ss][Ww][Oo][Ww]64.*"
                DIRECTORIES "${RASAEDIT_VCPKG}/bin"
            RUNTIME DESTINATION .)
    # The Visual C++ runtime (msvcp140, vcruntime140, vcruntime140_1) beside the executable, so a PC with an older runtime works.
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION .)
    include(InstallRequiredSystemLibraries)
elseif (RASAEDIT_VCPKG AND NOT CMAKE_VERSION VERSION_LESS 3.21)
    # Linux: the binary plus the shared libraries it takes from vcpkg (the LGPL ones, see the triplet); the system's libraries
    # (glibc, libstdc++, ...) stay out.
    install(TARGETS RasaEdit
            RUNTIME_DEPENDENCIES
                POST_INCLUDE_REGEXES ".*vcpkg_installed.*"
                POST_EXCLUDE_REGEXES ".*"
                DIRECTORIES "${RASAEDIT_VCPKG}/lib"
            RUNTIME DESTINATION .
            LIBRARY DESTINATION .)
else()
    install(TARGETS RasaEdit RUNTIME DESTINATION .)
endif()
if (RASAEDIT_VCPKG)
    # MariaDB Connector/C's authentication plugins (MySQL / MariaDB World databases), where DbConnection points the connector.
    if (EXISTS "${RASAEDIT_VCPKG}/plugins/libmariadb")
        install(DIRECTORY "${RASAEDIT_VCPKG}/plugins/libmariadb/" DESTINATION plugins/libmariadb)
    endif()
    # Every installed library's copyright / licence file, as licenses/<port>.txt - vcpkg's own helpers and build-only tools left out.
    file(GLOB RASAEDIT_COPYRIGHTS "${RASAEDIT_VCPKG}/share/*/copyright")
    foreach (copyright IN LISTS RASAEDIT_COPYRIGHTS)
        get_filename_component(port_dir "${copyright}" DIRECTORY)
        get_filename_component(port "${port_dir}" NAME)
        if (NOT port MATCHES "^(vcpkg-.*|gperf|yasm|nasm)$")
            install(FILES "${copyright}" DESTINATION licenses RENAME "${port}.txt")
        endif()
    endforeach()
endif()
install(FILES "packaging/README.txt" DESTINATION .)
install(FILES "LICENSE" DESTINATION . RENAME "LICENSE.txt")

# The Blender add-on as blender/rasaedit_addon.zip (Blender: Edit > Preferences > Add-ons > Install from Disk): the add-on folder's
# files at the zip's top level (the extension format; tools/blender/make_addon_zip.py makes the same zip).
file(GLOB RASAEDIT_ADDON_FILES CONFIGURE_DEPENDS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/tools/blender/rasaedit_addon"
     "${CMAKE_CURRENT_SOURCE_DIR}/tools/blender/rasaedit_addon/*.py" "${CMAKE_CURRENT_SOURCE_DIR}/tools/blender/rasaedit_addon/*.toml")
install(CODE "
    set(addon_out \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/blender\")
    file(MAKE_DIRECTORY \"\${addon_out}\")
    execute_process(COMMAND \"${CMAKE_COMMAND}\" -E tar cf \"\${addon_out}/rasaedit_addon.zip\" --format=zip ${RASAEDIT_ADDON_FILES}
                    WORKING_DIRECTORY \"${CMAKE_CURRENT_SOURCE_DIR}/tools/blender/rasaedit_addon\" RESULT_VARIABLE addon_result)
    if (addon_result)
        message(FATAL_ERROR \"packing the Blender add-on failed: \${addon_result}\")
    endif()
    message(STATUS \"Installing: \${addon_out}/rasaedit_addon.zip\")
")

if (WIN32)
    set(RASAEDIT_PLATFORM "windows-x64")
    set(CPACK_GENERATOR "ZIP")
else()
    string(TOLOWER "${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}" RASAEDIT_PLATFORM)
    set(CPACK_GENERATOR "TGZ")
endif()
set(CPACK_PACKAGE_NAME "RasaEdit")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME "RasaEdit-${PROJECT_VERSION}-${RASAEDIT_PLATFORM}")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/package")
include(CPack)
