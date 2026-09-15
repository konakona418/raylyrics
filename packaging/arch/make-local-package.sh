#!/usr/bin/env bash
# Build an Arch package from this repository's working tree.
#
#   packaging/arch/make-local-package.sh [makepkg args...]
#
# Extra arguments are forwarded to makepkg (e.g. --noconfirm, -C).
# Requires base-devel (makepkg) and the dependencies listed in PKGBUILD.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"

pkgver="$(sed -n 's/^pkgver=//p' "$here/PKGBUILD")"
tarball="$here/raylyrics-$pkgver.tar.gz"

echo "==> packing $repo -> $tarball"
rm -f "$tarball"
tar -C "$repo" \
  --exclude=.git \
  --exclude=build \
  --exclude=install \
  --exclude=.cache \
  --exclude=compile_commands.json \
  --exclude='*.tar.gz' \
  --exclude='*.pkg.tar.*' \
  --transform "s,^\.,raylyrics-$pkgver," \
  -czf "$tarball" .

cd "$here"
exec makepkg -f "$@"
