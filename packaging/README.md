# packaging

## Arch Linux (local build)

```sh
packaging/arch/build-and-install.sh              # build + sudo pacman -U
packaging/arch/build-and-install.sh --no-install # build only
```

The script tars the working tree into `raylyrics-$pkgver.tar.gz` and runs
`makepkg -f`; extra arguments are forwarded to makepkg (e.g. `--noconfirm`).
It needs `base-devel` and the dependencies listed in `PKGBUILD`.

After installing:

- the binary is `/usr/bin/raylyrics`
- the shipped presets are in `/usr/share/raylyrics/presets/`, with
  `/usr/share/raylyrics/config.example.lua` as the sample config
- user configuration still lives in `~/.config/raylyrics/`; a preset of the same
  name in the user directory wins, and `require` searches it first
- runtime dependencies: `wayland libglvnd fontconfig glib2 libsoup3 cjson lua54`
