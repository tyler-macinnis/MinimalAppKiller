# MinimalAppKiller

Minimal Windows console app that repeatedly terminates configured processes.

## Features

- No wxWidgets dependency
- Small, maintainable C++ codebase
- User-editable process list via config file
- Efficient scan-and-kill loop
- Live config reload while running

## Build

From the repository root:

```powershell
msbuild .\MinimalAppKiller.sln /t:Build /p:Configuration=Debug /p:Platform=x64
```

## Run

```powershell
.\x64\Debug\MinimalAppKiller.exe
```

Run with a custom config file:

```powershell
.\x64\Debug\MinimalAppKiller.exe .\my-config.conf
```

Press `Ctrl+C` to stop.

## Configuration

Default config path: `MinimalAppKiller\appkiller.conf`

Supported entries:

- `interval_ms=500`
- `no_hit_log_every_loops=60`
- One executable name per line (for example: `notepad.exe`)

## Safety

- Administrator rights may be required for some processes.
- Use carefully. Terminating system or security processes can destabilize the machine.
