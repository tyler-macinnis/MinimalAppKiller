# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.0] - 2026-06-12

### Added

- Complete rewrite as a lightweight native Windows GUI application (no console window).
- Clean, simple interface for managing the list of apps to kill.
- Per-app enable/disable checkboxes — toggle apps without removing them.
- Settings persisted to `%APPDATA%\MinimalAppKiller\settings.ini`.
- System tray support: closing or minimizing keeps the app running in the background.
- Tray context menu with Open, Active toggle, Run on startup, and Exit.
- "Run on startup" option (starts minimized to the tray).
- Pause/Resume button to temporarily stop killing apps.
- Kill counter and live status display.
- Safety blocklist preventing critical Windows system processes from being added.
- Inno Setup installer with optional desktop icon and startup tasks.
- GitHub Actions CI workflow (build + installer artifacts).
- GitHub Actions Release workflow publishing the installer and portable executable.

### Removed

- Terminal/console interface and `appkiller.conf` configuration file.
