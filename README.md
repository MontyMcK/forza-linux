# forza-linux

A native Linux runtime for the Xbox One version of Forza Horizon 2.

The Xbox One CPU is x86-64, so the game's code runs directly on your CPU. This project provides the rest:
- a PE loader
- reimplementations of the Xbox system APIs the game imports
- D3D11.X graphics on Vulkan via DXVK

**No game files are included or will ever be.** You need your own legally obtained dump of the game.

Status: early bring-up. See [docs/PLAN.md](docs/PLAN.md).
Contributors and AI agents: start with [CLAUDE.md](CLAUDE.md).
