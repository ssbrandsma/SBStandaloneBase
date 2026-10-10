#!/bin/sh
# Add loopback URLs to SqueezePlay's existing direct-artwork exceptions.
# The patch is deliberately source-verified and idempotent.
set -eu

target=${1:-/usr/share/jive/jive/slim/SlimServer.lua}
backup="$target.pre-standalonebase"
tmp="$target.standalonebase.tmp"
outer='if string.find(iconId, "^http://%d") and ('
private='string.find(iconId, "^http://192%.168") or'
marker='StandaloneBase loopback artwork'
legacy_proxy="jnt:getSNHostname() .. '/public/imageproxy?w='"

if test ! -f "$target"; then
  echo "artwork-patch: missing target: $target" >&2
  exit 2
fi

if grep -Fq "$marker" "$target"; then
  echo "artwork-patch: already applied: $target"
  exit 0
fi

# Community firmware may already support loopback explicitly. Do not rewrite
# its implementation merely because it differs from Logitech's old source.
if grep -Fq '127%.0%.0%.1' "$target" && grep -Fq 'localhost' "$target"; then
  echo "artwork-patch: native loopback support present; not needed: $target"
  exit 0
fi

# Newer firmware can remove the retired image-proxy path entirely. In that
# case there is nothing for this compatibility patch to bypass.
proxy_count=$(grep -F -c '/public/imageproxy' "$target" || true)
if test "$proxy_count" = 0; then
  echo "artwork-patch: legacy imageproxy absent; not needed: $target"
  exit 0
fi

outer_count=$(grep -F -c "$outer" "$target" || true)
private_count=$(grep -F -c "$private" "$target" || true)
legacy_proxy_count=$(grep -F -c "$legacy_proxy" "$target" || true)
if test "$proxy_count" != 1 || test "$legacy_proxy_count" != 1 ||
   test "$outer_count" != 1 || test "$private_count" != 1; then
  echo "artwork-patch: unsupported imageproxy source (proxy=$proxy_count legacy=$legacy_proxy_count outer=$outer_count private=$private_count): $target" >&2
  exit 3
fi

if test ! -f "$backup"; then
  cp -p "$target" "$backup" || {
    echo "artwork-patch: could not create backup: $backup" >&2
    exit 4
  }
fi

awk -v outer="$outer" -v private="$private" '
  index($0, outer) {
    indent=substr($0, 1, index($0, "if string.find") - 1)
    print indent "-- StandaloneBase loopback artwork: bypass retired SN imageproxy"
    print indent "if (string.find(iconId, \"^http://%d\") or"
    print indent "    string.find(iconId, \"^http://localhost[:/]\")) and ("
    next
  }
  index($0, private) {
    indent=substr($0, 1, index($0, "string.find") - 1)
    print indent "string.find(iconId, \"^http://127%.0%.0%.1[:/]\") or"
  }
  { print }
' "$target" >"$tmp" || {
  rm -f "$tmp"
  echo "artwork-patch: transformation failed: $target" >&2
  exit 4
}

if ! grep -Fq "$marker" "$tmp" ||
   ! grep -Fq 'string.find(iconId, "^http://127%.0%.0%.1[:/]") or' "$tmp" ||
   ! grep -Fq 'string.find(iconId, "^http://localhost[:/]")' "$tmp"; then
  rm -f "$tmp"
  echo "artwork-patch: verification failed: $target" >&2
  exit 4
fi

chmod "$(stat -c %a "$target" 2>/dev/null || echo 644)" "$tmp" 2>/dev/null || chmod 644 "$tmp"
mv "$tmp" "$target" || {
  rm -f "$tmp"
  echo "artwork-patch: installation failed: $target" >&2
  exit 4
}
echo "artwork-patch: applied: $target"
