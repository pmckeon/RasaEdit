RasaEdit
========

A map and content editor for Tabula Rasa (client 1.16.5) and the Rasa.NET server emulator.

The CLIENT workspaces (Map, UI, Text, Assets) edit the game's own files: maps, lights and sounds, the UI layout, the
text tables in data/game.zip and the files in the game's archives. The SERVER workspace (World database) edits a Rasa.NET
World database (SQLite file, or MySQL / MariaDB). Edited game files keep a copy of the original beside them
(<name>.rasaedit-original) the first time they are saved.

Getting started
---------------
1. Run RasaEdit (RasaEdit.exe on Windows).
2. File > Open Game Folder... and choose the Tabula Rasa install (the folder with tabula_rasa.cfg).
3. For server content: File > Open Server Database... (rasaworld.db) or File > Connect to MySQL Database...

Settings and the window layout are kept in your user folder (Windows: %APPDATA%\RasaEdit\RasaEdit).

Requirements
------------
Windows: Windows 10 or later (64-bit) with a Vulkan or Direct3D 12 graphics driver. Everything else is in this folder.
Linux: a 64-bit distribution with glibc 2.38 or newer (Ubuntu 24.04, Debian 13, Fedora 39 or later), a Vulkan driver,
X11 or Wayland, PulseAudio / PipeWire or ALSA. For Korean, Japanese or Chinese text, a CJK font (e.g. fonts-noto-cjk).

Blender add-on
--------------
blender/rasaedit_addon.zip - in Blender: Edit > Preferences > Add-ons > (menu) Install from Disk..., choose the zip and
enable "RasaEdit Tabula Rasa models". Models exported from the Assets workspace's Blender panel open with it, and it checks
a model before exporting it back to RasaEdit. The add-on is licensed GPL-3.0-or-later.

Licence
-------
RasaEdit is released under the MIT licence (LICENSE.txt). The Blender add-on is GPL-3.0-or-later. The libraries RasaEdit is
built with keep their own licences (licenses/). On Linux, the LGPL libraries (MariaDB Connector/C: libmariadb.so.3, mpg123:
libmpg123.so) are separate files beside the editor and can be replaced with compatible builds.

Folder contents
---------------
RasaEdit(.exe)          the editor
*.dll / *.so.*          the libraries it loads (Windows: also the Visual C++ runtime)
plugins/libmariadb/     sign-in plugins for MySQL / MariaDB servers
blender/                the Blender add-on
LICENSE.txt             RasaEdit's licence (MIT)
licenses/               the licences of the libraries RasaEdit is built with
