#!/usr/bin/env bash
# Builds guide/build/FH2MPGuide.dll, the in-game Guide (a FH2 Mod Platform mod for fh2-mods/): Dear ImGui 1.92.9b with its DX12 backend
# (third_party/imgui), stb_image (third_party/stb), Material Icons embedded (third_party/material-icons), libopus
# linked statically for voice (MSYS2: pacman -S mingw-w64-x86_64-opus). Needs a mingw-w64 g++ (MSYS2
# mingw64 on Windows: PATH with C:\msys64\mingw64\bin; CXX=x86_64-w64-mingw32-g++ on Linux).
#   build.sh           the mod
#   build.sh preview   guide/build/guide-preview.exe: the same drawing off screen into a PNG (tests/preview.cpp)
#   build.sh voicetest guide/build/voice-test.exe: the voice engine in a loopback without a game (tests/voice_test.cpp)
set -euo pipefail
cd "$(dirname "$0")"
CXX=${CXX:-g++}
OBJCOPY=${OBJCOPY:-objcopy}
mkdir -p build
root=..
PLATFORM=$(tr -d '[:space:]' < "$root/third_party/fh2-mod-platform/VERSION")
imgui=$root/third_party/imgui
# the icon font as an object file: symbols _binary_MaterialIcons_Regular_ttf_start/_end
(cd "$root/third_party/material-icons" && "$OBJCOPY" -I binary -O pe-x86-64 -B i386:x86-64 MaterialIcons-Regular.ttf "$OLDPWD/build/icons.o")
flags=(-O2 -std=c++17 -static -static-libgcc -static-libstdc++ -DWIDL_EXPLICIT_AGGREGATE_RETURNS -DNOMINMAX
       -Wall -Wno-unused-function -Wno-missing-field-initializers
       -I src -I "$imgui" -I "$root/third_party/fh2-mod-platform" -I "$root/third_party/stb")
common=(src/guide.cpp src/theme.cpp src/theme_page.cpp src/voice_page.cpp src/community.cpp src/messages.cpp src/achievements.cpp src/assets.cpp src/gfx.cpp src/net.cpp src/probe.cpp src/join.cpp src/sysui.cpp src/d3dcompile.cpp
        "$imgui/imgui.cpp" "$imgui/imgui_draw.cpp" "$imgui/imgui_tables.cpp" "$imgui/imgui_widgets.cpp" "$imgui/backends/imgui_impl_dx12.cpp"
        build/icons.o)
libs=(-ld3d12 -ldxgi -ldxguid -luuid -lole32 -luser32 -lgdi32 -lwinhttp)
if [ "${1:-}" = voicetest ]; then
    "$CXX" "${flags[@]}" -o build/voice-test.exe tests/voice_test.cpp -l:libopus.a -luuid
    echo "built: guide/build/voice-test.exe"
    exit 0
fi
if [ "${1:-}" = preview ]; then
    "$CXX" "${flags[@]}" -o build/guide-preview.exe tests/preview.cpp "${common[@]}" "${libs[@]}" -lwindowscodecs
    echo "built: guide/build/guide-preview.exe"
    exit 0
fi
"$CXX" "${flags[@]}" -s -shared -DFH2MOD_PLATFORM_VERSION="\"$PLATFORM\"" -o build/FH2MPGuide.dll src/overlay.cpp src/voice.cpp "${common[@]}" "${libs[@]}" -l:libopus.a
echo "built: guide/build/FH2MPGuide.dll"
