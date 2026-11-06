#!/usr/bin/env python3
"""Differential tests for zevic1 (the compiler written in Zevic) against zevic0 (the C one).

usage: zevic1_test.py <zevic0> <zevic1> [tokens] [ast] [modules] [types]

For every .zev file of the repository: `--dump-tokens`, `--dump-ast` and (as programs) `--dump-modules` of both must be equal,
and so must the error messages and the exit code.
"""
import glob
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def sources():
    out = []
    for pat in ["stdlib/*.zev", "examples/*.zev", "examples/**/*.zev", "tools/**/*.zev", "tests/golden/*/*.zev", "tests/zevfmt/*.zev", "tests/zevic1/**/*.zev"]:
        out += glob.glob(os.path.join(ROOT, pat), recursive=True)
    return sorted(set(out))


FLOAT = re.compile(r"^(\d+:\d+ number) (\S+)( _\w+)?$")
AST_FLOAT = re.compile(r"^(\s*Float) (\S+?)(_\w+)?$")


def normalize(text, floats):
    """zevic0 prints floats with %g, zevic1 with its own formatting: compare the numbers."""
    if not floats:
        return text
    lines = []
    for l in text.split("\n"):
        m = FLOAT.match(l) or AST_FLOAT.match(l)
        if m:
            try:
                l = "%s %g%s" % (m.group(1), float(m.group(2)), m.group(3) or "")
            except ValueError:
                pass
        lines.append(l)
    return "\n".join(lines)


def run(exe, flag, path):
    p = subprocess.run([exe, flag, path], capture_output=True, text=True)
    return p.returncode, p.stdout, p.stderr


def programs():
    """Each file alone, plus the multi-file programs: a package's src/*.zev and the directories of examples."""
    units = [[f] for f in sources()]
    for d in sorted(set(os.path.dirname(f) for f in sources())):
        fs = sorted(f for f in glob.glob(os.path.join(d, "*.zev")) if not f.endswith("_test.zev"))
        if len(fs) > 1 and "stdlib" not in d and "golden" not in d and "tests/zevic1" not in d.replace(os.sep, "/"):
            units.append(fs)
    units.append(sorted(glob.glob(os.path.join(ROOT, "tests/zevic1/resolve/cycle/*.zev"))))
    return units


def run_modules(z0, z1, flag="--dump-modules", label="modules"):
    failed = 0
    env = dict(os.environ, ZEVIC_HOME=ROOT)
    units = programs()
    for files in units:
        rel = " ".join(os.path.relpath(f, ROOT) for f in files)
        res = []
        for exe in (z0, z1):
            p = subprocess.run([exe, flag] + files, capture_output=True, text=True, env=env, cwd=ROOT)
            res.append((p.returncode, p.stdout, p.stderr))
        if res[0] != res[1]:
            print("FAIL  %s %s" % (label, rel))
            (c0, o0, e0), (c1, o1, e1) = res
            if c0 != c1:
                print("      exit code %d (zevic0) and %d (zevic1)" % (c0, c1))
            for i, (x, y) in enumerate(zip(o0.split("\n"), o1.split("\n"))):
                if x != y:
                    print("      line %d:\n        zevic0: %s\n        zevic1: %s" % (i + 1, x, y))
                    break
            if e0 != e1:
                print("      stderr zevic0: %r\n      stderr zevic1: %r" % (e0[:300], e1[:300]))
            failed += 1
        else:
            print("ok    %s %s" % (label, rel))
    return failed, len(units)


def run_types(z0, z1):
    """--selftest-types: the same types, sizes and LLVM types from both."""
    res = []
    for exe in (z0, z1):
        p = subprocess.run([exe, "--selftest-types"], capture_output=True, text=True)
        res.append((p.returncode, p.stdout, p.stderr))
    if res[0] != res[1]:
        print("FAIL  types selftest")
        for i, (x, y) in enumerate(zip(res[0][1].split("\n"), res[1][1].split("\n"))):
            if x != y:
                print("      line %d:\n        zevic0: %s\n        zevic1: %s" % (i + 1, x, y))
                break
        return 1, 1
    print("ok    types selftest")
    return 0, 1


def main():
    z0, z1 = sys.argv[1], sys.argv[2]
    flags = sys.argv[3:] or ["tokens"]
    failed = 0
    files = sources()
    total = 0
    if "types" in flags:
        flags = [f for f in flags if f != "types"]
        f, n = run_types(z0, z1)
        failed += f
        total += n
    if "gentypes" in flags:
        flags = [f for f in flags if f != "gentypes"]
        f, n = run_modules(z0, z1, "--dump-types", "gentypes")
        failed += f
        total += n
    if "modules" in flags:
        flags = [f for f in flags if f != "modules"]
        f, n = run_modules(z0, z1)
        failed += f
        total += n
    for flag in flags:
        opt = "--dump-" + flag
        for path in files:
            rel = os.path.relpath(path, ROOT)
            c0, o0, e0 = run(z0, opt, path)
            c1, o1, e1 = run(z1, opt, path)
            floats = True
            # the file name in the messages is the path that was given: the same for both
            if c0 != c1 or normalize(o0, floats) != normalize(o1, floats) or e0 != e1:
                print("FAIL  %s %s" % (flag, rel))
                if c0 != c1:
                    print("      exit code %d (zevic0) and %d (zevic1)" % (c0, c1))
                a = normalize(o0, floats).split("\n")
                b = normalize(o1, floats).split("\n")
                for i, (x, y) in enumerate(zip(a, b)):
                    if x != y:
                        print("      line %d:\n        zevic0: %s\n        zevic1: %s" % (i + 1, x, y))
                        break
                else:
                    if len(a) != len(b):
                        print("      %d lines (zevic0) and %d lines (zevic1)" % (len(a), len(b)))
                if e0 != e1:
                    print("      stderr zevic0: %r\n      stderr zevic1: %r" % (e0[:200], e1[:200]))
                failed += 1
            else:
                print("ok    %s %s" % (flag, rel))
    print("\n%d failed (of %d)" % (failed, total + len(files) * len(flags)))
    sys.exit(1 if failed else 0)


main()
