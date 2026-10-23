#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
config=${SB_PACKAGE_CONFIG:-$root/config.json}
test -f "$config" || { echo "missing package catalog: $config" >&2; exit 1; }
module="$root/artifacts/sbubifs-authorized.ko"
expected_sha=63652ce67df06a78abb84a4986253bdab02fbd7b7c000779c60b3d393ba9566b
image="$root/artifacts/ubi/sbdata-empty.ubifs"
expected_image_sha=34f26e33d80393c1f1f497e85dff6b78dbc6fc0e2df98ed1150ea45c6631065d
test "$(wc -c <"$module" | tr -d ' ')" = 199119 || { echo "incorrect authorized module size" >&2; exit 1; }
test "$(sha256sum "$module" | awk '{print $1}')" = "$expected_sha" || { echo "incorrect authorized module SHA-256" >&2; exit 1; }
test "$(wc -c <"$image" | tr -d ' ')" = 1806336 || { echo "incorrect sbdata image size" >&2; exit 1; }
test "$(sha256sum "$image" | awk '{print $1}')" = "$expected_image_sha" || { echo "incorrect sbdata image SHA-256" >&2; exit 1; }
stage="$root/build-package/StandaloneBase"
zipfile="$root/build-package/StandaloneBase-0.6.1.zip"
rm -rf "$stage"
mkdir -p "$stage"
cp "$root/applet/StandaloneBaseMeta.lua" "$root/applet/StandaloneBaseApplet.lua" "$root/applet/StorageManager.lua" "$root/applet/ExtendedStorageState.lua" "$root/applet/TimeSync.lua" "$root/applet/TimeResolver.lua" "$root/applet/RemoteBridge.lua" "$root/applet/ServerMigration.lua" "$root/applet/NetworkAddress.lua" "$root/applet/strings.txt" "$stage/"
cp "$root/build-arm/sbbase" "$root/build-arm/sbwebserver" "$root/build-arm/sbproxy" "$root/build-arm/sb-storage-helper" "$root/build-arm/sb-storage-updater" "$stage/"
cp "$root/build-arm/svgpngrenderer" "$stage/"
cp "$root/applet/ImageRender.lua" "$root/applet/ArtworkUrlNormalizer.lua" "$stage/"
cp "$root/applet/Artwork.lua" "$stage/"
cp "$root/applet/InstallerCompat.lua" "$stage/"
cp "$root/applet/HttpTrace.lua" "$stage/"
cp "$root/native/svgpngrenderer/THIRD_PARTY.md" "$stage/SVG-THIRD-PARTY-NOTICES"
chmod 0755 "$stage/svgpngrenderer"
cp "$root/scripts/storage-setup.sh" "$root/scripts/storage-boot.sh" "$stage/"
cp "$root/scripts/patch-squeezeplay-artwork.sh" "$stage/"
cp "$module" "$stage/sbubifs-authorized.ko"
cp "$image" "$stage/sbdata-empty.ubifs"
cp "$root/native/sbwebserver/web/index.html" "$stage/index.html"
cp "$root/native/sbwebserver/web/css/style.css" "$stage/style.css"
cp "$root/native/sbwebserver/web/js/app.js" "$stage/app.js"
cp "$root/native/sbwebserver/web/js/remote.js" "$stage/remote.js"
cp "$root/native/sbwebserver/web/js/logs.js" "$stage/logs.js"
cp "$root/native/sbwebserver/web/js/performance.js" "$stage/performance.js"
cp "$root/native/sbwebserver/web/vendor/uplot/uPlot.iife.min.js" "$stage/uPlot.iife.min.js"
cp "$root/native/sbwebserver/web/vendor/uplot/uPlot.min.css" "$stage/uPlot.min.css"
cp "$root/native/sbwebserver/web/vendor/uplot/LICENSE" "$stage/UPLOT-LICENSE"
cp "$root/native/sbwebserver/web/assets/virtual-radio/radio-front.webp" "$stage/radio-front.webp"
cp "$config" "$stage/config.json"
cp "$root/config/catalog.example.json" "$stage/catalog.json"
cp "$root/native/sbproxy/cacert.pem" "$stage/cacert.pem"
chmod 0755 "$stage/sbbase" "$stage/sbwebserver" "$stage/sbproxy" "$stage/sb-storage-helper" "$stage/sb-storage-updater" "$stage/storage-setup.sh" "$stage/storage-boot.sh" "$stage/patch-squeezeplay-artwork.sh"
(cd "$stage" && zip -qr "$zipfile" .)
unzip -l "$zipfile"
echo "$zipfile"
