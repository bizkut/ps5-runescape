# Title artwork

`rs_bg.jpg` (2560x1440, SHA-256
`d3b2c0209e952cdb2c658f071e6acc5b5644bd68a9de6629164c4c2510508924`) is artwork
supplied by the project owner, who states it is their own work. It is the
source of `ps5/title/sce_sys/pic0.dds`, which `ps5/title/build.sh` installs as
the PS5 home-screen tile background (`sce_sys/pic0.dds`), the launch background
(`sce_sys/pic1.dds`), and the desktop picture behind the Jagex Launcher
(`assets/desktop.bgra`).

Regenerate the DDS after changing the picture:

```sh
python3 ps5/title/tools/make-title-background.py --source ps5/title/art/rs_bg.jpg
```

`rs.png` (900x512, SHA-256
`a15347cfc210287ba2ba2363b9198aed70de1c273937c190676f64f50dacd78b`) is the
project owner's logo, likewise stated to be their own work. Its round logo is
cut out and fills `ps5/title/sce_sys/icon0.png`, the 512x512 home-screen tile
icon:

```sh
python3 ps5/title/tools/make-title-icon.py
```
