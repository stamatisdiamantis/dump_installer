# Changelog

## [1.0.0] — 2026-06-08

### Added

- **`.ffpfsc` / PFSC support** — outer PFSC container with auto-detected inner image (PFS via LVD + nullfs overlay, or exFAT direct to `/system_ex/app/TITLEID`)
- **Multi-file batch install** — install every game in a folder in one run; PFS-inner games first, exFAT-inner last
- **Storage auto-scan shortcuts** — Homebrew Options menu lists known paths with game counts (internal, USB, M.2, shadow mounts)
- **Autoload mode** — scan all storage and install/remount automatically; for Payload Manager autoloader or `--autoload`
- **Rich install toasts** — per-game “Installed” notification with icon and title (`NUC240`)
- **Batch-complete toast** — Dump Installer icon toast when a multi-game batch finishes
- **Browser progress logging** — step-by-step install/remount output in the Homebrew Launcher console
- **Grey icon cleanup** — Options entry to purge broken library entries from a games folder

### Changed

- **Smarter skip/remount/register planning** — live mounts, orphans after delete, and partial batches handled correctly
- **Nullfs slot management** — preflight, stale mount reclaim, and reboot warnings when PS5 nullfs limit is reached
- **Stable notifications** — registration steps run silently; progress in browser log, errors and toasts only where needed
- **Dev file logging disabled** — no `/data/dilogs/` output in release builds

### Fixed

- PFS-inner overlay path (nullfs bridge, save-data fallback, pfscache)
- Reinstall after game delete (orphan pre-purge, immediate per-game registration)
- Trophy and appmeta refresh after batch registration
- Stale nullfs mounts blocking new PFS installs without reboot

### Tested on

- PS5 firmware **10.6**
- Exploits **Y2JB**, **BDJB**
- kstuff **1.07 Test 2**
- Payload Manager **v0.3.0**
- Websrv / Homebrew Launcher **v0.28.3**

### Distribution

- **`dump_installer.elf`** — payload-only (Payload Manager autoload or manual send)
- **`dump_installer.zip`** — Homebrew Launcher package for `/data/homebrew/dump_installer/`
