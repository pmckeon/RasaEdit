# vcpkg triplet for RasaEdit on Linux: x64-linux (static libraries), except the LGPL libraries the editor links - MariaDB Connector/C
# and mpg123 (SDL3_mixer's MP3 decoder) - which are built as shared objects and shipped beside the executable, so they can be replaced
# (LGPL 2.1 section 6). CMakeLists.txt selects this triplet on Linux hosts unless VCPKG_TARGET_TRIPLET is given.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)

if (PORT MATCHES "^(libmariadb|mpg123)$")
    set(VCPKG_LIBRARY_LINKAGE dynamic)
endif()
