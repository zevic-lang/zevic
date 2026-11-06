#!/usr/bin/env python3
"""Write the release notes for the current VERSION to stdout.

    python3 tools/release_notes.py [--tag TAG] [--prerelease]

The notes are the CHANGELOG.md section of VERSION (or "Unreleased" when the version has none yet),
a list of the commits since the previous release tag, and the contents of the bundled `Zevic`
standard-library package. Run it from the repository root."""
import re, subprocess, sys

def sh(*a):
    r = subprocess.run(a, capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else ""

def changelog_section(version, pre):
    text = open("CHANGELOG.md", encoding="utf-8").read()
    parts = re.split(r"^## (.+)$", text, flags=re.M)
    sections = {parts[i].strip(): parts[i + 1].strip() for i in range(1, len(parts) - 1, 2)}
    for key in (("Unreleased", version) if pre else (version, "Unreleased")):
        if sections.get(key):
            return key, sections[key]
    return None, "(no changelog entry for this version)"

def main():
    args = sys.argv[1:]
    tag = args[args.index("--tag") + 1] if "--tag" in args else ""
    pre = "--prerelease" in args
    version = open("VERSION").read().strip()
    key, body = changelog_section(version, pre)
    out = [f"# zevic {tag or 'v' + version}", ""]
    if pre:
        out += ["Development build of the main branch (not a stable release).", ""]
    out += [f"## What's new" + ("" if key == version else f" ({key})"), "", body, ""]
    prev = sh("git", "describe", "--tags", "--abbrev=0", "--match", "v*", "HEAD^") if tag.startswith("v") and not pre \
        else sh("git", "describe", "--tags", "--abbrev=0", "--match", "v*")
    rng = f"{prev}..HEAD" if prev else "HEAD"
    log = sh("git", "log", "--no-merges", "--pretty=format:- %s (%h)", rng)
    if log:
        out += [f"## Commits since {prev}" if prev else "## Commits", "", log, ""]
    out += ["## Contents", "",
            f"- `bin/zevic`, `bin/zevpm`, `bin/zevfmt` {version}",
            f"- `packages/Zevic/`: the standard library, a zevpm package named `Zevic` (version {version})",
            "- `runtime/`, `examples/`, `SHA256SUMS.txt`", ""]
    sys.stdout.write("\n".join(out))

main()
