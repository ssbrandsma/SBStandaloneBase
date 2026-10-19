#!/bin/sh
set -eu

config=${SB_DEPLOY_CONFIG:-/tmp/standalonebase-deployment.env}
test -r "$config" || {
    echo "Missing private deployment configuration: $config" >&2
    exit 1
}
# The configuration is intentionally local-only and must define BASE_ROOT,
# MERGED_XML, BOOTSTRAP_CONFIG, WEB_OWNER, BOOTSTRAP_OWNER and CONTAINER_NAME.
. "$config"
: "${BASE_ROOT:?}" "${MERGED_XML:?}" "${BOOTSTRAP_CONFIG:?}"
: "${WEB_OWNER:?}" "${BOOTSTRAP_OWNER:?}" "${CONTAINER_NAME:?}"

version=${SB_RELEASE_VERSION:-0.5.3}
expected_sha=${SB_RELEASE_SHA1:-7a4fccb09639e8756c1ecd8c5b7620d0ea9da715}
release_dir=${SB_RELEASE_STAGE:-/tmp}
stamp=$(date +%Y%m%d-%H%M%S)
base_root=$BASE_ROOT
merged_xml=$MERGED_XML
bootstrap_config=$BOOTSTRAP_CONFIG

test "$(sha1sum "$release_dir/StandaloneBase-$version.zip" | cut -d' ' -f1)" = "$expected_sha"
python3 -m json.tool "$release_dir/squeezebox-config-standalonebase-$version.json" >/dev/null
python3 -c "import xml.etree.ElementTree as E; E.parse('$release_dir/standalonebase-extensions-$version.xml'); E.parse('$release_dir/merged-extensions-standalonebase-$version.xml')"

cp -p "$merged_xml" "$merged_xml.bak.$stamp"
cp -p "$bootstrap_config" "$bootstrap_config.bak.$stamp"
if test -f "$base_root/extensions.xml"; then
    cp -p "$base_root/extensions.xml" "$base_root/extensions.xml.bak.$stamp"
fi
if test -f "$base_root/StandaloneBase-$version.zip"; then
    cp -p "$base_root/StandaloneBase-$version.zip" "$base_root/StandaloneBase-$version.zip.bak.$stamp"
fi
mkdir -p "$base_root"

install -o root -g root -m 0644 "$release_dir/StandaloneBase-$version.zip" "$base_root/StandaloneBase-$version.zip"
install -o root -g root -m 0644 "$release_dir/standalonebase-extensions-$version.xml" "$base_root/extensions.xml"
install -o "$WEB_OWNER" -g "$WEB_OWNER" -m 0644 "$release_dir/merged-extensions-standalonebase-$version.xml" "$merged_xml"
install -o "$BOOTSTRAP_OWNER" -g "$BOOTSTRAP_OWNER" -m 0644 "$release_dir/squeezebox-config-standalonebase-$version.json" "$bootstrap_config"

test "$(sha1sum "$base_root/StandaloneBase-$version.zip" | cut -d' ' -f1)" = "$expected_sha"
docker restart "$CONTAINER_NAME" >/dev/null
docker ps --format '{{.Names}} {{.Status}}' | grep -F "$CONTAINER_NAME "
echo "Backup timestamp: $stamp"
