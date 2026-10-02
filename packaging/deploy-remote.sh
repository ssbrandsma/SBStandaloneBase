#!/bin/sh
set -eu

version=0.2.1
expected_sha=0a28e0443e18697982eb2da33350146bbd508eca
stamp=$(date +%Y%m%d-%H%M%S)
base_root=/var/www/bytestack_nl_usr/data/www/bytestack.nl/sbstandalonebase
merged_xml=/var/www/bytestack_nl_usr/data/www/bytestack.nl/squeezebox/extensions.xml
bootstrap_config=/var/www/squeezebox_o_usr/data/squeezebox-bootstrap/config.json

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
install -o bytestack_nl_usr -g bytestack_nl_usr -m 0644 "/tmp/merged-extensions-standalonebase-$version.xml" "$merged_xml"
install -o squeezebox_o_usr -g squeezebox_o_usr -m 0644 "/tmp/squeezebox-config-standalonebase-$version.json" "$bootstrap_config"

test "$(sha1sum "$base_root/StandaloneBase-$version.zip" | cut -d' ' -f1)" = "$expected_sha"
docker restart squeezebox-bootstrap-squeezebox-bootstrap-1 >/dev/null
docker ps --format '{{.Names}} {{.Status}}' | grep '^squeezebox-bootstrap-squeezebox-bootstrap-1 '
echo "Backup timestamp: $stamp"
