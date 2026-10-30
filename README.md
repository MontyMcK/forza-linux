# Open Horizon 2

An open-source, native Linux runtime for the Xbox One version of Forza Horizon 2. (The runtime binary is still called `forza-linux`.)

The Xbox One CPU is x86-64, so the game's code runs directly on your CPU. This project provides the rest:
- a PE loader
- reimplementations of the Xbox system APIs the game imports
- D3D11.X graphics on Vulkan via DXVK

**No game files are included or will ever be.** You need your own legally obtained dump of the game.

Status: early bring-up. See [docs/PLAN.md](docs/PLAN.md).
Contributors and AI agents: start with [CLAUDE.md](CLAUDE.md).

## Launcher

`launcher/forza_launcher.py` is a desktop launcher (Python + PySide6: `sudo dnf install python3-pyside6`):

```bash
python3 launcher/forza_launcher.py                     # open it
python3 launcher/forza_launcher.py --install-shortcuts # app menu entry + desktop icon
python3 launcher/forza_launcher.py --print-command     # show what Play would run
```

- **Play**: starts `build/forza-linux` with your game folder and shows the game's state (running, exited, crashed).
- **Settings**: game folder, runtime, display mode (windowed, borderless window or exclusive fullscreen; F11 / Alt+Enter switches in game), window size, frame rate (30 fps console timing by default, or dynamic: any frame rate with the game at normal speed, with an FPS cap), sound, GameMode, skipping the long movies, Anthem_Tone_Video and the new-game IE_opening (developer builds only, see CMakeLists.txt `FL_DEV_SKIP_MOVIES`).
- **Developer**: MangoHud (layout, items, position, FPS limit), DXVK HUD and log level, Vulkan validation, `FL_LOG` channels, test automation (`FL_EXIT_AFTER_FRAME`, `FL_SCREENSHOT_AT_FRAME`, `FL_AUTOPRESS`, a separate test save), extra environment variables and arguments, the exact command, and a rebuild button.
- **Saves**: back up, restore, or start fresh. Replaced saves are always moved to `~/.local/share/forza-linux-backups/` first.
- **Logs**: live output with filters. Every session's full output is kept in `~/.cache/forza-linux/logs/`.

Settings live in `~/.config/forza-linux/launcher.json`. The launcher shows art from your own game folder. The desktop icon is made from your game's logo and is written only to `~/.local/share/icons`. Neither is part of this repository.

## PS5 (work in progress)

A native homebrew title for jailbroken PS5 consoles is being built on the `ps5-test` branch: `docs/PS5_PORT.md` has the plan and its status. It doesn't run the game yet. What a console will need:

- A jailbroken PS5 with **[ShadowMount+](https://github.com/drakmor/ShadowMountPlus) 1.7beta4 or newer** (1.7beta11 recommended). Earlier versions don't give titles access to `/data` or USB drives, so Open Horizon 2 can't see your game files: a homebrew title can otherwise only read its own folder.
- An ELF loader on port 9021 ([elfldr](https://github.com/ps5-payload-dev/elfldr)) and an FTP server on port 2121 ([ftpsrv](https://github.com/ps5-payload-dev/ftpsrv)) for development.
- Your own dump of the game on the console's internal storage (for example `/data/OpenHorizon2/<folder>/Mount`) or on an exFAT/FAT32 USB drive with an MBR partition table (the PS5 can't see GPT drives). No game files ship with the title.

## License

Open Horizon 2 is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version. It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See [LICENSE](LICENSE) for the full text.

Third-party code keeps its own license: AMD addrlib in `third_party/addrlib` (MIT), DXVK in `third_party/dxvk` (zlib), and FFmpeg, which `tools/build_ffmpeg_xma.sh` downloads and builds (LGPL-2.1-or-later). PS5 builds also link PS5_Vulkan and the PS5 payload SDK (both GPL-3.0-or-later); see [docs/PS5_PORT.md](docs/PS5_PORT.md).

The game itself is not part of this project and is not covered by this license. No game files are included.
