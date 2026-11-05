#!/usr/bin/env python3
"""Integration tests for zevpm: packages, dependencies (path and git), the lock file, tests and libraries.

Git dependencies are served from local repositories (file:// addresses), so no network is needed."""
import os
import shutil
import stat
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = ".exe" if os.name == "nt" else ""
ZEVPM = os.path.abspath(os.environ.get("ZEVPM", os.path.join(ROOT, "build", "zevpm" + EXE)))
ZEVIC = os.path.abspath(os.environ.get("ZEVIC", os.path.join(ROOT, "build", "zevic" + EXE)))

failures = []


def rmtree(path):
    """shutil.rmtree that also removes read-only files: git writes its objects that way, and Windows refuses them."""
    def writable(func, p, _exc):
        try:
            os.chmod(p, stat.S_IWRITE)
            func(p)
        except OSError:
            pass
    shutil.rmtree(path, onerror=writable)


def sh(args, cwd, env=None, check=True):
    e = dict(os.environ)
    e["ZEVIC"] = ZEVIC
    e.update(env or {})
    p = subprocess.run(args, cwd=cwd, capture_output=True, text=True, encoding="utf-8", env=e, timeout=300)
    if check and p.returncode != 0:
        raise AssertionError("%s failed (%d)\n%s%s" % (args, p.returncode, p.stdout, p.stderr))
    return p


def pm(args, cwd, env=None, check=True):
    return sh([ZEVPM] + args, cwd, env, check)


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def expect(cond, msg):
    if not cond:
        raise AssertionError(msg)


def make_lib(d, name, body, deps="", extra=None):
    write(os.path.join(d, "zevic.toml"),
          '[package]\nname = "%s"\nversion = "1.0.0"\nkind = "lib"\n\n[deps]\n%s\n' % (name, deps))
    write(os.path.join(d, "src", name + ".zev"), "module %s\n\n%s\n" % (name, body))
    for rel, text in (extra or {}).items():
        write(os.path.join(d, rel), text)


def git_repo(d, tag=None):
    g = ["git", "-c", "user.name=t", "-c", "user.email=t@t", "-c", "init.defaultBranch=main"]
    sh(g + ["init", "-q"], d)
    sh(g + ["add", "-A"], d)
    sh(g + ["commit", "-q", "-m", "init"], d)
    if tag:
        sh(g + ["tag", tag], d)
    return sh(["git", "rev-parse", "HEAD"], d).stdout.strip()


def uri(d):
    return "file://" + d.replace("\\", "/")


def scenario(fn):
    name = fn.__name__
    tmp = tempfile.mkdtemp(prefix="zevpm-test-")
    try:
        fn(tmp)
        print("ok    " + name)
    except Exception as ex:  # noqa: BLE001
        failures.append(name)
        print("FAIL  " + name)
        print("      " + str(ex).replace("\n", "\n      "))
    finally:
        rmtree(tmp)


def new_run_test_clean(tmp):
    pm(["new", "my-app"], tmp)
    app = os.path.join(tmp, "my-app")
    expect("name = \"MyApp\"" in read(os.path.join(app, "zevic.toml")), "package name is PascalCase")
    expect(read(os.path.join(app, ".gitignore")) == "build/\n", ".gitignore")
    out = pm(["run"], app).stdout
    expect("Hello from MyApp!" in out, "run output: " + out)
    write(os.path.join(app, "src", "Main.zev"),
          'module Main\n\nimport Zevic.Console\nimport Zevic.Env\n\nfn Main() {\n'
          '    Console.Println("args=${Env.RawArgs().Len()}")\n}\n')
    out = pm(["run", "--", "a", "b"], app).stdout
    expect("args=" in out, "arguments reach the program: " + out)
    write(os.path.join(app, "src", "Math_test.zev"),
          'module Main\n\nimport Zevic.Test\n\ntest "one" {\n    Test.Equal(1, 1)\n}\n\ntest "two fails" {\n    Test.Equal(1, 2)\n}\n')
    p = pm(["test"], app, check=False)
    expect(p.returncode == 1, "a failing test gives exit code 1, got %d" % p.returncode)
    expect("1 passed, 1 failed" in p.stdout and "Math_test.zev:" in p.stdout, "test report: " + p.stdout)
    p = pm(["test", "one"], app)
    expect("1 passed, 0 failed" in p.stdout, "name filter: " + p.stdout)
    # test blocks and _test.zev files stay out of the normal build
    pm(["build", "--release"], app)
    pm(["clean"], app)
    expect(not os.path.exists(os.path.join(app, "build")), "clean removes build/")
    expect(pm(["new", "my-app"], tmp, check=False).returncode != 0, "new refuses an existing directory")


def path_dependency(tmp):
    util = os.path.join(tmp, "util")
    make_lib(util, "Util", "fn Twice(x: I32) -> I32 { x * 2 }")
    pm(["new", "app"], tmp)
    app = os.path.join(tmp, "app")
    with open(os.path.join(app, "zevic.toml"), "a", encoding="utf-8") as f:
        f.write('\n[deps]\nUtil = { path = "../util" }\n')
    write(os.path.join(app, "src", "Main.zev"),
          'module Main\n\nimport Util\nimport Zevic.Console\n\nfn Main() {\n    Console.Println("${Util.Twice(21)}")\n}\n')
    expect(pm(["run"], app).stdout.strip() == "42", "path dependency works")
    expect(not os.path.exists(os.path.join(app, "zevic.lock")), "path dependencies are not locked")


def native_in_dependency(tmp):
    nat = os.path.join(tmp, "nat")
    make_lib(nat, "Nat",
             'extern "C" {\n    fn nat_triple(x: CInt) -> CInt\n}\n\nfn Triple(x: I32) -> I32 {\n    nat_triple(x.ToCInt()).ToI32()\n}',
             extra={"native/nat.c": "int nat_triple(int x) { return x * 3; }\n"})
    pm(["new", "app"], tmp)
    app = os.path.join(tmp, "app")
    with open(os.path.join(app, "zevic.toml"), "a", encoding="utf-8") as f:
        f.write('\n[deps]\nNat = { path = "../nat" }\n')
    write(os.path.join(app, "src", "Main.zev"),
          'module Main\n\nimport Nat\nimport Zevic.Console\n\nfn Main() {\n    Console.Println("${Nat.Triple(14)}")\n}\n')
    expect(pm(["run"], app).stdout.strip() == "42", "the dependency's native/ is built")


def library_for_c(tmp):
    pm(["new", "geo", "--lib"], tmp)
    geo = os.path.join(tmp, "geo")
    write(os.path.join(geo, "src", "Geo.zev"),
          'module Geo\n\nimport Zevic.Test\n\nexport "C" fn geo_add(a: CInt, b: CInt) -> CInt {\n'
          '    (a.ToI32() + b.ToI32()).ToCInt()\n}\n\ntest "add through C types" {\n    Test.Equal(geo_add(1.ToCInt(), 2.ToCInt()).ToI32(), 3)\n}\n')
    pm(["build"], geo)
    lib = os.path.join(geo, "build", "Geo.lib" if os.name == "nt" else "libGeo.a")
    header = os.path.join(geo, "build", "include", "Geo.h")
    expect(os.path.exists(lib) and os.path.exists(header), "the library and its header exist")
    write(os.path.join(geo, "use.c"),
          '#include <stdio.h>\n#include "build/include/Geo.h"\nint main(void) {\n    zevic_init(0, 0);\n'
          '    printf("%d\\n", geo_add(2, 40));\n    return 0;\n}\n')
    cc = os.environ.get("CC", "clang")
    # what a C program needs to link next to the library: the math library, or Winsock on Windows
    syslibs = ["-lws2_32"] if os.name == "nt" else ["-lm", "-pthread"]
    sh([cc, "use.c", lib] + syslibs + ["-o", "use" + EXE], geo)
    expect(sh([os.path.join(geo, "use" + EXE)], geo).stdout.strip() == "42", "C calls the Zevic library")
    p = pm(["test"], geo)
    expect("1 passed, 0 failed" in p.stdout, "tests of a library: " + p.stdout)
    expect(pm(["run"], geo, check=False).returncode != 0, "a library cannot be run")


def git_dependencies(tmp):
    if shutil.which("git") is None:
        print("      (git not found: skipped)")
        return
    home = os.path.join(tmp, "zevpm-home")
    env = {"ZEVPM_HOME": home}
    # B, tagged v1.0.0 and then changed; A depends on B at v1.0.0
    b = os.path.join(tmp, "b")
    make_lib(b, "Bee", "fn Name() -> String { \"bee 1\" }")
    rev_b1 = git_repo(b, "v1.0.0")
    write(os.path.join(b, "src", "Bee.zev"), "module Bee\n\nfn Name() -> String { \"bee 2\" }\n")
    g = ["git", "-c", "user.name=t", "-c", "user.email=t@t"]
    sh(g + ["commit", "-q", "-am", "second"], b)
    sh(g + ["tag", "v2.0.0"], b)
    a = os.path.join(tmp, "a")
    make_lib(a, "Ay", "import Bee\n\nfn Name() -> String { \"ay+\" + Bee.Name() }",
             deps='Bee = { git = "%s", tag = "v1.0.0" }' % uri(b))
    git_repo(a, "v1.0.0")

    pm(["new", "app"], tmp)
    app = os.path.join(tmp, "app")
    write(os.path.join(app, "src", "Main.zev"),
          'module Main\n\nimport Ay\nimport Zevic.Console\n\nfn Main() {\n    Console.Println(Ay.Name())\n}\n')
    out = pm(["get", uri(a) + "@v1.0.0"], app, env).stdout
    expect("Added dependency `Ay`" in out, "get: " + out)
    toml = read(os.path.join(app, "zevic.toml"))
    expect('Ay = { git = "%s", tag = "v1.0.0" }' % uri(a) in toml, "zevic.toml has the dependency:\n" + toml)
    lock = read(os.path.join(app, "zevic.lock"))
    expect('name = "Ay"' in lock and 'name = "Bee"' in lock and rev_b1 in lock, "the lock has both packages:\n" + lock)
    expect(pm(["run"], app, env).stdout.strip() == "ay+bee 1", "transitive dependency is built")
    # the cache is used: a second build works without the repositories
    rmtree(a), rmtree(b)
    expect(pm(["run"], app, env).stdout.strip() == "ay+bee 1", "built from the cache")
    # a conflict: the app asks for Bee at another tag than Ay does
    b2 = os.path.join(tmp, "b2")
    make_lib(b2, "Bee", "fn Name() -> String { \"other\" }")
    git_repo(b2, "v9.0.0")
    with open(os.path.join(app, "zevic.toml"), "a", encoding="utf-8") as f:
        f.write('Bee = { git = "%s", tag = "v9.0.0" }\n' % uri(b2))
    p = pm(["build"], app, env, check=False)
    expect(p.returncode == 1 and "two different versions" in p.stderr, "conflict is an error: " + p.stderr)
    # remove
    expect("Removed dependency `Bee`" in pm(["remove", "Bee"], app, env).stdout, "remove")
    expect("Bee = " not in read(os.path.join(app, "zevic.toml")), "line is gone")
    expect(pm(["remove", "Nope"], app, env, check=False).returncode == 1, "removing an unknown name fails")
    pm(["remove", "Ay"], app, env)
    expect(not os.path.exists(os.path.join(app, "zevic.lock")), "an empty lock is removed")


def git_update(tmp):
    if shutil.which("git") is None:
        print("      (git not found: skipped)")
        return
    env = {"ZEVPM_HOME": os.path.join(tmp, "zevpm-home")}
    c = os.path.join(tmp, "c")
    make_lib(c, "Cee", "fn Name() -> String { \"cee 1\" }")
    git_repo(c)
    pm(["new", "app"], tmp)
    app = os.path.join(tmp, "app")
    write(os.path.join(app, "src", "Main.zev"),
          'module Main\n\nimport Cee\nimport Zevic.Console\n\nfn Main() {\n    Console.Println(Cee.Name())\n}\n')
    pm(["get", uri(c)], app, env)
    expect(pm(["run"], app, env).stdout.strip() == "cee 1", "first version")
    write(os.path.join(c, "src", "Cee.zev"), "module Cee\n\nfn Name() -> String { \"cee 2\" }\n")
    sh(["git", "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q", "-am", "two"], c)
    expect(pm(["run"], app, env).stdout.strip() == "cee 1", "the lock keeps the old commit")
    pm(["update"], app, env)
    expect(pm(["run"], app, env).stdout.strip() == "cee 2", "update moves to the newest commit")


def manifest_errors(tmp):
    pm(["new", "app"], tmp)
    app = os.path.join(tmp, "app")
    toml = os.path.join(app, "zevic.toml")
    good = read(toml)
    cases = [
        (good.replace('name = "App"', 'name = "app"'), "PascalCase"),
        (good.replace('version = "0.1.0"', 'version = "1.0"'), "1.2.3"),
        (good.replace('kind = "bin"', 'kind = "dll"'), "bin"),
        (good + '\n[deps]\nUtil = { git = "x", path = "y" }\n', "both `git` and `path`"),
        (good + '\n[deps]\nUtil = { git = "x", tag = "a", branch = "b" }\n', "only one of"),
        (good + '\n[oops]\n', "unknown section"),
        (good.replace('zevic = ">=0.1"', 'zevic = ">=99.0"'), "needs zevic"),
    ]
    for text, want in cases:
        write(toml, text)
        p = pm(["build"], app, check=False)
        expect(p.returncode != 0 and want in p.stderr, "expected `%s` in: %s" % (want, p.stderr))


def main():
    for fn in (new_run_test_clean, path_dependency, native_in_dependency, library_for_c, git_dependencies, git_update,
               manifest_errors):
        scenario(fn)
    print("\n%d passed, %d failed" % (7 - len(failures), len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
