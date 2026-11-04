#!/usr/bin/env python3
"""Golden test runner for zevic.

Each directory under tests/golden holds:
  input.zev              the program (more .zev, .c and .cpp files are passed too)
  args.txt               extra zevic options, e.g. -DNAME=1 -I{dir}/inc (optional; {tmp} is a scratch directory)
  expected_output.txt    exact stdout of the program        (run tests)
  expected_exit_code     exit code of the program, default 0 (run tests)
  run_args.txt           command line arguments for the program (optional)
  expected_error.txt     lines that must appear in the compiler's stderr (compile-error tests;
                         the compiler must exit with code 1)
"""
import concurrent.futures
import glob
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ZEVIC = os.environ.get("ZEVIC", os.path.join(ROOT, "build", "zevic"))
if os.name == "nt" and not ZEVIC.endswith(".exe") and not os.path.exists(ZEVIC):
    ZEVIC += ".exe"


def read(path, default=None):
    try:
        with open(path, encoding="utf-8", newline="") as f:
            return f.read()
    except FileNotFoundError:
        return default


TIMEOUT = int(os.environ.get("ZEVIC_TEST_TIMEOUT", "180"))  # seconds per compile or per program run


def run_test(d):
    try:
        return run_test_inner(d)
    except subprocess.TimeoutExpired as e:
        return os.path.basename(d), False, "timed out after %d seconds: %s" % (TIMEOUT, " ".join(map(str, e.cmd))[:300])


def run_test_inner(d):
    name = os.path.basename(d)
    sources = sorted(glob.glob(os.path.join(d, "*.zev")))
    inp = os.path.join(d, "input.zev")
    if inp in sources:
        sources.remove(inp)
        sources.insert(0, inp)
    sources += sorted(glob.glob(os.path.join(d, "*.c")) + glob.glob(os.path.join(d, "*.cpp")))  # linked in
    extra = (read(os.path.join(d, "args.txt")) or "").split()  # more zevic options; {dir}, {tmp} are replaced
    with tempfile.TemporaryDirectory() as tmp:
        sources += [a.replace("{dir}", d).replace("{tmp}", tmp) for a in extra]
        exe = os.path.join(tmp, "prog" + (".exe" if os.name == "nt" else ""))
        env = dict(os.environ)
        c = subprocess.run([ZEVIC] + sources + ["-o", exe], capture_output=True, text=True, encoding="utf-8", env=env,
                           timeout=TIMEOUT)
        expected_error = read(os.path.join(d, "expected_error.txt"))
        if expected_error is not None:
            if c.returncode != 1:
                return name, False, f"expected a compile error (exit 1), got exit {c.returncode}\n{c.stderr}"
            missing = [l for l in expected_error.splitlines() if l.strip() and l not in c.stderr]
            if missing:
                return name, False, "missing in the error output: %r\n--- stderr ---\n%s" % (missing, c.stderr)
            return name, True, ""
        if c.returncode != 0:
            return name, False, f"compilation failed (exit {c.returncode})\n{c.stderr}"
        run_args = (read(os.path.join(d, "run_args.txt")) or "").split()
        p = subprocess.run([exe] + run_args, capture_output=True, text=True, encoding="utf-8",
                           input=read(os.path.join(d, "input.txt"), ""), timeout=TIMEOUT)
        want_out = read(os.path.join(d, "expected_output.txt"), "")
        want_code = int((read(os.path.join(d, "expected_exit_code"), "0") or "0").strip())
        if p.stdout.replace("\r\n", "\n") != want_out.replace("\r\n", "\n"):
            import difflib
            diff = "".join(difflib.unified_diff(want_out.splitlines(True), p.stdout.splitlines(True),
                                                "expected", "actual"))
            return name, False, "output differs:\n" + diff
        if p.returncode != want_code:
            return name, False, f"exit code {p.returncode}, expected {want_code}\n{p.stderr}"
        want_err = read(os.path.join(d, "expected_stderr.txt"))
        if want_err is not None:
            missing = [l for l in want_err.splitlines() if l.strip() and l not in p.stderr]
            if missing:
                return name, False, "missing in stderr: %r\n--- stderr ---\n%s" % (missing, p.stderr)
        return name, True, ""


def main():
    pattern = sys.argv[1] if len(sys.argv) > 1 else ""
    dirs = sorted(d for d in glob.glob(os.path.join(ROOT, "tests", "golden", "*")) if os.path.isdir(d))
    dirs = [d for d in dirs if pattern in os.path.basename(d)]
    if not dirs:
        print("no tests found")
        return 1
    failed = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 2) as ex:
        for name, ok, msg in ex.map(run_test, dirs):
            print(("ok    " if ok else "FAIL  ") + name, flush=True)
            if not ok:
                failed += 1
                print("      " + msg.replace("\n", "\n      "), flush=True)
    print(f"\n{len(dirs) - failed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
