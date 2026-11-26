#!/usr/bin/env python3
"""Generate the two halves of the ovrplatform Linux<->Wine bridge from the Unity
Platform SDK's own C# P/Invoke declarations.

  gen_bridge.py <outdir> <platform-package-dir> [more package dirs...]

Writes:
  <outdir>/gen_linux.c   exports every ovr_* symbol as libovrplatform.so; each call is
                         serialized and sent to the Wine helper.
  <outdir>/gen_win.c     dispatch table for ovrbridge.exe; deserializes, calls the real
                         LibOVRPlatformImpl64_1.dll export, serializes the result.

Both sides share function ids and argument encodings, so they must come from the same run.
"""
import pathlib, re, sys

# ---------------------------------------------------------------- parse C#
DECL = re.compile(
    r"\[DllImport\(([^\]]*)\)\]\s*((?:(?:public|private|internal|static|extern|unsafe)\s+)+)"
    r"([\w\.]+(?:\[\])?)\s+(\w+)\s*\(([^)]*)\)\s*;", re.S)
WRAP = re.compile(r"public static ([\w\.\[\]<>]+) (\w+)\(([^)]*)\)\s*\{(.*?)\n    \}", re.S)

decls = {}        # entry -> (ret, [(type, name)], csname)
wrappers = {}     # public wrapper name -> (params, body)
for root in sys.argv[2:]:
    for cs in sorted(pathlib.Path(root).rglob("*.cs")):
        src = cs.read_text(errors="ignore")
        for attr, mods, ret, csname, params in DECL.findall(src):
            m = re.search(r'EntryPoint\s*=\s*"(\w+)"', attr)
            entry = m.group(1) if m else csname
            ps = []
            for p in [x.strip() for x in params.split(",") if x.strip()]:
                p = re.sub(r"\[[A-Za-z][^\]]*\]\s*", "", p)  # drop [MarshalAs] etc., keep T[]
                toks = p.split()
                ps.append((" ".join(toks[:-1]), toks[-1]))
            decls.setdefault(entry, (ret, ps, csname))
        for ret, name, params, body in WRAP.findall(src):
            wrappers.setdefault(name, (params, body))

def wrapper_param_types(entry):
    w = wrappers.get(entry)
    if not w:
        return {}
    out = {}
    for p in [x.strip() for x in w[0].split(",") if x.strip()]:
        toks = p.split()
        out[toks[-1]] = " ".join(toks[:-1])
    return out

# ---------------------------------------------------------------- types
SCALAR = {
    "int": "int32_t", "Int32": "int32_t", "uint": "uint32_t", "UInt32": "uint32_t",
    "long": "int64_t", "Int64": "int64_t", "ulong": "uint64_t", "UInt64": "uint64_t",
    "IntPtr": "uintptr_t", "UIntPtr": "uintptr_t", "bool": "int32_t",
    "float": "float", "Single": "float", "double": "double", "Double": "double",
    "short": "int16_t", "Int16": "int16_t", "byte": "uint8_t",
}
ELEM = {"byte": 1, "float": 4, "Int16": 2, "short": 2, "UInt32": 4, "uint": 4,
        "UInt64": 8, "ulong": 8, "IntPtr": 8}
CELEM = {"byte": "uint8_t", "float": "float", "Int16": "int16_t", "short": "int16_t",
         "UInt32": "uint32_t", "uint": "uint32_t", "UInt64": "uint64_t", "ulong": "uint64_t",
         "IntPtr": "const char *"}
LEN_HINT = re.compile(r"(count|length|len|size|num|elements)", re.I)

def ctype_of(t):
    if t in SCALAR:
        return SCALAR[t]
    return "int32_t"  # enums

def wire_of(ctype):
    return {"float": "f32", "double": "f64"}.get(ctype, "u64")

class Arg:
    pass

def classify(entry, ret, params):
    """Return (args, retkind) or None if the function can't be forwarded."""
    wtypes = wrapper_param_types(entry) if entry != decls[entry][2] else {}
    args = []
    for i, (t, n) in enumerate(params):
        a = Arg(); a.name = n or f"a{i}"; a.t = t
        if t == "string" or (t == "IntPtr" and wtypes.get(n) == "string") or \
                (entry == "ovr_Log_NewEvent" and n == "eventName"):
            a.kind, a.ctype = "str", "const char *"
        elif t.startswith("ref "):
            inner = t[4:]
            a.kind = "ref"
            a.ctype = {"float": "float *", "ovrNetSyncVec3": "float *"}.get(inner)
            a.size = {"float": 4, "ovrNetSyncVec3": 12}.get(inner)
            if inner == "OculusInitParams":
                a.kind, a.ctype = "initparams", "const ovr_init_params *"
            elif a.ctype is None:
                return None
        elif t.endswith("[]"):
            el = t[:-2]
            # find the length parameter: next scalar param whose name looks like a count
            ln = None
            for j in range(i + 1, len(params)):
                if params[j][0] in SCALAR and LEN_HINT.search(params[j][1]):
                    ln = j; break
            if ln is None:
                for j in range(i - 1, -1, -1):
                    if params[j][0] in SCALAR and LEN_HINT.search(params[j][1]):
                        ln = j; break
            a.lenidx = ln
            if el == "ovrKeyValuePair":
                a.kind, a.ctype = "kvp", "const ovr_kvp *"
            elif el == "string" or (el == "IntPtr" and entry == "ovr_Log_NewEvent"):
                a.kind, a.ctype = "strarr", "const char * const *"
                a.mult = 2 if entry == "ovr_Log_NewEvent" else 1
            elif el in ELEM:
                a.kind = "out" if re.search(r"output|timestamp", n, re.I) else "in"
                a.esize, a.ctype = ELEM[el], CELEM[el] + (" *" if a.kind == "out" else " const *")
                if a.kind == "out":
                    a.ctype = CELEM[el] + " *"
            else:
                return None
            if a.lenidx is None:
                a.kind = "nulled"  # no length to size it: pass NULL
        elif t.endswith("Callback") or t == "FilterCallback":
            a.kind, a.ctype = "nulled", "void *"
        else:
            a.kind, a.ctype = "scalar", ctype_of(t)
        args.append(a)
    if ret == "void":
        rk = ("void", None)
    elif ret == "string":
        rk = ("rstr", "const char *")
    elif ret == "IntPtr":
        body = ""
        for w in wrappers.values():
            if entry + "_Native(" in w[1] or entry + "(" in w[1]:
                pass
        csname = decls[entry][2]
        pub = wrappers.get(entry)
        if pub and re.search(r"StringFromNative\(\s*" + re.escape(csname) + r"\(", pub[1]):
            rk = ("rstr", "const char *")
        elif pub and re.search(r"BlobFromNative\([^;]*" + re.escape(csname) + r"\(", pub[1]):
            rk = ("rblob", "const void *")
        elif entry == "ovr_Packet_GetBytes":
            rk = ("rblob", "const void *")
        elif entry in RET_STR:
            rk = ("rstr", "const char *")
        else:
            rk = ("scalar", "uintptr_t")
    elif ret.endswith("[]"):
        return None
    else:
        rk = ("scalar", ctype_of(ret))
    return args, rk

# IntPtr returns that are C strings even though no ovr_* style wrapper shows it.
# SDK v203+ (Horizon Platform SDK) calls these on LibOVRPlatformImpl64_1 directly from WindowsClient.cs.
RET_STR = {"hzpsdk_PopMessage"}

BLOB_SIZE = {
    "ovr_Packet_GetBytes": "ovr_Packet_GetSize",
    "ovr_LeaderboardEntry_GetExtraData": "ovr_LeaderboardEntry_GetExtraDataLength",
    "ovr_ChallengeEntry_GetExtraData": "ovr_ChallengeEntry_GetExtraDataLength",
}

# ---------------------------------------------------------------- emit
funcs = []
skipped = []
for entry in sorted(decls):
    ret, params, _ = decls[entry]
    c = classify(entry, ret, params)
    if c is None:
        skipped.append(entry); continue
    funcs.append((entry, c[0], c[1]))

def cret(rk):
    k, ct = rk
    return "void" if k == "void" else ct

def proto(entry, args, rk):
    ps = ", ".join(f"{a.ctype} {a.name}" for a in args) or "void"
    return f"{cret(rk)} {entry}({ps})"

L = ['/* GENERATED by gen_bridge.py - do not edit */', '#include "bridge_common.h"', '#include "linux_client.h"', ""]
W = ['/* GENERATED by gen_bridge.py - do not edit */', '#include "bridge_common.h"', '#include "win_server.h"', ""]

for fid, (entry, args, rk) in enumerate(funcs, start=1):
    # ---- Linux export
    L.append(f"EXPORT {proto(entry, args, rk)} {{")
    L.append(f"  wbuf *w = cl_begin({fid});")
    for a in args:
        if a.kind == "scalar":
            L.append(f"  wb_{wire_of(a.ctype)}(w, {a.name});")
        elif a.kind == "str":
            L.append(f"  wb_str(w, {a.name});")
        elif a.kind == "ref":
            L.append(f"  wb_bytes(w, {a.name}, {a.name} ? {a.size} : 0);")
        elif a.kind == "initparams":
            L.append(f"  wb_initparams(w, {a.name});")
        elif a.kind == "kvp":
            L.append(f"  wb_kvp(w, {a.name}, (size_t){args[a.lenidx].name});")
        elif a.kind == "strarr":
            L.append(f"  wb_strarr(w, {a.name}, (size_t){args[a.lenidx].name} * {a.mult});")
        elif a.kind == "in":
            L.append(f"  wb_bytes(w, {a.name}, {a.name} ? (size_t){args[a.lenidx].name} * {a.esize} : 0);")
        elif a.kind == "out":
            L.append(f"  wb_u64(w, {a.name} ? (uint64_t){args[a.lenidx].name} * {a.esize} : 0);")
    L.append("  rbuf *r = cl_call(w);")
    for a in args:
        if a.kind == "ref":
            L.append(f"  rb_bytes_into(r, {a.name}, {a.name} ? {a.size} : 0);")
        elif a.kind == "out":
            L.append(f"  rb_bytes_into(r, {a.name}, {a.name} ? (size_t){args[a.lenidx].name} * {a.esize} : 0);")
    k, ct = rk
    if k == "void":
        L.append("  cl_end(r);")
    elif k == "scalar":
        L.append(f"  {ct} ret = ({ct})rb_{wire_of(ct)}(r); cl_end(r); return ret;")
    elif k == "rstr":
        L.append("  const char *ret = rb_str_keep(r); cl_end(r); return ret;")
    elif k == "rblob":
        L.append("  const void *ret = rb_blob_keep(r); cl_end(r); return ret;")
    L.append("}")

    # ---- Wine dispatcher
    # C# bool returns map to a native 1-byte bool: read only AL on the Wine side.
    wret = "uint8_t" if decls[entry][0] == "bool" else cret(rk)
    W.append(f"typedef {wret} (*fn{fid}_t)({', '.join(a.ctype for a in args) or 'void'});")
    W.append(f"static void d{fid}(rbuf *r, wbuf *w) {{")
    W.append(f'  static fn{fid}_t fn; static int looked;')
    W.append(f'  if (!looked) {{ fn = (fn{fid}_t)sv_sym("{entry}"); looked = 1; }}')
    names = []
    for a in args:
        v = f"v_{a.name}"
        if a.kind == "scalar":
            W.append(f"  {a.ctype} {v} = ({a.ctype})rb_{wire_of(a.ctype)}(r);")
        elif a.kind == "str":
            W.append(f"  const char *{v} = rb_str(r);")
        elif a.kind == "ref":
            W.append(f"  size_t {v}_n; {a.ctype} {v} = ({a.ctype})rb_bytes_dup(r, &{v}_n);")
        elif a.kind == "initparams":
            W.append(f"  ovr_init_params {v}_s; {a.ctype} {v} = rb_initparams(r, &{v}_s);")
        elif a.kind == "kvp":
            W.append(f"  {a.ctype} {v} = rb_kvp(r);")
        elif a.kind == "strarr":
            W.append(f"  {a.ctype} {v} = rb_strarr(r);")
        elif a.kind == "in":
            W.append(f"  size_t {v}_n; {a.ctype} {v} = ({a.ctype})rb_bytes_dup(r, &{v}_n);")
        elif a.kind == "out":
            W.append(f"  size_t {v}_n = (size_t)rb_u64(r); {a.ctype} {v} = ({a.ctype})sv_scratch({v}_n);")
        elif a.kind == "nulled":
            W.append(f"  {a.ctype} {v} = 0;")
        names.append(v)
    call = f"fn({', '.join(names)})"
    k, ct = rk
    if k == "void":
        W.append(f"  if (fn) {call};")
    else:
        rt = cret(rk)
        W.append(f"  {rt} ret = 0; if (fn) ret = {call};")
    for a in args:
        if a.kind == "ref":
            W.append(f"  wb_bytes(w, v_{a.name}, v_{a.name}_n);")
        elif a.kind == "out":
            W.append(f"  wb_bytes(w, v_{a.name}, v_{a.name}_n);")
    if k == "scalar":
        W.append(f"  wb_{wire_of(ct)}(w, ret);")
    elif k == "rstr":
        W.append("  wb_str(w, ret);")
    elif k == "rblob":
        sz = BLOB_SIZE.get(entry)
        first = names[0] if names else "0"
        if sz:
            W.append(f'  uint64_t n = ret ? sv_size_call("{sz}", (uintptr_t){first}) : 0;')
        else:
            W.append("  uint64_t n = 0;")
        W.append("  wb_bytes(w, ret, (size_t)n);")
    W.append("}")

L.append(f"const unsigned cl_table_len = {len(funcs) + 1};")
W.append("const sv_dispatch_fn sv_table[] = { 0,")
W.extend(f"  d{fid}," for fid in range(1, len(funcs) + 1))
W.append("};")
W.append(f"const unsigned sv_table_len = {len(funcs) + 1};")

out = pathlib.Path(sys.argv[1])
(out / "gen_linux.c").write_text("\n".join(L) + "\n")
(out / "gen_win.c").write_text("\n".join(W) + "\n")
print(f"{len(funcs)} functions bridged, {len(skipped)} skipped: {skipped}", file=sys.stderr)
