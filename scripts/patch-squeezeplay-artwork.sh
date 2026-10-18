#!/bin/sh
# Source-verified, atomic adapters; retain unrelated firmware-specific behavior.
set -eu
patch_one() {
  target=$1
  marker='StandaloneBase artwork routing v2'
  test -f "$target" || { echo "artwork-patch: missing target: $target" >&2; return 2; }
  if grep -Fq "$marker" "$target"; then
    if grep -q '/public/imageproxy' "$target" || ! grep -Fq '.route(' "$target"; then
      echo "artwork-patch: incomplete marked patch: $target" >&2; return 3
    fi
    echo "artwork-patch: already applied: $target"; return
  fi
  count=$(grep -c '/public/imageproxy' "$target" || true)
  if test "$count" = 0; then
    echo "artwork-patch: legacy imageproxy absent; not needed: $target"; return
  fi
  legacy=$(grep -c "jnt:getSNHostname() .. '/public/imageproxy?w='" "$target" || true)
  kind=slim
  if grep -q '^function requestImage(self, imageData)' "$target"; then kind=viewer; fi
  if test "$count" != 1 || test "$legacy" != 1; then
    echo "artwork-patch: unsupported imageproxy source: $target" >&2; return 3
  fi
  if test "$kind" = slim; then
    for anchor in 'local function _getArtworkThumbSink(self, cacheKey, size, url)' 'function processArtworkQueue(self)'; do
      test "$(grep -F -c "$anchor" "$target" || true)" = 1 || {
        echo "artwork-patch: unsupported SlimServer anchor: $anchor" >&2; return 3;
      }
    done
    test "$(grep -E -c '^[[:space:]]*url = iconId[[:space:]]*$' "$target" || true)" = 1 || return 3
  else
    test "$(grep -E -c 'if string.find\(urlString, "\^https?\??://%d"\) and \(' "$target" || true)" = 1 || {
      echo 'artwork-patch: unsupported ImageViewer source' >&2; return 3;
    }
  fi
  tmp="$target.standalonebase.tmp"
  awk -v kind="$kind" '
    BEGIN { helper="require(\"applets.StandaloneBase.Artwork\")" }
    kind == "slim" && /^local function _getArtworkThumbSink\(/ {
      print "-- StandaloneBase artwork routing v2"
      print
      print "  do return " helper ".sink(self, cacheKey, size, url, _loadArtworkImage) end"
      next
    }
    kind == "slim" && /^function cancelAllArtwork\(/ {
      print; print "  " helper ".cancel(self)"; next
    }
    kind == "slim" && /^[ \t]*url = iconId[ \t\r]*$/ {
      print "                url = " helper ".route(iconId, sizeW, sizeH)"; next
    }
    /jnt:getSNHostname\(\).*\/public\/imageproxy/ {
      if (kind == "slim") print "                url = " helper ".route(iconId, sizeW, sizeH)"
      else print "        urlString = " helper ".route(urlString, screenWidth, screenHeight)"
      next
    }
    kind == "viewer" && (index($0, "if string.find(urlString, \"^http://%d\") and (") || index($0, "if string.find(urlString, \"^https?://%d\") and (")) {
      print "    -- StandaloneBase artwork routing v2"
      print "    if string.match(urlString, \"^https?://\") then"
      print "        urlString = " helper ".route(urlString, screenWidth, screenHeight)"
      sub(/if string.find/, "elseif string.find")
    }
    kind == "slim" && /log:error\("Server ", self.name, " cannot handle artwork for ", entry.url\)/ {
      print; print "                self.artworkCache:set(entry.key, nil)"; next
    }
    { print }
  ' "$target" > "$tmp"
  if ! grep -Fq "$marker" "$tmp" || grep -q '/public/imageproxy' "$tmp"; then
    rm -f "$tmp"; echo "artwork-patch: verification failed: $target" >&2; return 4
  fi
  test -f "$target.pre-standalonebase" || cp -p "$target" "$target.pre-standalonebase"
  chmod "$(stat -c %a "$target" 2>/dev/null || echo 644)" "$tmp"
  mv "$tmp" "$target"
  echo "artwork-patch: applied: $target"
}
if test "$#" -gt 0; then
  patch_one "$1"
else
  patch_one /usr/share/jive/jive/slim/SlimServer.lua
  patch_one /usr/share/jive/applets/ImageViewer/ImageSourceServer.lua
fi
