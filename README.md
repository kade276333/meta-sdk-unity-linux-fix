# Meta XR Platform SDK fix for the Linux Unity Editor

> **Credits:** This tool was designed and written by **Claude** (Anthropic's AI model) using [Claude Code](https://claude.com/claude-code). [@kade276333](https://github.com/kade276333) requested it, tested it on their own setup, and published it here.

What it does: Meta's Platform SDK only ships a Windows DLL, so on Linux Unity throws
DllNotFoundException (ovrplatform / LibOVRPlatformImpl64_1) in Play mode. This installs a
small Linux library that forwards those calls to Meta's real DLL running under Wine.

Install:

```bash
    unzip meta-sdk-unity-linux-fix.zip
    cd meta-sdk-unity-linux-fix
    ./install.sh
```

It will:
  1. Install wine + python3 (+ gcc/mingw for rebuilding) with your distro's package manager
     (apt, dnf, yum, pacman, zypper, xbps, apk).
  2. Rebuild the bridge against the Meta Platform SDK version in your Unity projects
     (falls back to the bundled prebuilt binaries if it can't).
  3. Download Meta's runtime DLLs from Meta's own CDN into ~/.local/share/ovrplatform-linux
  4. Make a private Wine prefix there.
  5. Put libovrplatform.so in /usr/local/lib (sudo).
  6. Add a udev rule so Quest headsets show up over USB (if you don't have one).

Then restart Unity and press Play.

Options:  --prebuilt (skip rebuild)  --no-deps (don't touch packages)  --uninstall
Env vars: OVRPLATFORM_WINE=/path/to/wine   OVRPLATFORM_LINUX_DEBUG=1   OVRPLATFORM_LINUX_DIR=...

Tip: open your project in Unity once before installing, so the SDK package is in
Library/PackageCache and the bridge gets built for your exact SDK version. If you update
the Meta SDK later, run ./install.sh again.

## Credits

Written by Claude (Anthropic) with Claude Code, at the request of kade276333, who tested and published it. Not affiliated with or endorsed by Meta, Unity, or Valve.
