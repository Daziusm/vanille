# Brand logo

The menu titlebar logo (left of **vanille**) is an embedded PNG in `extern/resources/fonts/logo.c`.

The watermark is text-only — no logo there.

## Replace with your image (easy)

1. Save your logo as a **PNG** (square works best, e.g. 64–256 px; transparent background OK).

2. From the `vanille` folder, run:

```powershell
python scripts/png_to_logo_c.py "C:\path\to\your\logo.png"
```

That overwrites `extern/resources/fonts/logo.c` with the correct `vanille_png` / `vanille_png_len` symbols.

3. **Rebuild** in Visual Studio (Release | x64) or:

```powershell
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe" vanille.sln /t:Rebuild /p:Configuration=Release /p:Platform=x64
```

4. Run `build\vanille.exe` and open the menu — your logo should appear in the titlebar.

## Manual / online alternative

If you prefer not to use the script:

1. Use any “PNG to C array” tool (or `xxd -i logo.png` on Linux/Mac).
2. Replace the contents of `extern/resources/fonts/logo.c` so it contains exactly:
   - `const unsigned char vanille_png[] = { ... };`
   - `const unsigned int vanille_png_len = <byte count>;`
3. Rebuild.

## Where it’s used in code

| File | Role |
|------|------|
| `extern/resources/fonts/logo.c` | Embedded PNG bytes |
| `source/gui/overlay.cpp` → `LoadLogoTexture()` | Loads into `c_textures::logo` |
| `source/gui/overlay.cpp` (~menu titlebar) | `ImGui::Image(c_textures::logo, ...)` |
