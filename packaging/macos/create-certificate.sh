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

RESPONSE=$(curl -sS -X POST https://api.appstoreconnect.apple.com/v1/certificates \
  -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" -d "$BODY")

if printf '%s' "$RESPONSE" | grep -q '"errors"'; then
  printf '%s\n' "$RESPONSE" | python3 -c '
import json, sys
for e in json.load(sys.stdin).get("errors", []):
    print(f"  {e.get(\"title\")}: {e.get(\"detail\")}")
' >&2
  die "Apple refused the request (an Admin-role key and a paid membership are both required)"
fi

printf '%s' "$RESPONSE" | python3 -c '
import base64, json, sys
d = json.load(sys.stdin)["data"]["attributes"]
sys.stdout.buffer.write(base64.b64decode(d["certificateContent"]))
' > "$OUT"

say "Done"
openssl x509 -inform DER -in "$OUT" -noout -subject -enddate | sed 's/^/   /'
printf '\n   Saved to %s\n' "$OUT"
printf '   Next:\n     ./packaging/macos/prepare-signing.sh %s %s\n\n' \
  "$HOME/apple-signing/devid.key" "$OUT"
