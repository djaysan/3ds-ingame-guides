# 3DS In-Game Guides

Read a GameFAQs walkthrough over any 3DS game, on the console, without leaving
the game. Hold **L+R+Select**, the game pauses and the guide appears on the top
screen. Press **B** and the game carries on exactly where it was. The page you
were on is remembered per game, across restarts.

A [Luma3DS](https://github.com/LumaTeam/Luma3DS) plugin built on
[CTRPluginFramework](https://github.com/PabloMK7/CTRPluginFramework-BlankTemplate).
Works on old 3DS, 3DS XL, 2DS and New 3DS with Luma3DS 10.3 or newer.

## Install

1. Download [3DS-InGame-Guides.zip](https://github.com/djaysan/3ds-ingame-guides/releases/latest/download/3DS-InGame-Guides.zip)
   and copy its `luma` folder to the root of your SD card. You end up with
   `SD:/luma/plugins/default.3gx`.
2. Put the guides on the card with the browser tool at
   [guides.retromodlab.com/app](https://guides.retromodlab.com/app): pick the
   card, it finds your installed games and writes one text file per game to
   `SD:/guides/3ds/`. Cartridge games can be added by name.
3. On the console open the Rosalina menu (**L+Down+Select**), turn on
   **Plugin loader**, and start a game.

In the game: **L+R+Select** opens the guide. **Up/Down** one line (hold to
scroll), **L/R** one page, **B** closes. Plain Select stays with the game.

## Guide files

`SD:/guides/3ds/<title id low>.txt`, where the title id low is the last 8 hex
digits of the game's title id, upper case. Example: The Legend of Zelda: A Link
Between Worlds (EUR) is `000EC400.txt`. Plain ASCII, one line per screen line
(wrap at 64 columns), LF line endings. If a game has no guide the plugin says
so and shows the file name it looked for. The reading position lives next to
the guide as `<title id low>.pos`.

This repository hosts no guide content. The tool fetches the text from the
public GameFAQs archive on archive.org and converts it on your machine.

## Known limits

- Homebrew ports (libctru apps) cannot run CTRPF plugins. The plugin does not
  load into them.
- A few memory-hungry games will not boot with any plugin loaded on an old 3DS.
  Mario & Luigi: Bowser's Inside Story + Bowser Jr.'s Journey is one. Turn the
  plugin loader off for those.
- The plugin takes 5 MiB, the smallest block CTRPF 0.8.0 starts with.

## Build

Docker only, no toolchain install:

```sh
./build.sh        # produces default.3gx via the pablomk7/libctrpf:0.8.0 image
```

The pager logic has a host-side check: `clang++ -std=c++17 -I Sources tests/pager_test.cpp -o /tmp/pager_test && /tmp/pager_test`.

## Credits

[PabloMK7](https://github.com/PabloMK7) for the maintained CTRPluginFramework
and its Docker image, the Luma3DS team for the plugin loader, and the GameFAQs
authors whose walkthroughs this reads. Part of the
[RetroModLab guides](https://guides.retromodlab.com) family, alongside the
DS/DSpico and Miyoo/Onion readers.
