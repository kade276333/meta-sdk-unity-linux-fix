#!/usr/bin/env bash
# Meta XR Platform SDK fix for the Linux Unity Editor.
# Makes DllImport("ovrplatform") / ("LibOVRPlatformImpl64_1") work in Play mode by bridging
# to Meta's real Windows Platform DLL running under Wine.
#
# Works on Debian/Ubuntu/Mint/Pop, Fedora/RHEL, Arch/Manjaro/SteamOS, openSUSE, Void, Alpine*
# (x86_64 only, since the Unity Editor is x86_64 only).
#
# Usage:  ./install.sh              install (rebuilds against your projects' SDK if it can)
#         ./install.sh --prebuilt   skip rebuilding, use the bundled binaries
#         ./install.sh --no-deps    don't try to install packages
#         ./install.sh --uninstall
set -uo pipefail
cd "$(dirname "$(readlink -f "$0")")"
DEST="$HOME/.local/share/ovrplatform-linux"
LIB=/usr/local/lib
PREBUILT_ONLY=0; NODEPS=0

say()  { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m!!\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31mxx\033[0m %s\n' "$*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }
SUDO=""; [ "$(id -u)" -ne 0 ] && SUDO="sudo"

for a in "$@"; do case "$a" in
  --prebuilt) PREBUILT_ONLY=1 ;;
  --no-deps)  NODEPS=1 ;;
  --uninstall)
    say "Removing..."
    rm -rf "$DEST"
    $SUDO rm -f "$LIB/libovrplatform.so" "$LIB/libLibOVRPlatformImpl64_1.so" /etc/ld.so.conf.d/ovrplatform-linux.conf
    $SUDO ldconfig; say "Uninstalled."; exit 0 ;;
  -h|--help) sed -n '2,13p' "$0"; exit 0 ;;
  *) die "unknown option $a" ;;
esac; done

[ "$(uname -m)" = "x86_64" ] || die "Unity Editor on Linux is x86_64 only (this is $(uname -m))."
[ "$(id -u)" -eq 0 ] && warn "Running as root: files will go to root's home. Run as your normal user instead."

# ---------- 1. dependencies ----------
install_pkgs() {   # $1 = runtime pkgs, $2 = build pkgs (best effort)
  if   have apt-get; then
    $SUDO dpkg --add-architecture i386 2>/dev/null || true
    $SUDO apt-get update -qq
    $SUDO apt-get install -y python3 wine64 2>/dev/null || $SUDO apt-get install -y python3 wine
    $SUDO apt-get install -y gcc gcc-mingw-w64-x86-64 || true
  elif have dnf;    then $SUDO dnf install -y python3 wine;          $SUDO dnf install -y gcc mingw64-gcc || true
  elif have yum;    then $SUDO yum install -y python3 wine;          $SUDO yum install -y gcc mingw64-gcc || true
  elif have pacman; then
    grep -q '^\[multilib\]' /etc/pacman.conf || warn "Arch: enable [multilib] in /etc/pacman.conf if wine fails to install."
    $SUDO pacman -S --needed --noconfirm python wine;               $SUDO pacman -S --needed --noconfirm gcc mingw-w64-gcc || true
  elif have zypper; then $SUDO zypper -n install python3 wine;      $SUDO zypper -n install gcc mingw64-cross-gcc || true
  elif have xbps-install; then $SUDO xbps-install -Sy python3 wine; $SUDO xbps-install -y gcc cross-x86_64-w64-mingw32 || true
  elif have apk;    then $SUDO apk add python3 wine;                 $SUDO apk add gcc musl-dev mingw-w64-gcc || true
  elif have emerge; then warn "Gentoo: emerge app-emulation/wine-vanilla dev-lang/python yourself."
  elif have nix-env; then warn "NixOS: add wine64 + python3 to your config (and see README for the lib path)."
  else warn "Unknown package manager: install python3 and 64-bit wine yourself."
  fi
}

need_rt=0; have python3 || need_rt=1; (have wine64 || have wine) || need_rt=1
if [ $NODEPS -eq 0 ] && { [ $need_rt -eq 1 ] || { [ $PREBUILT_ONLY -eq 0 ] && ! have x86_64-w64-mingw32-gcc; }; }; then
  say "Installing dependencies (wine, python3, compilers)..."
  install_pkgs
fi
have python3 || die "python3 is required."
WINE=""; for w in wine wine64; do have $w && { WINE=$(command -v $w); break; }; done
[ -n "$WINE" ] || die "Wine not found. Install 64-bit Wine and re-run."
say "Using Wine: $WINE ($("$WINE" --version 2>/dev/null))"

# ---------- 2. build (or use prebuilt) ----------
mkdir -p build
SO=prebuilt/libovrplatform.so; EXE=prebuilt/ovrbridge.exe
if [ $PREBUILT_ONLY -eq 0 ] && have gcc && have x86_64-w64-mingw32-gcc; then
  mapfile -t PKGS < <(find "$HOME" -maxdepth 7 -type d -path "*/Library/PackageCache/com.meta.xr.sdk.platform*" -prune 2>/dev/null)
  if [ ${#PKGS[@]} -gt 0 ]; then
    say "Rebuilding against ${#PKGS[@]} Meta Platform SDK package(s) found in your Unity projects..."
    if ./build.sh "${PKGS[@]}"; then SO=build/libovrplatform.so; EXE=build/ovrbridge.exe
    else warn "Rebuild failed, falling back to bundled binaries."; fi
  else
    warn "No Unity project with com.meta.xr.sdk.platform found under \$HOME (open your project in Unity once first). Using bundled binaries."
  fi
else
  say "Using bundled prebuilt binaries."
fi

# ---------- 3. Meta's runtime DLLs ----------
mkdir -p "$DEST"
if ! ls "$DEST"/LibOVRPlatformImpl64_1.dll >/dev/null 2>&1; then
  say "Downloading Meta's Platform runtime DLLs from Meta's CDN (a few MB)..."
  python3 -I fetch_meta_platform_dll.py "$PWD/meta-runtime" || die "Download failed (check internet)."
  find meta-runtime -iname 'LibOVR*.dll' -exec cp {} "$DEST/" \;
fi
ls "$DEST"/LibOVRPlatformImpl64_1.dll >/dev/null 2>&1 || die "LibOVRPlatformImpl64_1.dll missing after download."
cp "$EXE" "$DEST/ovrbridge.exe"

# ---------- 4. Wine prefix ----------
if [ ! -d "$DEST/prefix/drive_c" ]; then
  say "Creating private Wine prefix (one time, can take a minute)..."
  WINEPREFIX="$DEST/prefix" WINEARCH=win64 WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml=" \
    "$WINE" wineboot -i >/dev/null 2>&1 || warn "wineboot reported an error; continuing."
fi
# Tell the bridge which wine to use if it isn't plain "wine" on PATH
if [ "$(basename "$WINE")" != "wine" ]; then
  warn "Your wine binary is '$WINE'. Add this to your shell profile / Unity launcher:"
  echo "    export OVRPLATFORM_WINE=$WINE"
fi

# ---------- 5. system library (what Unity's Mono dlopen finds) ----------
say "Installing libovrplatform.so to $LIB (needs sudo)..."
$SUDO install -Dm755 "$SO" "$LIB/libovrplatform.so"
# Platform SDK v203+ imports LibOVRPlatformImpl64_1 directly (hzpsdk_* API)
$SUDO ln -sf libovrplatform.so "$LIB/libLibOVRPlatformImpl64_1.so"
# Fedora/Arch/openSUSE don't search /usr/local/lib by default
if ! ldconfig -p 2>/dev/null | grep -q "$LIB/libovrplatform.so"; then
  echo "$LIB" | $SUDO tee /etc/ld.so.conf.d/ovrplatform-linux.conf >/dev/null
fi
$SUDO ldconfig

# ---------- 6. optional: Quest USB access for adb / Build And Run ----------
if [ -d /etc/udev/rules.d ] && [ ! -f /etc/udev/rules.d/51-oculus-quest.rules ]; then
  say "Adding udev rule so Unity/adb can see Quest headsets over USB..."
  getent group plugdev >/dev/null || $SUDO groupadd -r plugdev 2>/dev/null || true
  $SUDO usermod -aG plugdev "$USER" 2>/dev/null || true
  echo 'SUBSYSTEM=="usb", ATTR{idVendor}=="2833", MODE="0666", GROUP="plugdev"' \
    | $SUDO tee /etc/udev/rules.d/51-oculus-quest.rules >/dev/null
  $SUDO udevadm control --reload-rules 2>/dev/null && $SUDO udevadm trigger 2>/dev/null || true
fi

# ---------- 7. smoke test ----------
if have gcc; then
  gcc -O0 test/smoke.c -o build/smoke -ldl 2>/dev/null && \
    { say "Smoke test:"; ./build/smoke libovrplatform.so || warn "Smoke test failed (set OVRPLATFORM_LINUX_DEBUG=1 for logs)."; }
fi

echo
say "Done. Restart the Unity Editor, then enter Play mode."
echo "    Debug logs:  OVRPLATFORM_LINUX_DEBUG=1 unityhub   (or your Unity launcher)"
echo "    Uninstall:   ./install.sh --uninstall"
