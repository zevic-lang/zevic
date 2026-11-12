#!/bin/sh
# Imports the apt signing key from the environment variable KEY into the current gpg keyring.
# Accepted forms: an armored key, the same key as one base64 line, or just the armor body whose
# BEGIN/END lines (or line breaks) were lost when it was pasted into a secret.
KEY="${KEY:-}"
have_secret() { gpg --list-secret-keys --with-colons 2>/dev/null | grep -q '^sec'; }
try() { gpg --batch --import 2>>"${TMPDIR:-/tmp}/import-gpg.err"; have_secret; }
clean="$(printf '%s' "$KEY" | tr -d ' \r\n')"
if printf '%s' "$KEY" | grep -q 'BEGIN PGP'; then printf '%s\n' "$KEY" | try && exit 0; fi
printf '%s' "$clean" | base64 -d 2>/dev/null | try && exit 0
body="$(printf '%s' "$clean" | sed 's/^-*BEGIN[A-Z]*-*//;s/-*END[A-Z]*-*$//')"
{ echo '-----BEGIN PGP PRIVATE KEY BLOCK-----'; echo; printf '%s' "$body" | fold -w 64; echo; echo '-----END PGP PRIVATE KEY BLOCK-----'; } | try && exit 0
# the shape of the secret without its content: letters and digits are masked
mask() { tr 'A-Za-z0-9' 'x' | tr '+/' '#'; }
echo "secret: ${#KEY} characters, $(printf '%s\n' "$KEY" | wc -l) lines"
echo "secret starts with: $(printf '%s' "$KEY" | head -c 30 | mask)"
echo "secret ends with:   $(printf '%s' "$KEY" | tail -c 30 | mask)"
echo "base64 decodes to $(printf '%s' "$clean" | base64 -d 2>/dev/null | wc -c) bytes, first byte $(printf '%s' "$clean" | base64 -d 2>/dev/null | head -c 1 | od -An -tx1)"
sort -u "${TMPDIR:-/tmp}/import-gpg.err" 2>/dev/null | head -5
echo "::error::APT_GPG_PRIVATE_KEY could not be imported as a secret key (the secret is ${#KEY} characters long)."
echo "::error::Create it again with: gpg --export-secret-keys <id> | base64 -w0   (a single line, no spaces)"
exit 1
