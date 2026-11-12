# Packaging

| File | Output |
| --- | --- |
| `make-deb.sh` | `.deb` (depends on `clang (>= 1:15)`) |
| `make-pkg.sh` | macOS `.pkg` |
| `install.sh`, `install.ps1` | installers for an unpacked archive |
| `zevic.iss` | Windows setup exe (Inno Setup, per-user, adds `bin` to PATH) |
| `make-apt-repo.sh` | signed apt repository (`pool/` + `dists/stable`) |
| `winget/` | winget manifest templates and `render.sh` |

## One-time setup for the maintainer

apt (published to the `gh-pages` branch by the `apt-repo` job on every `v*` tag):

1. `gpg --quick-generate-key "Zevic <you@example.com>" rsa3072 sign never`
2. `gpg --armor --export-secret-keys <id>` goes into the repository secret `APT_GPG_PRIVATE_KEY`
   (`APT_GPG_PASSPHRASE` too, if the key has one).
3. Settings → Pages → deploy from the `gh-pages` branch (the first tag creates the branch).

winget:

1. The first version has to be submitted by hand to microsoft/winget-pkgs: download the `winget-manifests`
   artifact of the tag build and open a PR with it (`winget validate --manifest <dir>` first).
2. Later versions: put a classic token with `public_repo` scope into the secret `WINGET_TOKEN`, and the
   `winget` job opens the PR itself.
