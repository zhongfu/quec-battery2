#!/bin/sh
#
# Build an OpenWrt/opkg .ipk for the quec_battery reimplementation.
#
# Why the payload is named quec_battery2 and not quec_battery:
#   On the Mudi 7, /usr/bin/quec_battery is owned by the vendor "quec_battery"
#   opkg package. A second package that ships the same path is rejected by opkg
#   ("check_data_file_clashes: ... already provided by package quec_battery").
#   So this package ships /usr/bin/quec_battery2, and its control scripts stash
#   the stock daemon as /usr/bin/quec_battery.stock and symlink the live path
#   /usr/bin/quec_battery to it. The stock package is left registered, so the
#   vendor init script and config keep working unchanged.
#
# Requires GNU tar (--sort/--owner/--mtime) and a prebuilt target binary.
#
# Usage:
#   make clean && make CC=aarch64-linux-musl-gcc
#   VERSION=1.0.0 ARCH=aarch64_cortex-a53 packaging/build-ipk.sh
#
# Environment:
#   PKG_NAME    package name           (default: quec-battery2)
#   VERSION     package version        (default: git describe --tags)
#   ARCH        opkg architecture      (default: aarch64_cortex-a53)
#   BIN         prebuilt target binary (default: ./quec_battery)
#   OUT_DIR     output directory       (default: ./dist)
#   MAINTAINER  control Maintainer     (default: quec-battery2 package)
#
# Produces: $OUT_DIR/${PKG_NAME}_${VERSION}_${ARCH}.ipk

set -eu

PKG_NAME=${PKG_NAME:-quec-battery2}
ARCH=${ARCH:-aarch64_cortex-a53}
MAINTAINER=${MAINTAINER:-quec-battery2 package}

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BIN=${BIN:-$ROOT/quec_battery}
OUT_DIR=${OUT_DIR:-$ROOT/dist}

if [ -z "${VERSION:-}" ]; then
	if command -v git >/dev/null 2>&1 &&
		git -C "$ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
		VERSION=$(git -C "$ROOT" describe --tags --always --dirty 2>/dev/null || echo 0.0.0)
	else
		VERSION=0.0.0
	fi
fi
# Release tags are v-prefixed; opkg versions are not.
VERSION=${VERSION#v}

if [ ! -f "$BIN" ]; then
	echo "error: target binary not found: $BIN" >&2
	echo "       build it first, e.g. make CC=aarch64-linux-musl-gcc" >&2
	exit 1
fi

if ! tar --version 2>/dev/null | grep -q 'GNU tar'; then
	echo "error: GNU tar is required (for --sort/--owner/--mtime)" >&2
	exit 1
fi

size=$(wc -c < "$BIN" | tr -d ' ')

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT INT TERM

mkdir -p "$tmp/data/usr/bin" "$tmp/control"

cp "$BIN" "$tmp/data/usr/bin/quec_battery2"
chmod 0755 "$tmp/data/usr/bin/quec_battery2"

sed \
	-e "s|@PKG@|$PKG_NAME|g" \
	-e "s|@VERSION@|$VERSION|g" \
	-e "s|@ARCH@|$ARCH|g" \
	-e "s|@SIZE@|$size|g" \
	-e "s|@MAINTAINER@|$MAINTAINER|g" \
	"$ROOT/packaging/ipk/control.in" > "$tmp/control/control"

for script in preinst postinst prerm postrm; do
	cp "$ROOT/packaging/ipk/$script" "$tmp/control/$script"
	chmod 0755 "$tmp/control/$script"
done

# Reproducible archives: stable ordering, ownership, and timestamps.
tar --sort=name --owner=0 --group=0 --numeric-owner --mtime='@0' \
	-C "$tmp/control" -czf "$tmp/control.tar.gz" .
tar --sort=name --owner=0 --group=0 --numeric-owner --mtime='@0' \
	-C "$tmp/data" -czf "$tmp/data.tar.gz" .
printf '2.0\n' > "$tmp/debian-binary"

mkdir -p "$OUT_DIR"
out="$OUT_DIR/${PKG_NAME}_${VERSION}_${ARCH}.ipk"
tar --sort=name --owner=0 --group=0 --numeric-owner --mtime='@0' \
	-C "$tmp" -czf "$out" debian-binary control.tar.gz data.tar.gz

echo "built $out"
