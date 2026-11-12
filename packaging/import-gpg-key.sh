#!/bin/sh
# Imports the apt signing key from the environment variable KEY into the current gpg keyring.
# Accepted forms: an armored key, the same key as one base64 line, or just the armor body whose
# BEGIN/END lines (or line breaks) were lost when it was pasted into a secret.
KEY="${KEY:-}"
have_secret() { gpg --list-secret-keys --with-colons 2>/dev/null | grep -q '^sec'; }
try() { gpg --batch --import 2>/dev/null; have_secret; }
clean="$(printf '%s' "$KEY" | tr -d ' \r\n')"
if printf '%s' "$KEY" | grep -q 'BEGIN PGP'; then printf '%s\n' "$KEY" | try && exit 0; fi
printf '%s' "$clean" | base64 -d 2>/dev/null | try && exit 0
body="$(printf '%s' "$clean" | sed 's/^-*BEGIN[A-Z]*-*//;s/-*END[A-Z]*-*$//')"
{ echo '-----BEGIN PGP PRIVATE KEY BLOCK-----'; echo; printf '%s' "$body" | fold -w 64; echo; echo '-----END PGP PRIVATE KEY BLOCK-----'; } | try && exit 0
echo "::error::APT_GPG_PRIVATE_KEY could not be imported as a secret key (the secret is ${#KEY} characters long)."
echo "::error::Create it again with: gpg --export-secret-keys <id> | base64 -w0   (a single line, no spaces)"
exit 1
