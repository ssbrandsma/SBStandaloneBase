#!/bin/sh
set -eu

TOOLCHAIN_SHA256=60791a1fea52f6cf229fd71147abf44fe9f8d776edabb1095883a6577842a1d8
MAKE_SHA256=16b77de9f013bcd536b7bc1efbe314223aedfe250f9063e33cbb4dfd347215a2
root=${1:-"$PWD/build/legacy-kernel-tools"}
downloads=${DOWNLOAD_DIR:-"$PWD/.downloads"}
tc_archive="$downloads/arm-2010q1-202-arm-none-linux-gnueabi-i686-pc-linux-gnu.tar.bz2"
make_archive="$downloads/make-3.81.tar.gz"

mkdir -p "$root" "$downloads"
fetch() {
	file=$1 url=$2 hash=$3
	if test ! -f "$file"; then curl -L --fail --output "$file" "$url"; fi
	echo "$hash  $file" | sha256sum -c -
}

fetch "$tc_archive" \
	https://sources.buildroot.net/arm-2010q1-202-arm-none-linux-gnueabi-i686-pc-linux-gnu.tar.bz2 \
	"$TOOLCHAIN_SHA256"
fetch "$make_archive" https://ftp.gnu.org/gnu/make/make-3.81.tar.gz "$MAKE_SHA256"

rm -rf "$root/toolchain" "$root/make-src" "$root/make"
mkdir -p "$root/toolchain" "$root/runtime"
tar -xjf "$tc_archive" -C "$root/toolchain"

# CodeSourcery's host tools are i386 binaries. Obtain, but do not install,
# Ubuntu's compatibility runtime and patchelf packages.
(
	cd "$root/runtime"
	apt-get download libc6-i386 patchelf
	for deb in ./*.deb; do dpkg-deb -x "$deb" root; done
)
loader=$(find "$root/runtime/root" -type f -name ld-linux.so.2 | head -n 1)
libdir=$(dirname "$loader")
patchelf=$(find "$root/runtime/root" -type f -name patchelf | head -n 1)
test -x "$loader" && test -x "$patchelf"

find "$root/toolchain/arm-2010q1/bin" \
	"$root/toolchain/arm-2010q1/libexec" -type f \
	-exec "$patchelf" --set-interpreter "$loader" '{}' ';' 2>/dev/null || true

mkdir "$root/make-src"
tar -xzf "$make_archive" -C "$root/make-src" --strip-components=1
(
	cd "$root/make-src"
	./configure --prefix="$root/make"
	# GNU make 3.81's bundled glob expects glibc-private aliases removed later.
	sed -i 's/__alloca/alloca/g; s/__stat/stat/g' glob/glob.c
	make -j2
	make install
)

cat > "$root/env.sh" <<EOF
export SB_LEGACY_TOOLS='$root'
export LD_LIBRARY_PATH='$libdir'
export PATH='$root/toolchain/arm-2010q1/bin:$root/make/bin':\$PATH
export CROSS_COMPILE='$root/toolchain/arm-2010q1/bin/arm-none-linux-gnueabi-'
export MAKE='$root/make/bin/make'
EOF

# Exercise both the driver and its cc1 child before reporting success.
echo 'int sb_toolchain_probe;' > "$root/probe.c"
LD_LIBRARY_PATH="$libdir" \
	"$root/toolchain/arm-2010q1/bin/arm-none-linux-gnueabi-gcc" \
	-c "$root/probe.c" -o "$root/probe.o"
file "$root/probe.o"
echo "source $root/env.sh"

