#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
stage="$root/build-package/StandaloneBase"
zipfile="$root/build-package/StandaloneBase-0.2.2.zip"
rm -rf "$stage"
mkdir -p "$stage"
cp "$root/applet/StandaloneBaseMeta.lua" "$root/applet/StandaloneBaseApplet.lua" "$root/applet/strings.txt" "$stage/"
cp "$root/build-arm/sbbase" "$root/build-arm/sbwebserver" "$root/build-arm/sbproxy" "$stage/"
cp "$root/native/sbwebserver/web/index.html" "$stage/index.html"
cp "$root/native/sbwebserver/web/css/style.css" "$stage/style.css"
cp "$root/native/sbwebserver/web/js/app.js" "$stage/app.js"
cp "$root/config/config.example.json" "$stage/config.json"
cp "$root/config/catalog.example.json" "$stage/catalog.json"
cp "$root/native/sbproxy/cacert.pem" "$stage/cacert.pem"
chmod 0755 "$stage/sbbase" "$stage/sbwebserver" "$stage/sbproxy"
(cd "$stage" && zip -qr "$zipfile" .)
unzip -l "$zipfile"
echo "$zipfile"
