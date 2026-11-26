#!/usr/bin/env python3
"""Download Meta's real Windows Platform SDK runtime DLLs from Meta's own CDN.

Uses the same protocol as Meta's official Link installer (Setup.exe, "Daybreak"):
  1. GET graph.oculus.com/bootstrap_installer_config?channel_name=LIVE  (installer's public app token)
  2. For each application: GET manifest_uri -> zip containing manifest.json
  3. For each wanted file: GET segments_base_uri?segment_sha256=<hash>, zlib-inflate, verify sha256

Only files whose name matches WANTED are downloaded (a few MB, not the 4 GB Link install).

Usage: python3 fetch_meta_platform_dll.py [out_dir]   (default: ./meta-runtime)
"""
import hashlib, io, json, os, re, sys, urllib.parse, urllib.request, zipfile, zlib

TOKEN = "OC|1582076955407037|"  # public app token hard-coded in Meta's Setup.exe
CONFIG = "https://graph.oculus.com/bootstrap_installer_config"
WANTED = re.compile(r"(^|[\\/])LibOVR\w*64_1\.dll$", re.I)


def get(url, **params):
    params["access_token"] = TOKEN
    sep = "&" if "?" in url else "?"
    full = url + sep + urllib.parse.urlencode(params)
    req = urllib.request.Request(full, headers={"User-Agent": "Daybreak"})
    with urllib.request.urlopen(req, timeout=120) as r:
        return r.read()


def walk_apps(cfg):
    # field names are snake_case (manifest_uri, segments_base_uri, canonical_name)
    apps = cfg.get("applications") or cfg.get("apps") or []
    if isinstance(apps, dict):
        apps = apps.get("data") or list(apps.values())
    return apps


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "meta-runtime"
    os.makedirs(out, exist_ok=True)
    cfg = json.loads(get(CONFIG, channel_name="LIVE"))
    apps = walk_apps(cfg)
    if not apps:
        print("Unexpected config format; top-level keys:", list(cfg)[:20], file=sys.stderr)
        sys.exit(1)
    found = 0
    for app in apps:
        name = app.get("canonical_name", "?")
        try:
            mz = zipfile.ZipFile(io.BytesIO(get(app["manifest_uri"])))
            manifest = json.loads(mz.read("manifest.json"))
        except Exception as e:
            print(f"  skip {name}: {e}", file=sys.stderr)
            continue
        for fname, f in (manifest.get("files") or {}).items():
            if not WANTED.search(fname):
                continue
            size, segsize = f["size"], f["segmentSize"]
            data = bytearray()
            for i, (weak, strong, csize) in enumerate(f["segments"]):
                comp = get(app["segments_base_uri"], segment_sha256=strong)
                usize = segsize if i < len(f["segments"]) - 1 else (size - segsize * i)
                seg = zlib.decompress(comp)[:usize]
                if hashlib.sha256(seg).hexdigest() != strong.lower():
                    sys.exit(f"segment hash mismatch in {fname}")
                data += seg
            if hashlib.sha256(data).hexdigest() != f["sha256"].lower():
                sys.exit(f"file hash mismatch for {fname}")
            dest = os.path.join(out, name, fname.replace("\\", "/"))
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            open(dest, "wb").write(data)
            print(f"{dest}  ({len(data)} bytes, sha256 ok)")
            found += 1
    if not found:
        sys.exit("No LibOVRPlatform DLL found in any package.")


if __name__ == "__main__":
    main()
