# DuckStation for PS4

**PS4 port by Neoma** ([NeomaMonroy](https://github.com/NeomaMonroy))

A port of the [DuckStation](https://github.com/stenzek/duckstation) PlayStation 1 emulator to jailbroken PS4 consoles
running **GoldHEN**. It is built from [RSDuck's Switch fork](https://github.com/RSDuck/duckstation) (GPL-3.0) and
runs DuckStation's Big Picture interface full screen at 1920x1080.

**Status:** runs on PS4 hardware. Games play at full speed with the Vulkan hardware renderer.

**You bring your own BIOS and games.** No BIOS, games or other Sony files are included in this repository or in the
packages.

## Features

- Vulkan hardware renderer on the PS4 GPU (Mesa's RADV driver), with resolution scaling up to 16x
- PGXP (fixes the PS1's wobbly polygons and warped textures), true color, texture filtering (xBR, bilinear and more)
- Fast CPU recompiler (JIT), with the interpreter as a fallback
- CUE/BIN, CHD, PBP and ISO games, from the PS4's internal storage or a USB drive
- Two DualShock 4 controllers with rumble, analog sticks and the touch pad
- Memory cards per game, save states, and resume where you left off
- Saved shader and pipeline caches, so games start faster after the first time

## Download

Open the **Releases** page on the right of this page and download **`DuckStation-vXX.YY.pkg`**. Copy it to a USB drive
(exFAT) and install it with GoldHEN's **Package Installer**.

## Setting up the console

The first start creates `/data/DuckStation` on the PS4. Put your files there over FTP (GoldHEN's FTP server, port
2121) or with a file manager.

| What | Where |
|---|---|
| PS1 BIOS (`scph5501.bin`, `scph1001.bin`, `scph101.bin`, ...) | `/data/DuckStation/bios/` |
| Games (folders are fine) | `/data/DuckStation/games/` |
| Games on a USB drive | `PS1` folder on the drive (`/mnt/usb0/PS1`, `/mnt/usb1/PS1`) |
| Settings | `/data/DuckStation/settings.ini` |
| Per-game settings | `/data/DuckStation/gamesettings/` |
| Memory cards / save states | `/data/DuckStation/memcards/`, `/data/DuckStation/savestates/` |
| Logs | `/data/DuckStation/duckstation.log` and `ps4.log`, crashes in `crash.log`, freezes in `freeze.log` |

**Controls:** the PS4 buttons match the PS1 ones. Options = Start, touch pad click = Select. **L3 + R3** opens
DuckStation's menu in a game (settings, save states, quit).

**Default settings:** recompiler, Vulkan at 3x resolution, PGXP on, VSync. Everything can be changed in the menu.

## Build it yourself

### Option 1: in the cloud, no setup (any PC, including Windows)

GitHub builds the package for you. Nothing is installed on your PC.

1. Open the **Actions** tab of this repository.
2. Pick **Build PS4 package** on the left, then **Run workflow > Run workflow**.
3. Wait for the green check mark. The first build takes 2 to 3 hours, because it also builds the PS4 toolchain.
   Later builds reuse the toolchain and are much faster.
4. Open the finished run and download **DuckStation-PS4-pkg** under **Artifacts**. It's a ZIP with the `.pkg` inside.

To publish a release, type a version such as `00.04` in the **Version** box before clicking the green
**Run workflow** button. The package is attached to a new release automatically, as `DuckStation-v00.04.pkg`.

To build from your own copy, click **Fork** at the top of this page first, then run the steps above on your fork.

### Option 2: Windows with WSL

The build runs inside Ubuntu on Windows (WSL 2). Windows 10 (2004 or newer) or Windows 11.

1. Open **PowerShell as Administrator** and run:

   ```powershell
   wsl --install -d Ubuntu-24.04
   ```

   Restart the PC when it asks, then open **Ubuntu 24.04** from the Start menu and pick a user name and password.
2. On this GitHub page, click **Code > Download ZIP**.
3. In File Explorer's address bar, type `\\wsl$\Ubuntu-24.04\home\<your Ubuntu user name>` and copy the ZIP there.
   Keep everything inside Ubuntu's home folder: building from `C:\` is many times slower and breaks on paths with spaces.
4. In the Ubuntu window:

   ```bash
   sudo apt update && sudo apt install -y unzip
   cd ~
   unzip -o DuckStation-PS4-main.zip
   rm -rf duckstation-ps4 && mv DuckStation-PS4-main duckstation-ps4
   cd duckstation-ps4
   chmod +x setup-everything.sh
   ./setup-everything.sh
   ```

5. When it finishes, run `explorer.exe out` to open the folder with **`DuckStation-vXX.YY.pkg`** in Windows.

If the build runs out of memory, create `C:\Users\<you>\.wslconfig` with these lines, run `wsl --shutdown` in
PowerShell, and open Ubuntu again (add `JOBS=4` in front of the build command too):

```ini
[wsl2]
memory=12GB
swap=16GB
```

### Option 3: Windows with a virtual machine

If WSL isn't available (older Windows, or virtualization blocked for WSL), install
[VirtualBox](https://www.virtualbox.org/) and create an **Ubuntu 24.04** virtual machine with at least 8 GB of RAM,
4 CPU cores and 60 GB of disk. Then follow the Linux steps below inside it.

### Linux (Ubuntu 24.04)

Needs the shared PS4 toolchain folder `~/dk64-build` (OpenOrbis via PacBrew, clang 21, the PS4 sysroot and RADV).
The first run installs or builds whatever is missing.

```bash
cd ~/duckstation-ps4
./setup-everything.sh                               # makes out/DuckStation-vXX.YY.pkg
PKG_OUT=/media/$USER/USB ./setup-everything.sh      # put the .pkg somewhere else
PS4_HOST=<your PS4's IP> ./setup-everything.sh      # also send it to the PS4 over Wi-Fi
```

If a step fails, fix it and run the same command again: finished steps are skipped.

## Repository layout

| Folder | Contents |
|---|---|
| `patches/` | The PS4 changes to DuckStation and to Mesa |
| `ps4/` | PS4 host layer (JIT memory, shared memory, logs, monitor), Vulkan swapchain on VideoOut (`ps4_wsi.cpp`) |
| `port/` | PS4 runtime fixes: heap, threads, files, networking, crash log, RADV's PS4 GPU backend |
| `cmake/`, `toolchain/` | PS4 build setup and toolchain |
| `scripts/` | Build, package and upload steps |
| `assets/` | Package artwork |

## Credits and licenses

DuckStation is by Stenzek and its contributors. This port is based on RSDuck's Switch fork, which is GPL-3.0, so this
port is GPL-3.0 too (see [LICENSE](LICENSE)). Mesa is MIT, glslang is BSD-3-Clause and other permissive licenses,
and the VideoOut swapchain is adapted from Vita3K (GPL-2.0-or-later). Built with the
[OpenOrbis](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain) toolchain from PacBrew.

PlayStation is a trademark of Sony Interactive Entertainment. This fan project has no ties to Sony or to the
DuckStation project. Only play games you own.
