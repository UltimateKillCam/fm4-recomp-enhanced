# Project Forza Plus 4 (optional)

Modified 2026-10-23. GPL-3.0-only.

This edition can run the community mod **Project Forza Plus 4** (PFP4) from your own
extracted copy of the mod. It is **off by default**. When off, the stock game and your stock
saves are used exactly as before.

No mod files, game files or prepared data are included in this repository. You supply the
extracted mod folder; the launcher builds a separate, prepared copy on your machine.

## What it does

- Prepares a separate copy of your game with the mod's game files, the selected mode's
  database, and your physics/camera choices. The original game folder and the mod folder
  are only read, never changed.
- Keeps **separate saves per mode** in `.local/user/.fm4-plus/<mode>`. Stock saves are
  untouched. The first PFP4 start of a mode asks you to create a profile.
- Keeps the original game executable code. The game loads the mod's rebalanced data through
  a verified pre-start step (the mod's protected-file table is installed into fresh guest
  memory before the game runs). The mod's own executable is never used.

## Setup

1. Install Python 3.10 or newer (needed only when PFP4 is enabled).
2. Open **FM4 Launcher.cmd** and choose the **Forza Plus 4** tab.
3. Tick **Enable Project Forza Plus 4** and browse to the extracted mod folder (the folder
   containing `Main Game Changes`, `Game Mode DBs` and `Disc 2 and DLCs`).
4. Choose a career mode and options, then **Check readiness** (optional) and **Save and play**.

The first start of each option combination prepares a separate copy and needs
about 8 GB of free space. Later starts verify that copy before launch. Wheel
settings are copied into the mode’s save folder automatically.

## Options

| Option | Values | Notes |
| --- | --- | --- |
| Career mode | Normal, Hard, Sandbox, Vanilla | Each mode has its own saves. |
| Physics | Stock (mod default), 100, 120, 144, 165, Custom | Custom maps target FPS to `CarPhysicsUpdateHz = round(FPS * 5.79)`. This is the simulation rate, independent of display FPS or upscaling. |
| Camera FOV | Inherit, or a value per camera | Follow low/high, hood, high bumper, driver. Inherit keeps the mod/original value. |
| Optional update folder | An extracted official PFP4 update | Applied over the base mod. |
| Harder-AI file | Not supported yet | Choosing one blocks readiness until a verified mapping exists. |

Changing options prepares a new copy on the next start; existing saves are kept.

## Content packages

The mod’s Disc 2 and DLC packages are carried into its prepared mode. Extracted
packages without a header receive a standard content header so the game can
mount them. Mounted packages appear as `Content opened:` in the game log.

## Troubleshooting

- **"Project Forza Plus 4 is not ready"**: use **Check readiness** to see the reason.
- If the game closes immediately in PFP4 mode, read `.local/forza-plus/last-launch-error.txt`.
- Keep the edition folder path reasonably short; the mod's content uses deep folders.
- Only one game instance per edition can run at a time.

## Limitations

Normal mode with stock physics and inherited cameras has reached race start and
driving. Hard, Sandbox and Vanilla modes, physics presets, custom FOVs, official
updates and full career compatibility remain unverified in gameplay.
