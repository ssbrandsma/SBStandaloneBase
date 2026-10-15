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

version=0.4.0
expected_sha=6c831f57d422fea64e08a85fe3562ee1800466fa
stamp=$(date +%Y%m%d-%H%M%S)
base_root=$BASE_ROOT
merged_xml=$MERGED_XML
bootstrap_config=$BOOTSTRAP_CONFIG

test "$(sha1sum "/tmp/StandaloneBase-$version.zip" | cut -d' ' -f1)" = "$expected_sha"
python3 -m json.tool "/tmp/squeezebox-config-standalonebase-$version.json" >/dev/null
python3 -c "import xml.etree.ElementTree as E; E.parse('/tmp/standalonebase-extensions-$version.xml'); E.parse('/tmp/merged-extensions-standalonebase-$version.xml')"

cp -p "$merged_xml" "$merged_xml.bak.$stamp"
cp -p "$bootstrap_config" "$bootstrap_config.bak.$stamp"
if test -f "$base_root/extensions.xml"; then
    cp -p "$base_root/extensions.xml" "$base_root/extensions.xml.bak.$stamp"
fi
mkdir -p "$base_root"

install -o root -g root -m 0644 "/tmp/StandaloneBase-$version.zip" "$base_root/StandaloneBase-$version.zip"
install -o root -g root -m 0644 "/tmp/standalonebase-extensions-$version.xml" "$base_root/extensions.xml"
install -o "$WEB_OWNER" -g "$WEB_OWNER" -m 0644 "/tmp/merged-extensions-standalonebase-$version.xml" "$merged_xml"
install -o "$BOOTSTRAP_OWNER" -g "$BOOTSTRAP_OWNER" -m 0644 "/tmp/squeezebox-config-standalonebase-$version.json" "$bootstrap_config"

test "$(sha1sum "$base_root/StandaloneBase-$version.zip" | cut -d' ' -f1)" = "$expected_sha"
docker restart "$CONTAINER_NAME" >/dev/null
docker ps --format '{{.Names}} {{.Status}}' | grep -F "$CONTAINER_NAME "
echo "Backup timestamp: $stamp"
