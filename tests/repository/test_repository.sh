#!/bin/sh
set -eu
server=$1
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
work=$(mktemp -d)
pid=
trap 'test -z "$pid" || kill "$pid" 2>/dev/null || true; rm -rf "$work"' EXIT
mkdir -p "$work/config" "$work/repository"
cp "$root/config.json" "$work/config/config.json"
python3 - "$work/Demo-1.0.zip" <<'PY'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1], "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr("Demo/DemoMeta.lua", "return {}")
    z.writestr("Demo/DemoApplet.lua", "return {}")
PY
printf 'not-a-zip' >"$work/invalid.zip"
SBWEBSERVER_CONFIG_DIR="$work/config" SB_REPOSITORY_ROOT="$work/repository" \
"$server" --port 18080 \
--web-root "$root/native/sbwebserver/web" >"$work/server.log" 2>&1 &
pid=$!
sleep 1
test "$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:18080/api/applets)" = 401
capability=$(cat "$work/config/remote.capability")
pin=$(curl -sf -H "X-Remote-Capability: $capability" -X POST --data '' http://127.0.0.1:18080/api/internal/web-access/start | sed -n 's/.*"pin":"\([0-9][0-9]*\)".*/\1/p')
curl -sf -c "$work/cookies" -H 'Content-Type: application/json' -X POST --data "{\"pin\":\"$pin\"}" http://127.0.0.1:18080/api/auth/pair >/dev/null
curl -sf -b "$work/cookies" http://127.0.0.1:18080/api/applets >"$work/list.json"
grep -q 'StandaloneRadio' "$work/list.json"
test "$(curl -s -o /dev/null -w '%{http_code}' -F package=@"$work/Demo-1.0.zip" \
  'http://127.0.0.1:18080/api/applets/upload?name=Demo&title=Demo&version=1.0&target=baby&min_target_version=7.7')" = 401
curl -sf -b "$work/cookies" \
  -F package=@"$work/Demo-1.0.zip" \
  'http://127.0.0.1:18080/api/applets/upload?name=Demo&title=Demo&version=1.0&target=baby&min_target_version=7.7&desc=Test&changes=Initial&creator=Test&email=' \
  >"$work/upload.json"
test "$(curl -s -o /dev/null -w '%{http_code}' -b "$work/cookies" \
  -F package=@"$work/invalid.zip" \
  'http://127.0.0.1:18080/api/applets/upload?name=Bad&title=Bad&version=1.0&target=baby&min_target_version=7.7')" = 400
test "$(curl -s -o /dev/null -w '%{http_code}' -b "$work/cookies" \
  -F package=@"$work/Demo-1.0.zip" \
  'http://127.0.0.1:18080/api/applets/upload?name=Demo&title=Demo&version=1.0&target=baby&min_target_version=7.7')" = 409
grep -q '"name": "Demo"' "$work/config/config.json"
grep -q '"uuid": "9d989f40-499a-4b85-b92f-8dc415af2a04"' "$work/config/config.json"
digest=$(sha1sum "$work/Demo-1.0.zip" | awk '{print $1}')
grep -q "$digest" "$work/config/config.json"
curl -sf -b "$work/cookies" http://127.0.0.1:18080/applets/packages/Demo-1.0.zip >"$work/download.zip"
cmp "$work/Demo-1.0.zip" "$work/download.zip"
curl -sfI -b "$work/cookies" http://127.0.0.1:18080/applets/packages/Demo-1.0.zip | grep -qi '^Content-Length:'
curl -sf -b "$work/cookies" -H 'Content-Type: application/json' \
  -X PUT --data '{"name":"Demo","title":"Demo edited","version":"1.0","target":"baby","min_target_version":"7.7","url":"http://127.0.0.1:80/applets/packages/Demo-1.0.zip","sha":"0000000000000000000000000000000000000000","desc":"Test","changes":"Edited","creator":"Test","email":""}' \
  http://127.0.0.1:18080/api/applets/Demo >/dev/null
grep -q 'Demo edited' "$work/config/config.json"
curl -sf -b "$work/cookies" -X DELETE \
  'http://127.0.0.1:18080/api/applets/Demo?delete_package=1' >/dev/null
test ! -e "$work/repository/packages/Demo-1.0.zip"
! grep -q '"name": "Demo"' "$work/config/config.json"
echo repository-tests-ok
