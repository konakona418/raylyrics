#!/usr/bin/env bash
# Build the Arch package from this working tree and install it.
#
#   packaging/arch/build-and-install.sh [--no-install] [makepkg args...]
#
# Needs base-devel (makepkg); the install step uses sudo pacman.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

install=1
extra=()
for arg in "$@"; do
  case "$arg" in
    --no-install) install=0 ;;
    *) extra+=("$arg") ;;
  esac
done

"$here/make-local-package.sh" --noconfirm ${extra[@]+"${extra[@]}"}

pkgver="$(sed -n 's/^pkgver=//p' "$here/PKGBUILD")"
pkgrel="$(sed -n 's/^pkgrel=//p' "$here/PKGBUILD")"
package="$here/raylyrics-$pkgver-$pkgrel-$(uname -m).pkg.tar.zst"

if [[ ! -f "$package" ]]; then
  package="$(ls -t "$here"/raylyrics-"$pkgver"-"$pkgrel"-*.pkg.tar.zst 2>/dev/null | head -n1 || true)"
fi
if [[ ! -f "$package" ]]; then
  echo "error: built package not found in $here" >&2
  exit 1
fi

if (( install )); then
  echo "==> installing $package"
  sudo pacman -U --noconfirm "$package"
else
  echo "==> built $package (not installed)"
fi
