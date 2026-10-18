#!/bin/sh
set -eu
patcher=$1
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
target="$tmp/SlimServer.lua"

cat >"$target" <<'EOF'
local function _getArtworkThumbSink(self, cacheKey, size, url)
  return function() end
end
function processArtworkQueue(self)
end
function fetchArtwork(iconId)
	if string.find(iconId, "^http") then
		if string.find(iconId, "^http://%d") and (
			string.find(iconId, "^http://192%.168") or
			string.find(iconId, "^http://172%.16%.") or
			string.find(iconId, "^http://10%.")
		) then
			url = iconId
		else
			url = 'http://' .. jnt:getSNHostname() .. '/public/imageproxy?w=' .. sizeW
		end
	end
end
EOF

cp "$target" "$tmp/original.lua"
sh "$patcher" "$target" >/dev/null
test -f "$target.pre-standalonebase"
cmp "$target.pre-standalonebase" "$tmp/original.lua"
grep -Fq 'StandaloneBase artwork routing v2' "$target"
grep -Fq '.route(iconId, sizeW, sizeH)' "$target"
grep -Fq '.sink(self, cacheKey, size, url, _loadArtworkImage)' "$target"
! grep -q '/public/imageproxy' "$target"

cp "$target" "$tmp/patched.lua"
sh "$patcher" "$target" >/dev/null
cmp "$target" "$tmp/patched.lua"
cmp "$target.pre-standalonebase" "$tmp/original.lua"

printf '%s\n' 'url = "http://unknown.example/public/imageproxy"' >"$tmp/unknown.lua"
if sh "$patcher" "$tmp/unknown.lua" >/dev/null 2>&1; then
  echo "unknown source was unexpectedly patched" >&2
  exit 1
else
  test "$?" = 3
fi
test ! -e "$tmp/unknown.lua.pre-standalonebase"

# A newer implementation with no retired imageproxy is explicitly a no-op,
# even if it happens to retain similar private-address checks.
cat >"$tmp/modern.lua" <<'EOF'
function fetchArtwork(iconId)
	if string.find(iconId, "^http://%d") and (
		string.find(iconId, "^http://192%.168") or
		string.find(iconId, "^http://10%.")
	) then url = iconId else url = modernArtworkURL(iconId) end
end
EOF
cp "$tmp/modern.lua" "$tmp/modern.original"
sh "$patcher" "$tmp/modern.lua" | grep -Fq 'legacy imageproxy absent; not needed'
cmp "$tmp/modern.lua" "$tmp/modern.original"
test ! -e "$tmp/modern.lua.pre-standalonebase"

# Explicit native support is also a no-op, regardless of surrounding code.
cat >"$tmp/native.lua" <<'EOF'
if string.find(iconId, "127%.0%.0%.1") or string.find(iconId, "localhost") then
	url = iconId
end
EOF
cp "$tmp/native.lua" "$tmp/native.original"
sh "$patcher" "$tmp/native.lua" | grep -Fq 'legacy imageproxy absent; not needed'
cmp "$tmp/native.lua" "$tmp/native.original"
test ! -e "$tmp/native.lua.pre-standalonebase"

# An alternative proxy implementation is unknown, not legacy: fail closed.
cat >"$tmp/alternative.lua" <<'EOF'
if string.find(iconId, "^http://%d") and (
	string.find(iconId, "^http://192%.168") or
	string.find(iconId, "^http://10%.")
) then url = iconId else url = "http://community.example/public/imageproxy" end
EOF
cp "$tmp/alternative.lua" "$tmp/alternative.original"
if sh "$patcher" "$tmp/alternative.lua" >/dev/null 2>&1; then
  echo "alternative proxy source was unexpectedly patched" >&2
  exit 1
else
  test "$?" = 3
fi
cmp "$tmp/alternative.lua" "$tmp/alternative.original"
test ! -e "$tmp/alternative.lua.pre-standalonebase"

# The independent viewer must classify HTTPS before the old HTTP-only logic.
cat >"$tmp/viewer.lua" <<'EOF'
function requestImage(self, imageData)
  local urlString = imageData.image
  if string.find(urlString, "^http://%d") and (
    string.find(urlString, "^http://10%.")
  ) then
  elseif not string.find(urlString, "^http://") then
    urlString = "lms-relative:" .. urlString
  else
    urlString = 'http://' .. jnt:getSNHostname() .. '/public/imageproxy?w=' .. screenWidth
  end
end
EOF
sh "$patcher" "$tmp/viewer.lua"
grep -Fq 'if string.match(urlString, "^https?://") then' "$tmp/viewer.lua"
grep -Fq '.route(urlString, screenWidth, screenHeight)' "$tmp/viewer.lua"
! grep -q '/public/imageproxy' "$tmp/viewer.lua"
cp "$tmp/viewer.lua" "$tmp/viewer.once"
sh "$patcher" "$tmp/viewer.lua"
cmp "$tmp/viewer.once" "$tmp/viewer.lua"

echo artwork-patch-tests-ok
