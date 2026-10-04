#!/bin/sh
set -eu

# Reconstruct the public Logitech Radio kernel tree. This is an offline build
# input only; it does not build firmware or communicate with a player.
SQUEEZEOS_COMMIT=bad080aecfec8226a4c1699b29d32cbba4ba396b
LINUX_SHA256=c6f94b0c35c5e6e6a4fe031f9279661816e84e77f072c356867926e3dd354a81
work=${1:-"$PWD/build/logitech-kernel"}
downloads=${DOWNLOAD_DIR:-"$PWD/.downloads"}
src_repo="$work/squeezeos"
kernel="$work/linux-2.6.26"
archive="$downloads/linux-2.6.26.tar.xz"

mkdir -p "$work" "$downloads"
if test ! -d "$src_repo/.git"; then
	git clone --filter=blob:none --no-checkout https://github.com/LMS-Community/squeezeos.git "$src_repo"
fi
git -C "$src_repo" fetch --depth 1 origin "$SQUEEZEOS_COMMIT"
git -C "$src_repo" checkout --detach "$SQUEEZEOS_COMMIT"

if test ! -f "$archive"; then
	curl -L --fail --output "$archive" \
		https://cdn.kernel.org/pub/linux/kernel/v2.6/linux-2.6.26.tar.xz
fi
echo "$LINUX_SHA256  $archive" | sha256sum -c -

rm -rf "$kernel"
tar -xJf "$archive" -C "$work"
cp -R "$src_repo/src/imx25/patches" "$kernel/patches"
(
	cd "$kernel"
	while IFS= read -r entry; do
		case "$entry" in ''|'#'*) continue ;; esac
		patch_name=${entry%% *}
		patch -p1 --batch --forward < "patches/$patch_name"
	done < patches/series
)

test "$(grep '^CONFIG_LOCALVERSION=' "$kernel/.config" || true)" = '' || true
grep -q '^CONFIG_MODVERSIONS=y$' "$kernel/.config"
echo "prepared $kernel"
echo "source commit $SQUEEZEOS_COMMIT"
echo "next: build the complete kernel with CodeSourcery 2010q1-202 to generate the exact Module.symvers"
