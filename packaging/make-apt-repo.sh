#!/bin/sh
# Build (or extend) a signed apt repository.
#   sh packaging/make-apt-repo.sh DEB_FILE... -- REPO_DIR
# REPO_DIR may already hold an older repository: the new .deb files are added to its pool and the indexes are
# rebuilt. Environment:
#   APT_GPG_KEY_ID      the key that signs the Release file (without it the repository is left unsigned)
#   APT_GPG_PASSPHRASE  the passphrase of that key, if it has one
# Result:  REPO_DIR/pool/main/z/zevic/*.deb
#          REPO_DIR/dists/stable/{Release,InRelease,Release.gpg,main/binary-amd64/Packages{,.gz}}
#          REPO_DIR/zevic.gpg (the public key, binary, for signed-by=) and REPO_DIR/zevic.asc (armored)
set -eu

DEBS=""
while [ $# -gt 0 ] && [ "$1" != "--" ]; do DEBS="$DEBS $1"; shift; done
[ "${1:-}" = "--" ] || { echo "usage: $0 DEB_FILE... -- REPO_DIR" >&2; exit 2; }
REPO="$2"

POOL="$REPO/pool/main/z/zevic"
DIST="$REPO/dists/stable"
mkdir -p "$POOL"
for d in $DEBS; do cp "$d" "$POOL/"; done

for arch in amd64 arm64; do
    BIN="$DIST/main/binary-$arch"
    mkdir -p "$BIN"
    # Packages lists the files of this architecture (and "all"), with paths relative to the repository root
    ( cd "$REPO" && dpkg-scanpackages --arch "$arch" pool /dev/null 2>/dev/null ) > "$BIN/Packages"
    gzip -9 -n -c "$BIN/Packages" > "$BIN/Packages.gz"
done

# the Release file with the hashes of every index
hashes() {
    algo="$1"; cmd="$2"
    echo "$algo:"
    ( cd "$DIST" && find main -type f | sort | while read -r f; do
        printf ' %s %16d %s\n' "$($cmd "$f" | cut -d' ' -f1)" "$(wc -c < "$f")" "$f"
    done )
}
{
    echo "Origin: Zevic"
    echo "Label: Zevic"
    echo "Suite: stable"
    echo "Codename: stable"
    echo "Architectures: amd64 arm64"
    echo "Components: main"
    echo "Description: Zevic programming language packages"
    echo "Date: $(LC_ALL=C date -u '+%a, %d %b %Y %H:%M:%S UTC')"
    hashes MD5Sum md5sum
    hashes SHA256 sha256sum
} > "$DIST/Release"

if [ -n "${APT_GPG_KEY_ID:-}" ]; then
    GPG="gpg --batch --yes --pinentry-mode loopback"
    if [ -n "${APT_GPG_PASSPHRASE:-}" ]; then GPG="$GPG --passphrase $APT_GPG_PASSPHRASE"; fi
    rm -f "$DIST/InRelease" "$DIST/Release.gpg"
    $GPG --default-key "$APT_GPG_KEY_ID" --clearsign -o "$DIST/InRelease" "$DIST/Release"
    $GPG --default-key "$APT_GPG_KEY_ID" -abs -o "$DIST/Release.gpg" "$DIST/Release"
    gpg --batch --yes --export "$APT_GPG_KEY_ID" > "$REPO/zevic.gpg"
    gpg --batch --yes --armor --export "$APT_GPG_KEY_ID" > "$REPO/zevic.asc"
else
    echo "warning: APT_GPG_KEY_ID is not set, the repository is not signed" >&2
fi

# a small page for people who open the address in a browser
cat > "$REPO/index.html" <<'HTML'
<!doctype html>
<meta charset="utf-8">
<title>Zevic apt repository</title>
<h1>Zevic apt repository</h1>
<pre>
curl -fsSL https://zevic-lang.github.io/zevic/zevic.gpg | sudo tee /usr/share/keyrings/zevic.gpg &gt;/dev/null
echo "deb [signed-by=/usr/share/keyrings/zevic.gpg] https://zevic-lang.github.io/zevic stable main" | sudo tee /etc/apt/sources.list.d/zevic.list
sudo apt update
sudo apt install zevic
</pre>
<p>This also installs clang, which zevic needs. See <a href="https://github.com/zevic-lang/zevic">zevic-lang/zevic</a>.</p>
HTML
touch "$REPO/.nojekyll"
echo "repository ready in $REPO"
