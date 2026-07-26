# Chocola loader

## Run

`dist\Chocola.exe` — canonical release build (copied here by `build-chocola.ps1`).

Dev build output: `ChocolaLoader\bin\Release\Chocola.exe`

## Build

```powershell
.\build-chocola.ps1
```

Stages the Vanille payload, regenerates the app icon from `..\Vanille\assets\logo_icon.png`, builds the WinForms loader, and copies the result to `dist\`.

## Icon

Application/window icon is generated from the Vanille character sprite at `Vanille\assets\logo_icon.png` via `generate-icon.ps1` → `ChocolaLoader\icon.ico`.

Roblox row icon in the UI uses `ChocolaLoader\Resources\roblox.png` (not the app icon).
