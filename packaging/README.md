# packaging

## Arch Linux（本地构建）

```sh
packaging/arch/make-local-package.sh          # 生成 raylyrics-<ver>.pkg.tar.zst
sudo pacman -U packaging/arch/raylyrics-<ver>-1-x86_64.pkg.tar.zst
```

脚本把当前工作树打成 `raylyrics-$pkgver.tar.gz`，再调用 `makepkg -f`；额外参数会转发给 makepkg
（如 `--noconfirm`）。需要 `base-devel` 以及 `PKGBUILD` 里列出的依赖。

安装后：

- 可执行文件在 `/usr/bin/raylyrics`
- 内置 preset 在 `/usr/share/raylyrics/presets/`，示例配置在 `/usr/share/raylyrics/config.example.lua`
- 用户配置仍在 `~/.config/raylyrics/`；preset 同名时**用户目录优先**，`require` 也会先查用户目录
- 运行期依赖：`wayland libglvnd fontconfig glib2 libsoup3 cjson lua54`
