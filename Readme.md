# Slippi Dolphin: Matchmaking Teams with multiple local players

## TL;DR

**[Download the Windows ZIP](https://github.com/STCDR/Ishiiruka/releases)**, extract it and run `Slippi Dolphin.exe`.

In Teams CSS, move P1's cursor into the **top-left grey corner** and press **A** to cycle **1-4 local players**. Everyone picks a character/team and presses **Start**, then enter your usual Teams room code.

![Change the local player count in Teams](docs/assets/local-player-toggle.gif)

First launch offers to import your Launcher Dolphin settings, controllers and account if this profile is still untouched.

## TL;DR for devs

- **Dolphin:** Each local player gets a Slippi netplay client in one process, sharing one Melee simulation and rollback engine. Extra locals feed inputs over ENet as remote peers. Dolphin handles slot mapping and synchronizes the clients.
- **ASM:** Extended Teams CSS for 1-4 local cursors, independent character/costume/team picks and Start confirmations. Custom EXI commands (`0xC5`–`0xC8`) exchange controller data, selections and readiness with Dolphin, handle roster changes and restore picks for rematches.

Existing matchmaking and remote packet formats are unchanged. Ship the paired executable, DLL and codeset together.

Source: [Dolphin](https://github.com/STCDR/Ishiiruka), [ASM](https://github.com/STCDR/slippi-ssbm-asm), [Rust](https://github.com/STCDR/slippi-rust-extensions), or the matching source ZIP.

Windows x64 preview, based on [Slippi Dolphin 3.6.4 / Ishiiruka](https://github.com/project-slippi/Ishiiruka). Currently proposes Battlefield. Original licenses and notices are retained.
