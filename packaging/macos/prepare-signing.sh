#!/usr/bin/env bash
# Turns a Developer ID certificate into the GitHub secrets the release
# workflow expects.
#
# Run this yourself: it handles a private key and pushes it to GitHub, which
# is deliberately not something automated on your behalf. Everything it does
# is local except the `gh secret set` calls at the end, and it prints each one
# before running it.
#
#   ./prepare-signing.sh ~/apple-signing/devid.key ~/Downloads/developerID_application.cer
#
set -euo pipefail

KEY=${1:-}
CER=${2:-}
REPO=${CC_REPO:-minhnd410/computer-control}

die() { printf 'error: %s\n' "$*" >&2; exit 1; }
say() { printf '\n== %s\n' "$*"; }

[ -f "$KEY" ] || die "private key not found: $KEY"
[ -f "$CER" ] || die "certificate not found: $CER"
command -v gh >/dev/null || die "gh is not installed"
gh auth status >/dev/null 2>&1 || die "gh is not authenticated; run: gh auth login"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
chmod 700 "$WORK"

say "Converting the certificate"
# Apple ships DER; openssl wants PEM for the bundle step.
openssl x509 -inform DER -in "$CER" -out "$WORK/cert.pem" 2>/dev/null \
  || cp "$CER" "$WORK/cert.pem"

SUBJECT=$(openssl x509 -in "$WORK/cert.pem" -noout -subject)
IDENTITY=$(printf '%s' "$SUBJECT" | sed -n 's/.*CN *= *\([^,\/]*\).*/\1/p')
EXPIRY=$(openssl x509 -in "$WORK/cert.pem" -noout -enddate | cut -d= -f2)
printf '   identity: %s\n   expires : %s\n' "$IDENTITY" "$EXPIRY"

case "$IDENTITY" in
  "Developer ID Application:"*) ;;
  *) die "that is a '$IDENTITY' certificate. Notarization requires a
       'Developer ID Application' certificate, which needs a paid Apple
       Developer Program membership. An Apple Development certificate can
       sign locally but cannot be notarized or distributed." ;;
esac

say "Checking the key matches the certificate"
# A mismatched pair produces a .p12 that imports fine and fails at signing
# time on the runner, which is a miserable thing to debug.
KEY_MOD=$(openssl rsa -in "$KEY" -noout -modulus 2>/dev/null | openssl md5)
CRT_MOD=$(openssl x509 -in "$WORK/cert.pem" -noout -modulus | openssl md5)
[ "$KEY_MOD" = "$CRT_MOD" ] || die "this certificate was not issued for that private key"
printf '   match confirmed\n'

say "Building the .p12"
P12_PASSWORD=$(openssl rand -base64 24)
openssl pkcs12 -export -legacy \
  -inkey "$KEY" -in "$WORK/cert.pem" \
  -out "$WORK/devid.p12" -passout "pass:$P12_PASSWORD" 2>/dev/null \
  || openssl pkcs12 -export \
       -inkey "$KEY" -in "$WORK/cert.pem" \
       -out "$WORK/devid.p12" -passout "pass:$P12_PASSWORD"

say "Uploading secrets to $REPO"
set_secret() {
  printf '   gh secret set %s\n' "$1"
  gh secret set "$1" --repo "$REPO" --body "$2" >/dev/null
}
set_secret MACOS_CERT_P12 "$(base64 < "$WORK/devid.p12" | tr -d '\n')"
set_secret MACOS_CERT_PASSWORD "$P12_PASSWORD"
set_secret MACOS_SIGN_IDENTITY "$IDENTITY"

if [ -n "${NOTARY_KEY:-}" ]; then
  [ -f "$NOTARY_KEY" ] || die "NOTARY_KEY is set but $NOTARY_KEY does not exist"
  [ -n "${NOTARY_KEY_ID:-}" ] || die "set NOTARY_KEY_ID alongside NOTARY_KEY"
  [ -n "${NOTARY_ISSUER:-}" ] || die "set NOTARY_ISSUER alongside NOTARY_KEY"
  set_secret MACOS_NOTARY_KEY "$(base64 < "$NOTARY_KEY" | tr -d '\n')"
  set_secret MACOS_NOTARY_KEY_ID "$NOTARY_KEY_ID"
  set_secret MACOS_NOTARY_ISSUER "$NOTARY_ISSUER"
else
  printf '\n   Skipped notarization secrets. To add them, re-run with:\n'
  printf '     NOTARY_KEY=~/Downloads/AuthKey_XXXX.p8 \\\n'
  printf '     NOTARY_KEY_ID=XXXX NOTARY_ISSUER=<uuid> %s ...\n' "$0"
fi

say "Done"
printf '   The .p12 and its password existed only in %s, which is now deleted.\n' "$WORK"
printf '   Keep %s safe: it is the only copy of the private key.\n' "$KEY"
printf '   The next tagged release will sign.\n\n'
