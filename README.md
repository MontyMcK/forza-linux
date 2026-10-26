# forza-linux

A native Linux runtime for the Xbox One version of Forza Horizon 2.

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
- **Settings**: game folder, runtime, window size, sound, GameMode, skipping the long movies, Anthem_Tone_Video and the new-game IE_opening (developer builds only, see CMakeLists.txt `FL_DEV_SKIP_MOVIES`).
- **Developer**: MangoHud (layout, items, position, FPS limit), DXVK HUD and log level, Vulkan validation, `FL_LOG` channels, test automation (`FL_EXIT_AFTER_FRAME`, `FL_SCREENSHOT_AT_FRAME`, `FL_AUTOPRESS`, a separate test save), extra environment variables and arguments, the exact command, and a rebuild button.
- **Saves**: back up, restore, or start fresh. Replaced saves are always moved to `~/.local/share/forza-linux-backups/` first.
- **Logs**: live output with filters. Every session's full output is kept in `~/.cache/forza-linux/logs/`.

Settings live in `~/.config/forza-linux/launcher.json`. The launcher shows art from your own game folder. The desktop icon is made from your game's logo and is written only to `~/.local/share/icons`. Neither is part of this repository.
