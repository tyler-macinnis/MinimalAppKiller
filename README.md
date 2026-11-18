<div align="center">

# Minimal App Killer

**Pick the apps you never want running. It closes them the moment they start.**

A tiny native Windows tray app. One executable, no dependencies.

![Platform](https://img.shields.io/badge/Windows-10%20%7C%2011-0078D4)
![License](https://img.shields.io/badge/license-MIT-green)

[**Download the latest release**](https://github.com/tyler-macinnis/MinimalAppKiller/releases/latest)

</div>

## Install

Grab one of these from [Releases](https://github.com/tyler-macinnis/MinimalAppKiller/releases/latest):

- **`MinimalAppKiller-x.y.z-setup.exe`** installs it, with an optional desktop icon and run on startup.
- **`MinimalAppKiller.exe`** is portable and runs as is, no install.

## Use

| To | Do this |
| --- | --- |
| Add an app | Type its process name (`notepad` or `notepad.exe`) and press Enter |
| Turn one app on or off | Tick or untick its checkbox |
| Remove apps | Select them and press Delete |
| Stop everything for a while | **Pause**, then **Resume** |
| Start with Windows | Tick **Run on startup** |
| Quit | Right-click the tray icon, then **Exit** |

Closing or minimizing the window keeps it running in the tray. Double-click the tray icon to bring it back.

Process names are in Task Manager under **Details**. Settings live in `%APPDATA%\MinimalAppKiller\settings.ini`.

> [!WARNING]
> Apps are closed forcefully, so unsaved work in them is lost. Some processes need the app to run as administrator. Critical Windows processes cannot be added.

## Build

Needs Visual Studio with the *Desktop development with C++* workload.

```powershell
msbuild MinimalAppKiller.sln /p:Configuration=Release /p:Platform=x64
```

In VS Code, press `Ctrl+Shift+B`. The result is `x64\Release\MinimalAppKiller.exe`.

To release, bump `MinimalAppKiller/version.h`, add a `CHANGELOG.md` entry, and push a `v1.2.3` tag. GitHub Actions builds and publishes both files.

## License

[MIT](LICENSE)
