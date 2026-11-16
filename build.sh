#!/usr/bin/env bash
# Builds UE4SS-Launcher.exe with MinGW-w64 (Linux, WSL or MSYS2).
# Needs: x86_64-w64-mingw32-g++ / gcc / windres, and Dear ImGui (fetched below into ./imgui).
set -e
cd "$(dirname "$0")"
IMGUI=${IMGUI:-imgui}
[ -f "$IMGUI/imgui.h" ] || git clone --depth 1 --branch v1.91.9 https://github.com/ocornut/imgui.git "$IMGUI"
mkdir -p obj
CXX="x86_64-w64-mingw32-g++ -O2 -std=c++17 -municode -DUNICODE -D_UNICODE -DIMGUI_USER_CONFIG=\"imgui_user_config.h\" -I. -I$IMGUI -I$IMGUI/backends"
for f in imgui imgui_draw imgui_tables imgui_widgets; do [ -f obj/$f.o ] || $CXX -c $IMGUI/$f.cpp -o obj/$f.o; done
for f in imgui_impl_win32 imgui_impl_dx11; do [ -f obj/$f.o ] || $CXX -c $IMGUI/backends/$f.cpp -o obj/$f.o; done
x86_64-w64-mingw32-g++ -O2 -shared -static -s -municode -DUNICODE input_block.cpp -o res/LauncherInput.dll
x86_64-w64-mingw32-windres app.rc -O coff -o obj/app.res
for f in core bridge updater unity classic; do $CXX -Wall -Wno-unknown-pragmas -c $f.cpp -o obj/$f.o; done
$CXX -Wno-unknown-pragmas -Ithird_party/acl -Ithird_party/rtm -c models.cpp -o obj/models.o
for f in simplifier allocator; do [ -f obj/mo_$f.o ] || $CXX -c third_party/meshopt/$f.cpp -o obj/mo_$f.o; done
x86_64-w64-mingw32-gcc -O2 -c puff.c -o obj/puff.o
[ -f obj/ufbx.o ] || x86_64-w64-mingw32-gcc -O2 -c third_party/ufbx/ufbx.c -o obj/ufbx.o
$CXX -Wall -Wno-unknown-pragmas -c app.cpp -o obj/app.o
LIBS="-ld3d11 -ld3dcompiler_47 -ldxgi -ldwmapi -lcomdlg32 -lshlwapi -lshell32 -lole32 -luuid -lgdi32 -limm32 -lwinhttp -lbcrypt -lwinmm -lwindowscodecs -ldxguid -s"
$CXX -mwindows -static obj/*.o obj/app.res -o UE4SS-Launcher.exe $LIBS
ls -la UE4SS-Launcher.exe
