#!/usr/bin/env python3
"""Differential tests for zevic1 (the compiler written in Zevic) against zevic0 (the C one).

usage: zevic1_test.py <zevic0> <zevic1> [tokens] [ast]

For every .zev file of the repository: `--dump-tokens` (and, later, `--dump-ast`) of both must be equal,
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


def main():
    z0, z1 = sys.argv[1], sys.argv[2]
    flags = sys.argv[3:] or ["tokens"]
    failed = 0
    files = sources()
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
    print("\n%d failed (of %d)" % (failed, len(files) * len(flags)))
    sys.exit(1 if failed else 0)


main()
