#!/usr/bin/env python3
"""Builds and runs every program in examples/ and compares the output with tests/examples_expected/<name>.txt.
examples/apps/* are zevpm packages: their tests must pass."""
import glob
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = ".exe" if os.name == "nt" else ""
ZEVIC = os.environ.get("ZEVIC", os.path.join(ROOT, "build", "zevic" + EXE))
ZEVPM = os.environ.get("ZEVPM", os.path.join(ROOT, "build", "zevpm" + EXE))
EX = os.path.join(ROOT, "examples")
EXPECTED = os.path.join(ROOT, "tests", "examples_expected")


def run(args, cwd=None, env=None):
    e = dict(os.environ)
    e["ZEVIC"] = ZEVIC
    e.update(env or {})
    return subprocess.run(args, cwd=cwd, capture_output=True, text=True, encoding="utf-8", env=e)


def check_single(name, sources):
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "prog" + EXE)
        c = run([ZEVIC] + sources + ["-o", exe])
        if c.returncode != 0:
            return "compilation failed\n" + c.stderr
        p = run([exe], cwd=tmp)
        if p.returncode != 0:
            return "exit code %d\n%s" % (p.returncode, p.stderr)
        want_path = os.path.join(EXPECTED, name + ".txt")
        if os.environ.get("UPDATE_EXPECTED"):
            with open(want_path, "w", encoding="utf-8", newline="\n") as f:
                f.write(p.stdout.replace("\r\n", "\n"))
            return None
        with open(want_path, encoding="utf-8") as f:
            want = f.read()
        if p.stdout.replace("\r\n", "\n") != want:
            return "output differs from %s\n--- actual ---\n%s" % (want_path, p.stdout)
    return None


def spec_sample_matches():
    """Appendix B of the specification is the wordcount example."""
    with open(os.path.join(ROOT, "docs", "spec", "zevic-spec.md"), encoding="utf-8") as f:
        spec = f.read()
    start = spec.index("## 付録B")
    start = spec.index("```\n", start) + 4
    code = spec[start:spec.index("\n```\n", start)]
    with open(os.path.join(EX, "apps", "wordcount", "src", "Main.zev"), encoding="utf-8") as f:
        return code.strip() == f.read().strip()


def main():
    failed = 0
    if not spec_sample_matches():
        print("FAIL  specification appendix B differs from examples/apps/wordcount/src/Main.zev")
        failed += 1
    cases = []
    for f in sorted(glob.glob(os.path.join(EX, "*.zev"))):
        cases.append((os.path.basename(f)[:-4], [f]))
    for d in sorted(glob.glob(os.path.join(EX, "[0-9]*_*"))):
        if os.path.isdir(d):
            files = sorted(glob.glob(os.path.join(d, "*.zev")) + glob.glob(os.path.join(d, "*.c")))
            cases.append((os.path.basename(d), files))
    for name, sources in cases:
        err = check_single(name, sources)
        print(("ok    " if not err else "FAIL  ") + name)
        if err:
            failed += 1
            print("      " + err.replace("\n", "\n      "))
    for pkg in sorted(glob.glob(os.path.join(EX, "apps", "*"))):
        name = "apps/" + os.path.basename(pkg)
        p = run([ZEVPM, "test"], cwd=pkg)
        ok = p.returncode == 0
        print(("ok    " if ok else "FAIL  ") + name + " (zevpm test)")
        if not ok:
            failed += 1
            print("      " + (p.stdout + p.stderr).replace("\n", "\n      "))
        run([ZEVPM, "clean"], cwd=pkg)
    print("\n%d failed" % failed if failed else "\nall examples work")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
