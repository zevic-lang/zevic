#!/usr/bin/env python3
"""zevfmt checks: the formatted source parses to the same AST as the original, formatting is idempotent,
and the Zevic sources of the repository are already formatted.

usage: zevfmt_test.py <zevic> <zevfmt>
"""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIRS = ["stdlib", "examples", "tools"]
EXTRA = [os.path.join("tests", "zevfmt")]


def zev_files():
    for d in DIRS + EXTRA:
        for base, _, names in os.walk(os.path.join(ROOT, d)):
            for n in sorted(names):
                if n.endswith(".zev"):
                    yield os.path.join(base, n)


def ast(zevic, path):
    p = subprocess.run([zevic, "--dump-ast", path], capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=120)
    return p.returncode, p.stdout, p.stderr


def same_ast(a, b):
    """Equal ASTs; the order of the imports does not matter (the formatter sorts them)."""
    def norm(t):
        lines = t.split("\n")
        imports = sorted(l for l in lines if l.startswith("import "))
        return imports, [l for l in lines if not l.startswith("import ")]
    return norm(a) == norm(b)


def fmt(zevfmt, path):
    p = subprocess.run([zevfmt, path], capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=120)
    return p.returncode, p.stdout, p.stderr


def main():
    zevic, zevfmt = sys.argv[1], sys.argv[2]
    failed = 0
    tmp = tempfile.mkdtemp()
    for path in zev_files():
        rel = os.path.relpath(path, ROOT)
        code, out, err = fmt(zevfmt, path)
        if code != 0:
            print("FAIL  %s: zevfmt failed\n%s" % (rel, err))
            failed += 1
            continue
        once = os.path.join(tmp, "once.zev")
        with open(once, "w", encoding="utf-8", newline="\n") as f:
            f.write(out)
        c0, a0, e0 = ast(zevic, path)
        c1, a1, e1 = ast(zevic, once)
        if c1 != 0 or not same_ast(a0, a1):
            print("FAIL  %s: the formatted source does not mean the same\n%s" % (rel, e1))
            failed += 1
            continue
        code, out2, err = fmt(zevfmt, once)
        if code != 0 or out2 != out:
            print("FAIL  %s: formatting is not idempotent" % rel)
            failed += 1
            continue
        if os.path.join(ROOT, "tests") not in path:
            with open(path, encoding="utf-8") as f:
                if f.read() != out:
                    print("FAIL  %s: not formatted (run `zevfmt -w`)" % rel)
                    failed += 1
                    continue
        expected = path[:-4] + ".expected"
        if os.path.exists(expected):
            with open(expected, encoding="utf-8") as f:
                if f.read() != out:
                    print("FAIL  %s: differs from %s" % (rel, os.path.basename(expected)))
                    failed += 1
                    continue
        print("ok    %s" % rel)
    print("\n%d failed" % failed)
    sys.exit(1 if failed else 0)


main()
