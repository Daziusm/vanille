# Offsets — file paths (no values)

Vanille does **not** commit live Roblox offset hex values. They change every client update. This page lists **where** offsets live and **how** to get them.

## Runtime file (what Vanille reads)

| Path | Used when |
|------|-----------|
| `<folder containing vanille.exe>\values.txt` | Running `vanille.exe` directly |
| `%LOCALAPPDATA%\Chocola\values.txt` | Running via the loader (default install dir) |

The client searches next to the exe first (`Vanille/extern/auth/auth.cpp` → `find_values_file()`).

**Format:** PHP-style map, one entry per line:

```php
'namespace::field' => '0x...',
```

See the shape-only template: **`Vanille/values.txt.example`** (placeholders, not for production).

## Loader — auto-fetch (recommended)

| File | Role |
|------|------|
| `loader/ChocolaLoader/OffsetUpdater.cs` | Detects Roblox version, downloads offsets, writes `values.txt` into the install dir |
| `%APPDATA%\Chocola\loader.ini` | Optional overrides: `offsets_url`, `offsets_path`, `dumper_path` |

Default remote source pattern (version = Roblox client version string):

```
https://offsets.imtheo.lol/<version>/offsets.txt
https://offsets.imtheo.lol/<version>/offsets.json
```

The loader converts imtheo format → `values.txt` keys Vanille expects.

## Build-time (packaging payload into loader)

| Path | Role |
|------|------|
| `Vanille/values.txt` | Staged into `payload.zip` by `loader/cmake/stage_payload.cmake` when you run `loader/build-chocola.ps1` |
| `Vanille/values.txt.example` | Fallback if `values.txt` is missing at build time (placeholder only) |

## Code — offset names (not values)

| Path | Role |
|------|------|
| `Vanille/source/sdk/offsets.h` | Registers all offset **names** (`namespace::field`); runtime values start at `0` |
| `Vanille/extern/auth/auth.cpp` | `LoadOffsets()` — parses `values.txt` and fills `roblox::offsets::*` |

## Optional local override paths (loader)

Checked by `OffsetUpdater.cs` when imtheo fails:

| Path | Format |
|------|--------|
| `offsets_path` in `loader.ini` | Custom file |
| `<install dir>\offsets.txt` | imtheo-style or mapped text |
| `<install dir>\offsets.json` | JSON (converted via dumper script if present) |

## If offsets are wrong / stale

1. Delete `%LOCALAPPDATA%\Chocola\values.txt`
2. Run the loader again — it refetches for the current Roblox version
3. Or drop a fresh `values.txt` next to `vanille.exe` manually

## Not in this repo

- `Vanille/values.txt` — gitignored (user/version-specific)
- `Vanille/tools/generate_values.py` — local dumper pipeline only
- `roblox-dumper/` — local tooling, not published
