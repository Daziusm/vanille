# Chocola loader

## Run

`dist\Vanille.exe` — release build (from [GitHub Releases](https://github.com/Daziusm/vanille/releases/latest) or `build-chocola.ps1`).

Install dir: `%LOCALAPPDATA%\Chocola\` (contains `vanille.exe` + `values.txt`).

## Source code

Full repo (no compile needed for end users): https://github.com/Daziusm/vanille

Offset file paths: [docs/OFFSETS.md](../docs/OFFSETS.md)

## Build

```powershell
.\build-chocola.ps1
```

Stages the Vanille payload, regenerates the app icon from `..\Vanille\assets\logo_icon.png`, builds the WinForms loader, and copies the result to `dist\`.

## Icon

Application/window icon is generated from the Vanille character sprite at `Vanille\assets\logo_icon.png` via `generate-icon.ps1` → `ChocolaLoader\icon.ico`.

Roblox row icon in the UI uses `ChocolaLoader\Resources\roblox.png` (not the app icon).
