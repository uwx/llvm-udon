#!/usr/bin/env python3
"""Generate the Udon extern table from the live SDK.

The Udon node registry over-reports: `SystemInt64.__op_Remainder` is a registered
node definition and throws "not implemented yet" the moment a delegate is
requested. Membership is therefore not proof that an extern works, and the table
this project emits is generated from *delegate resolution*, not from registry
membership.

The probe is `tools/Udon.ExternProbe`, a small .NET program that constructs the
SDK's own `VRC.Udon.Wrapper.UdonWrapper`. Its modules take only
`IUdonComponentGetter` and `IUdonSecurityFilter`, both pure dispatch
interfaces, so the wrapper runs in plain .NET with no Unity runtime -- which
means this script needs no editor and no MCP endpoint.

Inputs, in order of preference:

  * `--nodedefs <path>`: a JSON dump of `GetNodeDefinitions()`, used only as the
    list of *candidate* signatures. Without it, the candidates come from the
    families this backend emits, built from `--externs-json` if present.
  * `--externs-json <path>`: an existing table to refresh in place, which is
    what `--check` uses to detect drift.

Exit codes: 0 clean, 1 checked and drifted (or an input problem), 2 the probe
could not run at all ("couldn't check").

Stdlib only, Python 3.9.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DEF_PATH = os.path.join(ROOT, "lib", "Target", "UDON", "UDONExterns.def")
JSON_PATH = os.path.join(ROOT, "test", "externs.json")
PROBE_PROJECT = os.path.join(ROOT, "tools", "Udon.ExternProbe", "Udon.ExternProbe.csproj")

# The families the writer names. Everything else the SDK offers is out of scope
# for this milestone, and emitting it would only make the table larger than the
# set the backend can actually produce.
FAMILY_PREFIXES = (
    "SystemInt32.",
    "SystemUInt32.",
    "SystemInt64.",
    "SystemUInt64.",
    "SystemSingle.",
    "SystemDouble.",
    "SystemBoolean.",
    "SystemChar.",
    "SystemByte.",
    "SystemSByte.",
    "SystemInt16.",
    "SystemUInt16.",
    "SystemConvert.",
    "SystemBitConverter.",
    "SystemMath.",
    "SystemString.",
    "SystemArray.",
    "SystemObject.",
    "SystemByteArray.",
    "SystemInt32Array.",
    "SystemUInt32Array.",
    "SystemInt64Array.",
    "SystemUInt64Array.",
    "SystemSingleArray.",
    "SystemDoubleArray.",
    "SystemStringArray.",
    "SystemObjectArray.",
    "SystemBooleanArray.",
    "SystemCharArray.",
    "UnityEngineVector2.",
    "UnityEngineVector3.",
    "UnityEngineVector4.",
    "UnityEngineQuaternion.",
    "UnityEngineColor.",
    "UnityEngineMathf.",
    "UnityEngineDebug.",
    "VRCUdonCommonInterfacesIUdonEventReceiver.",
)

# Always probed, whether or not the registry lists them. Each is a signature the
# backend must never emit, and recording it explicitly is what makes the
# generator's "implemented" answer meaningful: a table that says these are
# implemented is wrong, and the check in Step 3 catches that.
DISCRIMINATING_PROBES = (
    # Registered as node definitions but with no implementation; the delegate
    # request throws "not implemented yet", so the writer must never emit them.
    "SystemInt64.__op_Remainder__SystemInt64_SystemInt64__SystemInt64",
    "SystemUInt64.__op_Remainder__SystemUInt64_SystemUInt64__SystemUInt64",
    "SystemUInt32.__op_Remainder__SystemUInt32_SystemUInt32__SystemUInt32",
    # Present in neither the registry nor the wrapper.
    "SystemInt32.__op_UnsignedRightShift__SystemInt32_SystemInt32__SystemInt32",
    "SystemMathF.__Sqrt__SystemSingle__SystemSingle",
    # The positive controls: if these came back unimplemented the probe itself
    # is broken, not the SDK.
    "SystemInt32.__op_Addition__SystemInt32_SystemInt32__SystemInt32",
    "SystemByteArray.__ctor__SystemInt32__SystemByteArray",
    "UnityEngineDebug.__Log__SystemObject__SystemVoid",
)

# C# identifiers cannot carry these characters, so the .def names each extern
# with a mangled enum member.
def enum_name(signature: str) -> str:
    out = []
    for ch in signature:
        out.append(ch if ch.isalnum() else "_")
    name = "".join(out)
    if not name or name[0].isdigit():
        name = "_" + name
    return "UDONExtern_" + name


def collect_candidates(nodedefs_path: str | None, existing_json: str | None) -> list[str]:
    """Every signature worth probing, sorted and deduplicated."""
    candidates: set[str] = set()

    if existing_json and os.path.exists(existing_json):
        try:
            with open(existing_json, encoding="utf-8") as f:
                candidates.update(json.load(f).keys())
        except (json.JSONDecodeError, OSError) as exc:
            raise SystemExit(f"gen-externs: cannot read {existing_json}: {exc}")

    if nodedefs_path:
        if not os.path.exists(nodedefs_path):
            raise SystemExit(f"gen-externs: no such nodedefs file: {nodedefs_path}")
        try:
            with open(nodedefs_path, encoding="utf-8") as f:
                defs = json.load(f)
        except (json.JSONDecodeError, OSError) as exc:
            raise SystemExit(f"gen-externs: cannot read {nodedefs_path}: {exc}")
        for d in defs:
            full = d.get("full") if isinstance(d, dict) else None
            if full and "__" in full and full.startswith(FAMILY_PREFIXES):
                candidates.add(full)

    candidates.update(DISCRIMINATING_PROBES)
    return sorted(candidates)


def run_probe(candidates: list[str], unity_managed: str | None) -> dict:
    """Ask the real SDK wrapper about each candidate."""
    with tempfile.TemporaryDirectory(prefix="udon-externs-") as tmp:
        in_path = os.path.join(tmp, "in.json")
        out_path = os.path.join(tmp, "out.json")
        with open(in_path, "w", encoding="utf-8") as f:
            json.dump(candidates, f)

        cmd = [
            "dotnet", "run", "--project", PROBE_PROJECT, "-c", "Release",
            "--no-launch-profile", "--",
        ]
        if unity_managed:
            cmd += ["--unity-managed", unity_managed]
        cmd += [in_path, out_path]
        try:
            proc = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
        except FileNotFoundError:
            raise SystemExit("gen-externs: dotnet is not on PATH; cannot check")
        except subprocess.TimeoutExpired:
            raise SystemExit("gen-externs: the probe timed out; cannot check")

        if proc.returncode != 0 or not os.path.exists(out_path):
            sys.stderr.write(proc.stdout)
            sys.stderr.write(proc.stderr)
            raise SystemExit(
                "gen-externs: the probe could not run (exit %d); cannot check" % proc.returncode
            )

        sys.stderr.write(proc.stderr)
        with open(out_path, encoding="utf-8") as f:
            return json.load(f)


def render_def(table: dict) -> str:
    lines = [
        "//===-- UDONExterns.def - generated extern table ------------*- C++ -*-===//",
        "//",
        "// GENERATED by scripts/gen-externs.py. Do not edit by hand.",
        "//",
        "// One line per extern whose delegate the SDK actually resolves. A",
        "// signature the node registry lists but implements nowhere is deliberately",
        "// absent, because emitting it aborts the VM with \"not implemented yet\".",
        "//",
        "//===----------------------------------------------------------------------===//",
        "",
        "// EXTERN(EnumName, \"FullSignature\", Arity)",
        "",
    ]
    for signature in sorted(table):
        entry = table[signature]
        if not entry.get("implemented"):
            continue
        lines.append('EXTERN(%s, "%s", %d)' % (enum_name(signature), signature, entry["arity"]))
    lines.append("")
    return "\n".join(lines)


def render_json(table: dict) -> str:
    return json.dumps(table, indent=2, sort_keys=True) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate the Udon extern table.")
    parser.add_argument("--nodedefs", help="JSON dump of GetNodeDefinitions() for candidates")
    parser.add_argument("--externs-json", default=JSON_PATH,
                        help="existing table to refresh (default: test/externs.json)")
    parser.add_argument("--check", action="store_true",
                        help="compare against the checked-in files instead of writing")
    parser.add_argument("--unity-managed",
                        default=os.environ.get("UNITY_MANAGED_DIR"),
                        help="Unity's Managed/UnityEngine directory, so the "
                             "Unity-facing wrapper modules can construct")
    parser.add_argument("--def-out", default=DEF_PATH)
    parser.add_argument("--json-out", default=JSON_PATH)
    args = parser.parse_args()

    candidates = collect_candidates(args.nodedefs, args.externs_json)
    if not candidates:
        raise SystemExit(
            "gen-externs: no candidate signatures; pass --nodedefs <dump> or point "
            "--externs-json at an existing table"
        )

    probe = run_probe(candidates, args.unity_managed)
    table = {}
    for entry in probe["results"]:
        signature = entry["signature"]
        record = {"implemented": bool(entry.get("implemented"))}
        if record["implemented"]:
            record["arity"] = int(entry["arity"])
        else:
            record["error"] = entry.get("error", "not implemented")
        table[signature] = record

    def_text = render_def(table)
    json_text = render_json(table)

    if args.check:
        drifted = []
        for path, text in ((args.def_out, def_text), (args.json_out, json_text)):
            if not os.path.exists(path):
                drifted.append((path, "missing"))
                continue
            with open(path, encoding="utf-8") as f:
                if f.read() != text:
                    drifted.append((path, "differs"))
        if drifted:
            for path, why in drifted:
                sys.stderr.write("gen-externs: %s %s\n" % (path, why))
            return 1
        sys.stderr.write("gen-externs: clean (%d signatures)\n" % len(table))
        return 0

    os.makedirs(os.path.dirname(args.def_out), exist_ok=True)
    with open(args.def_out, "w", encoding="utf-8", newline="\n") as f:
        f.write(def_text)
    with open(args.json_out, "w", encoding="utf-8", newline="\n") as f:
        f.write(json_text)
    implemented = sum(1 for e in table.values() if e["implemented"])
    sys.stderr.write("gen-externs: wrote %s and %s (%d of %d implemented)\n"
                     % (args.def_out, args.json_out, implemented, len(table)))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except SystemExit:
        raise
    except Exception as exc:  # noqa: BLE001 - "couldn't check" is exit 2, not a traceback
        sys.stderr.write("gen-externs: %s: %s\n" % (type(exc).__name__, exc))
        sys.exit(2)
