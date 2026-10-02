#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
stage="$root/build-package/StandaloneBase"
zipfile="$root/build-package/StandaloneBase-0.2.1.zip"
rm -rf "$stage"
mkdir -p "$stage/bin" "$stage/web" "$stage/config"
cp "$root/applet/StandaloneBaseMeta.lua" "$root/applet/StandaloneBaseApplet.lua" "$root/applet/strings.txt" "$stage/"
cp "$root/build-arm/sbbase" "$root/build-arm/sbwebserver" "$root/build-arm/sbproxy" "$stage/bin/"
cp -R "$root/native/sbwebserver/web/." "$stage/web/"
cp "$root/config/config.example.json" "$stage/config/config.json"
cp "$root/config/catalog.example.json" "$stage/config/catalog.json"
cp "$root/native/sbproxy/cacert.pem" "$stage/config/cacert.pem"
chmod 0755 "$stage/bin/"*
(cd "$root/build-package" && zip -qr "$zipfile" StandaloneBase)
unzip -l "$zipfile"
echo "$zipfile"
