# Slippi Dolphin: Matchmaking Teams with multiple local players


**[Download the Windows ZIP](https://github.com/STCDR/Ishiiruka/releases)**, extract it and run `Slippi Dolphin.exe`.

First launch offers to import your Slippi Launcher Dolphin settings, controllers and account if this profile is still untouched.

In Teams CSS, move P1's cursor into the **top-left grey corner** and press **A** to cycle **1-4 local players**. Everyone picks a character/team and presses **Start**, then enter your usual Teams room code.

![Change the local player count in Teams](docs/assets/local-player-toggle.gif)

## TL;DR for devs

- **Dolphin:** Each local player gets a Slippi netplay client in one process, sharing one Melee simulation and rollback engine. Extra locals feed inputs over ENet as remote peers. Dolphin handles slot mapping and synchronizes the clients.
- **ASM:** Extended Teams CSS for 1-4 local cursors, independent character/costume/team picks and Start confirmations. Custom EXI commands (`0xC5`–`0xC8`) exchange controller data, selections and readiness with Dolphin, handle roster changes and restore picks for rematches.



Source: [Dolphin](https://github.com/STCDR/Ishiiruka), [Rust](https://github.com/STCDR/slippi-rust-extensions), [ASM](https://github.com/STCDR/slippi-ssbm-asm). [Windows build instructions](docs/LOCAL-TEAMS-BUILD.md).

**Disclaimer**: Portions of the codebase have been generated with the assistance of AI, including code snippets, functions, classes, structs, methods, and files. AI assistance may also have been used to produce comments, docstrings, type signatures, and project documentation.
