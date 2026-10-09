# MGPU Bridge NR check (experimental)

This checks whether your NVIDIA driver and your `nvngx_dlssnr.dll` can start DLSS-NR on each of your GPUs. No game runs during the check. It does not change your driver, the add-on or any game files.

## Get it

Use one of these:

- **Download the build.** Open the repository's **Actions** tab, then **nrcheck (experimental)**, then the latest successful run. Download `nrcheck-experimental` under **Artifacts**. You need to be signed in to GitHub.
- **Build it yourself.** Fork the repository, enable Actions in your fork's **Actions** tab, then run **nrcheck (experimental)** with **Run workflow**. The artifact appears on that run.

## How to run

1. Close the game.
2. Copy `nrcheck.exe`, `nvngx.dll_nrcheck.dll` and `Verify-NRCheck.ps1` into the game folder, beside the `mgpu\` folder.
3. Right-click `Verify-NRCheck.ps1` > **Run with PowerShell**.
4. Attach `nrcheck_results.zip` to your GitHub issue.

If PowerShell refuses to run the script, open a terminal in the game folder and run `nrcheck.exe`. It writes `nrcheck_report.txt` and the `nvngx_*_start*_adapter*.log` files. Attach those instead.

## What it tests

For each GPU, the check starts the other GPU first, as a game does. It then tries to start DLSS-NR on this GPU up to eight times, and changes one thing between tries. It stops at the first success and reports which try worked.

The check takes about a minute.

## The bench (experimental, R268)

This measures what one GPU sustains for DLSS-NR at a resolution, without a game. No game runs. It reads nothing from a game and changes nothing.

1. Close the game.
2. In the game folder, beside the `mgpu\` folder, run:
   `nrcheck.exe --bench --neural <adapter index> --res 2560x1440 --passes 1 --frames 300`
   The adapter index is the one `nrcheck.exe` lists for your neural GPU. `--other <index>` creates the game's device first, as a game would.
3. It prints the time of each pass and whether a frame fits 60, 120 or 144 fps, and writes `nrbench_result.ini` beside `nrcheck.exe`.

The first 10 frames are warm-up and are not counted. Upscaling (SR) is not part of this bench yet.
