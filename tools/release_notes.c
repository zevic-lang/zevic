/* Writes the release notes for the current VERSION to standard output.
 *
 *   cc tools/release_notes.c -o build/release_notes
 *   build/release_notes [--tag TAG] [--prerelease] [--assets FILE]...
 *
 * The notes are the CHANGELOG.md section of VERSION (builds of main use "Unreleased"), the list of the files of the
 * release with what each one is for, the commits since the previous release tag, and how to install.
 * Run it from the repository root. */
#define _POSIX_C_SOURCE 200809L
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 4096, n = 0, r;
    char *b = malloc(cap);
    while ((r = fread(b + n, 1, cap - n - 1, f)) > 0) {
        n += r;
        if (n + 1 >= cap) b = realloc(b, cap *= 2);
    }
    fclose(f);
    b[n] = 0;
    return b;
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0;
    return s;
}

/* the output of a command, trimmed ("" when it fails) */
static char *capture(const char *cmd) {
    FILE *p = popen(cmd, "r");
    if (!p) return strdup("");
    size_t cap = 4096, n = 0, r;
    char *b = malloc(cap);
    while ((r = fread(b + n, 1, cap - n - 1, p)) > 0) {
        n += r;
        if (n + 1 >= cap) b = realloc(b, cap *= 2);
    }
    int rc = pclose(p);
    b[n] = 0;
    if (rc != 0) b[0] = 0;
    return strdup(trim(b));
}

/* The body of the "## <heading>" section of the changelog, or NULL. */
static char *section(const char *text, const char *heading) {
    size_t hl = strlen(heading);
    const char *p = text;
    while (p && *p) {
        if (strncmp(p, "## ", 3) == 0 && strncmp(p + 3, heading, hl) == 0 && (p[3 + hl] == '\n' || p[3 + hl] == '\r' || p[3 + hl] == ' ')) {
            const char *start = strchr(p, '\n');
            if (!start) return NULL;
            start++;
            const char *end = start;
            while (*end && !(strncmp(end, "## ", 3) == 0 && (end == start || end[-1] == '\n'))) {
                const char *nl = strchr(end, '\n');
                if (!nl) {
                    end += strlen(end);
                    break;
                }
                end = nl + 1;
            }
            char *s = malloc((size_t)(end - start) + 1);
            memcpy(s, start, (size_t)(end - start));
            s[end - start] = 0;
            char *t = trim(s);
            return *t ? t : NULL;
        }
        const char *nl = strchr(p, '\n');
        p = nl ? nl + 1 : NULL;
    }
    return NULL;
}

/* what a release file is for, from its name */
static const char *describe(const char *name) {
    if (strstr(name, "SHA256SUMS")) return "SHA-256 checksums of every file";
    if (strstr(name, ".deb")) return "Debian / Ubuntu package (`sudo apt install ./<file>`)";
    if (strstr(name, ".pkg")) return "macOS installer package";
    if (strstr(name, "windows") && strstr(name, ".zip")) return "Windows: unzip and put `bin` on PATH (or run `install.ps1`)";
    if (strstr(name, "linux") && strstr(name, ".tar.gz")) return "Linux x64: unpack and put `bin` on PATH (or run `install.sh`)";
    if (strstr(name, "macos") && strstr(name, ".tar.gz")) return "macOS (Apple silicon): unpack and put `bin` on PATH (or run `install.sh`)";
    if (strstr(name, "install.sh")) return "installer for Linux and macOS";
    if (strstr(name, "install.ps1")) return "installer for Windows (PowerShell)";
    return "";
}

int main(int argc, char **argv) {
    const char *tag = "";
    bool pre = false;
    const char *assets[256];
    int nassets = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--tag") && i + 1 < argc) tag = argv[++i];
        else if (!strcmp(argv[i], "--prerelease")) pre = true;
        else if (!strcmp(argv[i], "--assets")) {
            while (i + 1 < argc && strncmp(argv[i + 1], "--", 2) != 0 && nassets < 256) assets[nassets++] = argv[++i];
        } else {
            fprintf(stderr, "usage: release_notes [--tag TAG] [--prerelease] [--assets FILE...]\n");
            return 2;
        }
    }
    char *version = slurp("VERSION");
    if (!version) {
        fprintf(stderr, "cannot read VERSION\n");
        return 1;
    }
    version = trim(version);
    char *log = slurp("CHANGELOG.md");
    const char *heading = version;
    char *body = log ? section(log, pre ? "Unreleased" : version) : NULL;
    if (pre && body) heading = "Unreleased";
    if (!body && log) {
        body = section(log, pre ? version : "Unreleased");
        if (body && !pre) heading = "Unreleased";
    }
    if (!body) body = "(no changelog entry for this version)";
    printf("# zevic %s%s\n\n", *tag ? "" : "v", *tag ? tag : version);
    if (pre) printf("Development build of the main branch (not a stable release).\n\n");
    printf("## What's new%s%s%s\n\n%s\n\n", strcmp(heading, version) ? " (" : "", strcmp(heading, version) ? heading : "",
           strcmp(heading, version) ? ")" : "", body);

    printf("## Downloads\n\n");
    if (nassets == 0) printf("See the files of this release.\n\n");
    else {
        printf("| File | |\n|---|---|\n");
        for (int i = 0; i < nassets; i++) { const char *b = strrchr(assets[i], '/'); b = b ? b + 1 : assets[i]; printf("| `%s` | %s |\n", b, describe(b)); }
        printf("\n");
    }
    printf("## Install\n\n");
    printf("Linux and macOS: `sh install.sh` (or unpack the archive and add its `bin` directory to PATH).\n\n");
    printf("Windows (PowerShell): `.\\install.ps1`, or unzip and add the `bin` folder to PATH.\n\n");
    printf("Then `zevic --version`, `zevpm new hello`, `cd hello`, `zevpm run`. The standard library is the `Zevic` package in `packages/Zevic`.\n");
    printf("clang 15 or newer must be installed: `zevic` hands the generated LLVM IR to it.\n\n");

    char *prev = capture(pre ? "git describe --tags --abbrev=0 --match \"v*\" 2>/dev/null"
                             : "git describe --tags --abbrev=0 --match \"v*\" HEAD^ 2>/dev/null");
    char cmd[512];
    if (*prev) snprintf(cmd, sizeof cmd, "git log --no-merges --pretty=format:\"- %%s (%%h)\" %s..HEAD", prev);
    else snprintf(cmd, sizeof cmd, "git log --no-merges --pretty=format:\"- %%s (%%h)\" HEAD");
    char *commits = capture(cmd);
    if (*commits) {
        if (*prev) printf("## Commits since %s\n\n%s\n\n", prev, commits);
        else printf("## Commits\n\n%s\n\n", commits);
    }
    printf("## Contents\n\n- `bin/zevic`, `bin/zevpm`, `bin/zevfmt` %s\n", version);
    printf("- `packages/Zevic/`: the standard library, a zevpm package named `Zevic` (version %s)\n", version);
    printf("- `runtime/`, `examples/`, `LICENSE`, `README.md`, `VERSION`\n");
    return 0;
}
