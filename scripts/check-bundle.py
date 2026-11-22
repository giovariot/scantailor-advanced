#!/usr/bin/env python3
"""Fails if a Mach-O in the .app depends on something that is not inside the bundle or the system.

Every dependency is resolved the way dyld does (@rpath, @loader_path, @executable_path), looking only
at the bundle: Homebrew and other external locations count as missing, so a bundle that only runs on
the machine that built it is caught.
"""
import os
import subprocess
import sys

app = os.path.abspath(sys.argv[1])
SYSTEM = ("/usr/lib/", "/System/Library/")


def run(*cmd):
    return subprocess.run(cmd, capture_output=True, text=True, check=True).stdout


def is_macho(path):
    try:
        with open(path, "rb") as f:
            return f.read(4) in (b"\xcf\xfa\xed\xfe", b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca")
    except OSError:
        return False


def rpaths(path):
    out, lines = [], run("otool", "-l", path).splitlines()
    for i, line in enumerate(lines):
        if line.strip() == "cmd LC_RPATH":
            out.append(lines[i + 2].split("path ", 1)[1].rsplit(" (offset", 1)[0])
    return out


def deps(path):
    return [l.split()[0] for l in run("otool", "-L", path).splitlines()[1:]]


def expand(ref, loader, exe_dir):
    return ref.replace("@loader_path", os.path.dirname(loader)).replace("@executable_path", exe_dir)


problems = []
exe_dir = os.path.join(app, "Contents", "MacOS")
for root, _, names in os.walk(app):
    for name in names:
        path = os.path.join(root, name)
        if os.path.islink(path) or not is_macho(path):
            continue
        for rp in rpaths(path):
            if rp.startswith(("/opt/homebrew", "/usr/local")):
                problems.append(f"{os.path.relpath(path, app)}: rpath {rp}")
        for dep in deps(path):
            if dep.startswith(SYSTEM):
                continue
            if dep.startswith("@rpath/"):
                found = any(
                    os.path.exists(os.path.join(expand(rp, path, exe_dir), dep[len("@rpath/"):]))
                    for rp in rpaths(path)
                )
            elif dep.startswith("@"):
                found = os.path.exists(expand(dep, path, exe_dir))
            else:
                found = False
            if not found and os.path.basename(dep) != name:
                problems.append(f"{os.path.relpath(path, app)}: {dep}")

for p in problems:
    print("MISSING", p)
sys.exit(1 if problems else 0)
