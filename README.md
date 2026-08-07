# Vanille

Roblox external overlay (C++) with WinForms loader.

## Downloads

| What | Where |
|------|--------|
| **Prebuilt loader** (recommended) | [GitHub Releases](https://github.com/Daziusm/vanille/releases/latest) — download `Vanille.exe` |
| **Full source code** | This repo — [browse](https://github.com/Daziusm/vanille) or [ZIP (master)](https://github.com/Daziusm/vanille/archive/refs/heads/master.zip) |
| **Source for a release tag** | `https://github.com/Daziusm/vanille/archive/refs/tags/vX.Y.Z.zip` |

You do **not** need to compile if you use the release loader. It embeds `vanille.exe`, installs to `%LOCALAPPDATA%\Chocola\`, and fetches offsets automatically.

## Quick start (no compile)

1. Download **Vanille.exe** from [Releases](https://github.com/Daziusm/vanille/releases/latest).
2. Run it (installs payload to `%LOCALAPPDATA%\Chocola\`).
3. Launch Roblox, open the loader, inject / run Vanille.
4. Offsets are refreshed on launch (see [docs/OFFSETS.md](docs/OFFSETS.md)).

## Build from source (optional)

Requires Visual Studio 2022 (C++ + .NET), CMake 3.20+, Windows x64.

```powershell
# 1. Place a valid values.txt next to Vanille.exe (see docs/OFFSETS.md)
# 2. Build client + loader
.\loader\build-chocola.ps1
# Output: loader\dist\Vanille.exe
```

## Repo layout

```
Vanille/          C++ overlay client (vanille.sln)
loader/           Chocola WinForms loader + build scripts
docs/             User docs (offsets paths, etc.)
mcp-server/       Optional MCP bridge for Cursor (dev)
```

## Offsets

Offset **file paths** and how to obtain values (without shipping hex in the repo): **[docs/OFFSETS.md](docs/OFFSETS.md)**

## License

See [LICENSE](LICENSE) if present; otherwise all rights reserved by repository owner.
