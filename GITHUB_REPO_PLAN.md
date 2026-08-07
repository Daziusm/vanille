# Vanille — GitHub Repository Plan

Prepared for `D:\Vanille` → public repo at [github.com/Daziusm/vanille](https://github.com/Daziusm/vanille).

> **Current state:** `D:\Vanille` has no root git repo. `Vanille/` and `roblox-dumper/` are nested repos with their own remotes. An older snapshot (`fragment.*`) already exists on GitHub with a minimal tree. This plan targets a polished **slim repo** — overlay client + loader only — that replaces that snapshot.

---

## 1. Scope summary

| Verdict | What |
|---------|------|
| **Include** | `Vanille/` — C++ overlay client source |
| **Include** | `loader/ChocolaLoader/` + loader build scripts (CMake, `build-chocola.ps1`) |
| **Include** | Root `docs/images/` (screenshot), `README.md`, `LICENSE`, `.gitignore` |
| **Exclude** | Everything else at `D:\Vanille` (see §2) |

**Not in scope for the public repo:** offset dumper, dumper tooling, helper experiments, local test bundles, third-party loader forks, or any documentation that points users at reverse-engineering tooling.

---

## 2. Directory audit

```
D:\Vanille\
├── Vanille/              # Main overlay client (C++, VS solution) — INCLUDE
├── loader/               # Chocola WinForms + CMake loader — INCLUDE (source + scripts only)
├── docs/images/          # Repo marketing assets — INCLUDE at root
├── roblox-dumper/        # EXCLUDE — local only, never commit
├── chocola-helper/       # EXCLUDE — separate experiment
├── test-dist/            # EXCLUDE — local runtime bundle
├── loader-alone-test/    # EXCLUDE — standalone loader test binary
└── GITHUB_REPO_PLAN.md   # INCLUDE (optional in public repo)
```

### `Vanille/` (client)

| Path | Verdict | Rationale |
|------|---------|-----------|
| `source/` | **Include** | Core application code |
| `extern/` (imgui, freetype, clipper2, text_editor, tinygltf, simplemaths) | **Include** | Vendored deps; standard practice |
| `extern/auth/` | **Include** | Misnamed — only `LoadOffsets()` reading `values.txt`; no secrets |
| `extern/vmSoft/` | **Exclude** | VMProtect commercial SDK — license + redistribution |
| `extern/intermediates/` | **Exclude** | Build artifacts |
| `third_party/lua/` | **Include source** | Lua build inputs |
| `third_party/lua/lua53-64.dll` | **Exclude** | Prebuilt runtime DLL — build locally |
| `assets/` | **Include** | Brand PNGs (logo, splash, icons) |
| `fonts/` | **Include** | Plus Jakarta Sans (OFL); `fonts/README.md` documents license |
| `docs/` | **Include** | `LUA_VM.md`, `BRAND_LOGO.md` |
| `scripts/png_to_logo_c.py` | **Include** | Asset pipeline |
| `scripts/*.lua` (examples) | **Include** | Ship as `examples/lua/` or document in README |
| `tools/generate_values.py` | **Exclude** | Offset/dumper pipeline — not part of public repo |
| `build/` | **Exclude** | `vanille.exe`, `.pdb`, `lua53-64.dll`, `VMProtectSDK64.dll`, configs |
| `values.txt` | **Exclude** | User-supplied; version-specific; never commit |
| `uitest/` | **Exclude** | Internal UI theme experiments |
| `.think/` | **Exclude** | Private notes |
| `vanille.sln`, `*.vcxproj*` | **Include** | Build system |
| `*.vcxproj.user` | **Exclude** | Local VS settings |
| `source/features/tests.cpp` | **Exclude** | Dev test harness |

### `loader/`

| Path | Verdict | Rationale |
|------|---------|-----------|
| `ChocolaLoader/` (C# source) | **Include** | Active loader UI + injection logic |
| `src/` (C++ alt loader) | **Include** | CMake-native loader path |
| `cmake/`, `CMakeLists.txt`, `build-chocola.ps1`, `generate-icon.ps1` | **Include** | Build tooling |
| `res/` | **Include** | Resources |
| `extern/miniz/` | **Include** | Vendored zip lib |
| `ChocolaLoader/payload.zip` | **Exclude** | Embedded Vanille binary |
| `ChocolaLoader/bin/`, `obj/` | **Exclude** | MSBuild output |
| `dist/`, `build/` | **Exclude** | Release binaries |
| `gamesense-Loader/` | **Exclude** | Third-party fork with its own `.git` |
| `LoaderSettings.cs` default repo URL | **Include** | Points to `Daziusm/vanille` — correct |

### Explicitly excluded (entire folders — do not commit)

| Path | Rationale |
|------|-----------|
| `roblox-dumper/` | Offset dumper — local dev only; keep on disk, exclude from git |
| `chocola-helper/` | Parallel experiment; not part of Vanille release |
| `test-dist/` | Full runtime bundle with binaries |
| `loader-alone-test/` | Standalone loader test binary |
| `loader/gamesense-Loader/` | Third-party fork |
| `Vanille/uitest/` | Internal UI experiments |
| `Vanille/.think/` | Private notes |

---

## 3. `values.txt`

| Include | Exclude |
|---------|---------|
| — | Tied to one Roblox client version; stale quickly |
| — | Users must obtain offsets independently |

**Decision:** **Do not commit** `values.txt`. Document in README that users must place a valid `values.txt` next to `vanille.exe` before running. No dumper instructions, no offset-generation scripts, no links to reverse-engineering tooling in the public repo.

---

## 4. `lua53-64.dll`

Lua is [MIT licensed](https://www.lua.org/license.html). Prebuilt DLLs are convenient but bloat the repo and may be AV-flagged.

**Recommendation:** Exclude prebuilt DLL. Document building Lua 5.3 from `third_party/lua` or copying the DLL into `build/` post-compile.

---

## 5. Recommended repo layout

```
vanille/                          # repo root (github.com/Daziusm/vanille)
├── .github/
│   └── workflows/                # optional: build.yml (VS + CMake matrix)
├── docs/
│   ├── images/
│   │   └── screenshot.png
│   ├── LUA_VM.md                 # copy or symlink from Vanille/docs/
│   └── BRAND_LOGO.md
├── examples/
│   └── lua/                      # example_lua_tab, ui_example, lua_example
├── Vanille/                      # C++ overlay (keep actual folder name)
│   ├── source/
│   ├── extern/                   # minus vmSoft/
│   ├── assets/
│   ├── fonts/
│   ├── third_party/lua/
│   ├── scripts/
│   ├── vanille.sln
│   └── vanille.vcxproj
├── loader/
│   ├── ChocolaLoader/
│   ├── src/
│   ├── cmake/
│   └── build-chocola.ps1
├── .gitignore
├── README.md
├── LICENSE
└── GITHUB_REPO_PLAN.md           # optional in public repo
```

**Note:** Folder names on disk are `Vanille/` (PascalCase) and `loader/ChocolaLoader/`. The loader's `build-chocola.ps1` already references `Vanille/` — no rename required.

### Loader ↔ repo coupling

The loader downloads sources from this same repo (`LoaderSettings.cs` → `https://github.com/Daziusm/vanille`, branch `master`). `SourceDownloader` uses the public GitHub zipball API. Users who clone manually build `Vanille/` first, then run `loader/build-chocola.ps1` to stage the payload and produce `Vanille.exe` (loader binary name).

---

## 6. Sensitive pattern scan

| Pattern | Found? | Action |
|---------|--------|--------|
| API keys / tokens | **No** | — |
| GitHub tokens | **No** | Public zipball API only |
| Embedded `vanille.exe` | **Yes — exclude** | `loader/ChocolaLoader/payload.zip`, `loader/dist/` |
| VMProtect SDK | **Yes — exclude** | `extern/vmSoft/`, `build/VMProtectSDK64.dll` |
| Dumper / offset tooling in docs | **Remove** | No `roblox-dumper/`, no `generate_values.py` in README or plan marketing |
| `values.txt` | **Exclude** | User-supplied at build/runtime |

---

## 7. Pre-publish checklist

- [ ] `git init` at `D:\Vanille` (or export to clean folder without nested `.git/`)
- [ ] Remove nested `.git` from `Vanille/`, `roblox-dumper/`, `loader/gamesense-Loader/` when flattening
- [ ] Verify `.gitignore` excludes all folders in §2
- [ ] Delete / gitignore all binaries listed above
- [ ] Add `LICENSE` file
- [ ] Add GitHub topics: `roblox`, `imgui`, `overlay`, `lua`, `c++`, `windows`
- [ ] Replace empty GitHub repo contents (current `fragment.*` tree) with slim repo
- [ ] Optional: GitHub Actions for MSVC build (no artifact upload of exe if policy concern)
- [ ] Review screenshot for anything you want blurred
- [ ] Confirm README has no dumper/offset tooling references

---

## 8. First commit contents (suggested)

```
docs/images/screenshot.png
docs/LUA_VM.md
docs/BRAND_LOGO.md
examples/lua/*.lua
Vanille/            # full source tree per §2, minus excluded paths
loader/             # source + scripts only, no payload.zip/dist/build
.gitignore
README.md
LICENSE
GITHUB_REPO_PLAN.md
```

### Do not commit

```
roblox-dumper/              # entire folder — local only
chocola-helper/
test-dist/
loader-alone-test/
loader/gamesense-Loader/
Vanille/uitest/
Vanille/.think/
Vanille/tools/generate_values.py
build/, dist/, bin/, obj/
*.exe, *.dll, *.pdb
payload.zip
values.txt
extern/vmSoft/
source/features/tests.cpp
```
