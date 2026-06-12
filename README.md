# Minimal App Killer

A lightweight, clean Windows desktop app that automatically terminates the applications you choose — set it, minimize it to the tray, and forget it.

![Platform](https://img.shields.io/badge/platform-Windows%2010%2F11-blue)
![Language](https://img.shields.io/badge/language-C%2B%2B20-orange)
![License](https://img.shields.io/badge/dependencies-none-brightgreen)

## Features

- 🖥️ **Simple, clean GUI** — no console window, no clutter
- ⚡ **Lightweight** — a single small native executable with zero dependencies
- ✅ **Per-app toggles** — add apps once, then enable or disable each one with a checkbox
- 💾 **Remembers your settings** — your app list and toggles persist between sessions
- 📌 **Lives in the system tray** — close or minimize the window and it keeps working in the background
- 🚀 **Run on startup** — one checkbox to start automatically (minimized) when Windows starts
- ⏯️ **Pause / Resume** — temporarily stop killing apps without losing your list
- 🛡️ **Built-in safety** — critical Windows system processes can't be added as targets

## Installation

### Installer (recommended)

1. Download the latest `MinimalAppKiller.x.y.z.Setup.exe` from the [Releases](https://github.com/tyler-macinnis/MinimalAppKiller/releases) page.
2. Run the installer and follow the prompts. You can optionally enable a desktop icon and automatic startup during setup.

### Portable

Download `MinimalAppKiller.x.y.z.portable.exe` from [Releases](https://github.com/tyler-macinnis/MinimalAppKiller/releases) and run it directly — no installation needed.

## Usage

1. **Add an app**: type the process name (for example `notepad.exe`) into the text box and click **Add** (or press Enter). If you leave off the extension, `.exe` is added automatically.
2. **Toggle apps**: use the checkbox next to each app to enable or disable it without removing it from your list.
3. **Pause / Resume**: the button in the top-right corner pauses or resumes all killing at once.
4. **Remove apps**: select one or more apps and click **Remove selected** (or press Delete).
5. **Run on startup**: tick **Run on startup** and the app will launch minimized to the tray when you sign in.
6. **Tray**: closing or minimizing the window sends the app to the system tray, where it keeps working. Double-click the tray icon to reopen, or right-click it for quick controls (Active toggle, startup toggle, Exit).

> 💡 Find a process name in Task Manager → Details tab.

## Where settings are stored

Your app list and preferences are saved to:

```
%APPDATA%\MinimalAppKiller\settings.ini
```

## Building from source

Requirements: Visual Studio 2022 or later with the *Desktop development with C++* workload.

```powershell
msbuild .\MinimalAppKiller.sln /t:Build /p:Configuration=Release /p:Platform=x64
```

The executable is produced at `x64\Release\MinimalAppKiller.exe`.

### Building the installer

Requires [Inno Setup 6](https://jrsoftware.org/isinfo.php):

```powershell
ISCC.exe installer\MinimalAppKiller.iss /DAppVersion=1.0.0
```

The installer is produced in the `dist` folder.

## Releasing

Releases are automated with GitHub Actions:

1. Update `APP_VERSION_STRING` in `MinimalAppKiller/version.h` and add a matching entry to `CHANGELOG.md`.
2. Push a tag like `v1.0.0` (or run the **Release** workflow manually with the version number).
3. The workflow builds the app, compiles the installer, and publishes both to a GitHub Release.

## Safety notes

- Administrator rights may be required to terminate some processes.
- Use carefully — terminating apps forcefully discards any unsaved work in them.
- Critical Windows system processes (e.g. `lsass.exe`, `winlogon.exe`, `svchost.exe`) are blocked from being added.

## License

This project is provided as-is, without warranty. Use at your own risk.
