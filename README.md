# Homebrew Dump Installer

Run homebrew apps on your PS5 using the Kstuff, Websrv payloads and Homebrew Launcher.

Fork of [EchoStretch/dump_installer](https://github.com/EchoStretch/dump_installer) with **`.ffpfsc` / PFSC support**, **multi-file batch install**, **storage auto-scan shortcuts**, and **trophy / reinstall fixes**.

### Tested on

| Component | Version |
|-----------|---------|
| PS5 firmware | **10.6** |
| Exploit | **Y2JB** |
| kstuff | **1.07 Test 2** |
| Payload Manager | **v0.3.1** |
| Websrv / Homebrew Launcher | **v0.28.3** |

Other firmware, exploits, or payload versions may work but are untested with this fork.

---

### ✅ Installation & Launch

1. **Copy your dump_installer folder** to:
   - `/data/homebrew/`
   - `/mnt/usb#/homebrew/` *(replace `#` with your USB number, e.g., `usb0`, `usb1`, etc.)*
   - `/mnt/ext#/homebrew/` *(replace `#` with your EXT number, e.g., `ext0`, `ext1`, etc.)*

2. **Install the Homebrew Launcher** and send the Websrv payload to your console:  
   👉 [Websrv v0.28.3](https://github.com/ps5-payload-dev/websrv/releases/tag/v0.28.3)

3. **Open the Homebrew Launcher.**  
   Your Dump Installer should appear. **Navigate to the homebrew and run it.**

---

### 📌 Example Folder Structure

**On the PS5 (homebrew app):**

`/data/homebrew/dump_installer/`, `/mnt/usb0/homebrew/dump_installer/` or `/mnt/ext0/homebrew/dump_installer/`

- `dump_installer.elf`
- `homebrew.js`
- `sce_sys/icon0.png`

**Game dumps** (separate from the homebrew folder — usually directly in `/data/homebrew/`):

```
/data/homebrew/
├── PPSA17599.ffpfsc
├── PPSA23226.ffpfsc
└── PPSA26344.exfat.ffpfsc
```

Image files must sit **in the folder you select** (not buried in subfolders). Use a `PPSA` / `CUSA` prefix in the filename when possible.

---

### 📀 Supported game files

Place dumps in **one folder** (e.g. `/data/homebrew/`). The installer picks up:

| Extension | Type |
|-----------|------|
| `.ffpfsc` | PFSC container (compressed dumps; inner image auto-detected) |
| `.ffpfs` / `.pfs` | PFS image |
| `.ffpkg` / `.ufs` | UFS image |
| `.exfat` | exFAT image |
| `.img` | exFAT image (legacy extension alias) |
| Folder with `sce_sys/` | Unpacked / folder install |

**PFSC examples:** `PPSA17599.ffpfsc` (PFS inner), `PPSA26344.exfat.ffpfsc` (exFAT inner).

---

### 📦 Multi-file batch install

Put **multiple** game images in the same folder and run the installer once.

- All supported files in that directory are found automatically.
- Each game is staged (mount + metadata), then registered in **one** batch pass.
- Install order: PFS-inner PFSC games first, exFAT-inner last.
- Games that are **already fully installed** (live mount + library metadata) are **skipped**.

---

### 📂 Storage scan & shortcuts

On launch, Dump Installer **scans common storage paths** for folders that contain installable games. Any location with games shows up in the **Options menu** with a label and count, e.g. `Internal · homebrew (4)`.

Tap a shortcut to install **directly from that folder** — no need to browse again.

**Scanned locations include:**

- `/data/homebrew`
- `/data/etaHEN/games`
- `/mnt/usb0`–`/mnt/usb7` and their `homebrew` / `etaHEN/games` subfolders
- `/mnt/ext0`, `/mnt/ext1` and their `homebrew` / `etaHEN/games` subfolders
- Connected USB and M.2 drives detected under `/mnt/`
- `/mnt/shadowmount` and `/mnt/shadowmnt` (if present)

The main **Select Game Directory** action opens the picker at `/data/homebrew` by default.

---

### 🎮 Using the installer

1. Open **Dump Installer** from Homebrew Launcher.
2. Either:
   - Tap a **storage shortcut** in Options (if games were found), or
   - Use the main action and pick the folder with your dumps.
3. Wait for install to finish. Icons appear on the home screen / game library.
4. Launch games from the library.

**Mount-based installs:** Dumps stay as files (e.g. in `/data/homebrew/`). The installer mounts them at `/system_ex/app/TITLEID` for the session — this is **not** a full copy install like PSN.

- **Same session (no reboot):** games keep working after install.
- **After reboot:** mounts are cleared. Re-jailbreak, then run the installer again on your games folder to remount (already-live games are skipped).
- **Re-JB without reboot:** mounts stay alive; no need to re-run.

**Trophies:** The installer registers trophy metadata (trophy tab on the game tile). In-game trophy pops still depend on **kstuff** at runtime.

---

### ⚙️ Options menu

| Option | Description |
|--------|-------------|
| **Storage shortcuts** | Install from a scanned path (shows game count) |
| **Remove grey icons** | Clears stuck library entries (no delete button in UI); pick the folder with your game dumps |
| **Remove /data/dilocs folder only** | Removes legacy shortcut folder under `/data/dilocs/` (safe; does not delete games) |

---

### 🔧 Troubleshooting

| Problem | What to try |
|---------|-------------|
| **Grey icons** after reinstall | Options → **Remove grey icons**, then run install again on `/data/homebrew` |
| **Game won't launch** | Run installer again to remount; check dump file is still in the folder |
| **Trophy tab missing** | Re-run install (post-register trophy refresh is built in) |

---

### ✨ Additional improvements (this fork)

- **`.ffpfsc` / PFSC** — nested PFS via LVD + nullfs overlay; exFAT-inner mounts directly to `/system_ex/app/TITLEID`
- **Trophies** — trophy metadata copied to system appmeta; refreshed after batch registration
- **Reinstall / grey icons** — library cleanup before fresh install; dedicated grey-icon removal in Options
- **Smarter skip** — only skips titles that are truly live (mount, eboot, links, and appmeta intact)
- **Quiet install** — essential on-screen notifications only (no log files)

---

### 🔧 Building from source

**Repository layout** (same as upstream):

```
dump_installer/
├── include/
├── src/
├── sce_sys/
├── homebrew.js
├── Makefile
└── README.md
```

With [PS5 Payload SDK](https://github.com/ps5-payload-dev/pacbrew-repo) installed:

```bash
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
make
```

---

### Credits

- [EchoStretch/dump_installer](https://github.com/EchoStretch/dump_installer) — original project
- [ps5-payload-dev](https://github.com/ps5-payload-dev) — Payload SDK & Websrv / Homebrew Launcher
