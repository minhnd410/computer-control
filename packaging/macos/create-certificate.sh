#!/usr/bin/env bash
# Creates a Developer ID Application certificate through the App Store Connect
# API, so the only thing you do by hand is generate an API key once.
#
# Run this yourself. It authenticates as you and mints a code-signing
# certificate, which is not something to hand to an automation.
#
#   export ASC_KEY_ID=XXXXXXXXXX
#   export ASC_ISSUER_ID=aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee
#   export ASC_KEY=~/Downloads/AuthKey_XXXXXXXXXX.p8
#   ./create-certificate.sh ~/apple-signing/devid.csr
#
# Get the key from App Store Connect > Users and Access > Integrations > Keys.
# It needs the Admin role; a Developer-role key cannot create certificates.
set -euo pipefail

CSR=${1:-$HOME/apple-signing/devid.csr}
OUT=${2:-$HOME/apple-signing/developerID_application.cer}

die() { printf 'error: %s\n' "$*" >&2; exit 1; }
say() { printf '\n== %s\n' "$*"; }

cat >&2 <<'NOTE'
This does not work for Developer ID certificates, and cannot be made to.

  Team keys reach the provisioning endpoints but top out at the Admin role,
  and Apple restricts DEVELOPER_ID_APPLICATION to the Account Holder:

      403 FORBIDDEN_ERROR
      "This operation can only be performed by the Account Holder."

  Individual keys are tied to a person, but Apple documents them as having
  no access to provisioning endpoints at all - which is where certificates
  live. There is no key that satisfies both halves.

Create it one of these two ways instead, then run prepare-signing.sh:

  Xcode      Settings > Accounts > your Apple ID > Manage Certificates
             > + > Developer ID Application. Xcode makes the key and CSR
             and installs the result. Export it from Keychain Access as a
             .p12 and pass that to prepare-signing.sh.

  Browser    developer.apple.com > Certificates, Identifiers & Profiles
             > Certificates > + > Developer ID Application, signed in as
             the Account Holder. Upload the CSR you already have at
             ~/apple-signing/devid.csr, download the .cer, and pass it
             with its key to prepare-signing.sh.

This script is kept because the JWT signing and the request shape are
correct and useful for the endpoints a Team key *can* reach. Set
CC_FORCE=1 to run it anyway.
NOTE
[ -n "${CC_FORCE:-}" ] || exit 2

[ -f "$CSR" ] || die "no CSR at $CSR (generate one with openssl req -new)"
: "${ASC_KEY_ID:?set ASC_KEY_ID}"
: "${ASC_ISSUER_ID:?set ASC_ISSUER_ID}"
: "${ASC_KEY:?set ASC_KEY to the .p8 path}"
[ -f "$ASC_KEY" ] || die "no key at $ASC_KEY"
command -v python3 >/dev/null || die "python3 is required"

say "Signing a request token"
# ES256 JWT, twenty minutes, which is the maximum Apple accepts.
TOKEN=$(python3 - "$ASC_KEY" "$ASC_KEY_ID" "$ASC_ISSUER_ID" <<'PY'
import base64, hashlib, json, subprocess, sys, time, tempfile, os

key_path, key_id, issuer = sys.argv[1], sys.argv[2], sys.argv[3]

def b64(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode()

header = b64(json.dumps({"alg": "ES256", "kid": key_id, "typ": "JWT"}).encode())
now = int(time.time())
payload = b64(json.dumps({"iss": issuer, "iat": now, "exp": now + 1200,
                          "aud": "appstoreconnect-v1"}).encode())
signing_input = f"{header}.{payload}".encode()

# openssl emits DER; JWS wants the raw r||s pair.
with tempfile.NamedTemporaryFile(delete=False) as f:
    f.write(signing_input)
    tmp = f.name
try:
    der = subprocess.run(["openssl", "dgst", "-sha256", "-sign", key_path, tmp],
                         capture_output=True, check=True).stdout
finally:
    os.unlink(tmp)

# Minimal DER SEQUENCE { INTEGER r, INTEGER s } reader.
def unwrap(buf):
    assert buf[0] == 0x30
    i = 2 if buf[1] < 0x80 else 2 + (buf[1] & 0x7F)
    out = []
    while i < len(buf):
        assert buf[i] == 0x02
        n = buf[i + 1]
        out.append(int.from_bytes(buf[i + 2:i + 2 + n], "big"))
        i += 2 + n
    return out

r, s = unwrap(der)
sig = r.to_bytes(32, "big") + s.to_bytes(32, "big")
print(f"{header}.{payload}.{b64(sig)}")
PY
)
[ -n "$TOKEN" ] || die "could not sign a token"

say "Asking Apple for the certificate"
CSR_CONTENT=$(grep -v -- '-----' "$CSR" | tr -d '\n')
BODY=$(python3 -c '
import json, sys
print(json.dumps({"data": {"type": "certificates", "attributes": {
    "certificateType": "DEVELOPER_ID_APPLICATION", "csrContent": sys.argv[1]}}}))
' "$CSR_CONTENT")

# Capture the status separately: a 401 with an empty body and a 409 with a
# useful message look identical when you only keep stdout.
HTTP=$(curl -sS -o /tmp/cc-asc-response.json -w '%{http_code}' \
  -X POST https://api.appstoreconnect.apple.com/v1/certificates \
  -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" -d "$BODY")
RESPONSE=$(cat /tmp/cc-asc-response.json)
printf '   HTTP %s\n' "$HTTP"

if [ "$HTTP" != "201" ] && [ "$HTTP" != "200" ]; then
  printf '   response:\n' >&2
  printf '%s\n' "$RESPONSE" | head -c 2000 >&2
  printf '\n' >&2
fi

if printf '%s' "$RESPONSE" | grep -q '"errors"'; then
  # Print whatever came back even if it does not parse. An error reporter that
  # can fail is worse than none: it hides the message it exists to show.
  printf '%s\n' "$RESPONSE" | python3 -c '
import json, sys
raw = sys.stdin.read()
try:
    for e in json.loads(raw).get("errors", []):
        title = e.get("title", "")
        detail = e.get("detail", "")
        code = e.get("code", "")
        print("  " + title)
        if detail and detail != title:
            print("    " + detail)
        if code:
            print("    code: " + code)
except Exception:
    print("  (unparsed response)")
    print(raw[:2000])
' >&2
  die "Apple refused the request"
fi

printf '%s' "$RESPONSE" | python3 -c '
import base64, json, sys
raw = sys.stdin.read()
try:
    d = json.loads(raw)["data"]["attributes"]
except Exception:
    sys.stderr.write("unexpected response from Apple:\n" + raw[:2000] + "\n")
    raise SystemExit(1)
sys.stdout.buffer.write(base64.b64decode(d["certificateContent"]))
' > "$OUT"

say "Done"
openssl x509 -inform DER -in "$OUT" -noout -subject -enddate | sed 's/^/   /'
printf '\n   Saved to %s\n' "$OUT"
printf '   Next:\n     ./packaging/macos/prepare-signing.sh %s %s\n\n' \
  "$HOME/apple-signing/devid.key" "$OUT"
