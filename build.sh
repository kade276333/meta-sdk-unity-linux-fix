#!/usr/bin/env bash
# Build libovrplatform.so (Linux) + ovrbridge.exe (Wine) from the Platform SDK's C# declarations.
# Usage: ./build.sh [platform-package-dir ...]
#   default: every com.meta.xr.sdk.platform* found in Unity projects under $HOME
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build

if [ $# -eq 0 ]; then
  mapfile -t PKGS < <(find "$HOME" -maxdepth 6 -type d -path "*/Library/PackageCache/com.meta.xr.sdk.platform*" -prune 2>/dev/null)
  [ ${#PKGS[@]} -gt 0 ] || { echo "No com.meta.xr.sdk.platform package found; pass its path." >&2; exit 1; }
else
  PKGS=("$@")
fi

python3 -I gen_bridge.py build "${PKGS[@]}"

gcc -O2 -fPIC -shared -fvisibility=hidden -Wall -Wno-unused-variable -Isrc \
    src/bridge_common.c src/linux_client.c build/gen_linux.c \
    -o build/libovrplatform.so -lpthread -ldl
x86_64-w64-mingw32-gcc -O2 -Wall -Wno-unused-variable -Isrc \
    src/bridge_common.c src/win_server.c build/gen_win.c \
    -o build/ovrbridge.exe -static
echo "built: build/libovrplatform.so build/ovrbridge.exe"
