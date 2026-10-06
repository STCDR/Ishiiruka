# Building Local Teams on Windows

Use the paired Dolphin, Rust and ASM revisions. The Rust submodule is pinned in
Dolphin; `DUBS-SOURCE-REVISIONS.txt` records the matching ASM commit.

Install Visual Studio 2022 Community with Desktop development with C++, MSVC
v143 and Windows SDK 10.0.26100.0, Git, and stable Rust with the
`x86_64-pc-windows-msvc` target. Keep this checkout layout:

```rust
git clone --branch dubs --recurse-submodules https://github.com/STCDR/Ishiiruka.git Ishiiruka
git clone --branch dubs https://github.com/STCDR/slippi-ssbm-asm.git slippi-ssbm-asm
git -C slippi-ssbm-asm checkout cf31f810a34845a2baf5bafbf5256d26c717301e
```

Put Gecko 5.0.0's `gecko.exe`, `powerpc-eabi-as.exe` and
`powerpc-eabi-objcopy.exe` in `tools/gecko` beside the two checkout folders.
Download it from [Gecko releases](https://github.com/JLaferri/gecko/releases/tag/v5.0.0).
Extract [Microsoft.DXSDK.D3DX 9.29.952.8](https://www.nuget.org/packages/Microsoft.DXSDK.D3DX/9.29.952.8)
into `tools/d3dx` at the same level, preserving its `build/native` layout.

From the parent folder, run:

```rust
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Ishiiruka\Tools\LocalTeams\build.ps1 -Rebuild
```

The script assembles the matching Melee codes, builds Release/x64 Dolphin and
Rust, and copies the application into `runtime`. C/C++ and Rust use consistent
source locations for reproducible builds.
Run `-Rebuild` when changing from an older build to regenerate all C/C++ objects.

For distribution, copy the executable, DLLs, `Sys`, `Languages`, `license.txt`
and `portable.txt` from `runtime`. Include the VC143 x64 runtime DLLs and original
license notices. Generate an empty `User` profile from
`Ishiiruka/Externals/SlippiRustExtensions/user/defaults`; never copy a development
profile. Do not include logs, PDBs, object files, game images or account files.

Use your preferred ZIP tool to archive the portable folder.

Source is available in these forks, including the pinned Rust submodule and
matching ASM revision; an additional source ZIP is not required for this layout.
The bundled upstream FFmpeg 3.2.4 and OpenAL Soft 1.16.0 libraries use their
unmodified upstream sources: [FFmpeg 3.2.4](https://ffmpeg.org/releases/ffmpeg-3.2.4.tar.xz)
and [OpenAL Soft 1.16.0](https://openal-soft.org/openal-releases/openal-soft-1.16.0.tar.bz2).
Their license texts remain in the application package.
