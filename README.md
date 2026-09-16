# Texture Baker for UNIGINE

Plugin that bakes texture sets from a high-poly model (multi-material model) onto a low-poly model’s UV layout — albedo (_alb), shading (_sh: metalness/roughness/specular), normal (_n) and optional emission (_e).

Typical use: baking full texture sets for simplified distant LODs of multi-material models.

![UNIGINE](https://img.shields.io/badge/UNIGINE-2.22-blue) ![License](https://img.shields.io/badge/license-MIT-green)

## Features

- **"As rendered" GPU mode**: the engine itself renders the high-poly materials — layered materials, masks, per-layer tiling, second UV channels and material graphs all transfer correctly. Unwraps are chart-repacked into a unique atlas with mesh-area texel density equalization, so overlapping/tiled/mirrored UVs are handled.
- Multi-mesh high-poly and low-poly (parts share one UV layout and one texture set)
- **Bake groups** (manual or auto-matched by name): rays of each low-poly part see only its own high-poly part
- **Paintable skew mask** (Marmoset-style paint skew): one click creates the mask, assigns it and switches the editor into Texture Paint Mode
- **World decals** (Ortho/Proj/Mesh) are projected into the bake, including decals nested in Node References; disabled decals bake too
- **Per-part cage**: every low-poly part can carry its own frontal/rear distance, edited in the front/back columns of the participants list. **Fit cage** probes each part with rays and picks the smallest distances that still catch its high-poly; the global pair is the ceiling, so one distant part no longer forces a big cage on the whole model
- Frontal/rear cage distances, 4–64 samples per texel, edge dilation
- **Emission** (`_e`): RGB is the glow colour, alpha is a glow mask (opaque where the material's Emission state is on) for compositing elsewhere
- **Selectable maps**: `_alb` / `_sh` / `_n` / `_e` each have their own checkbox. An unchecked map is not written and the material keeps its current texture, so one map can be re-baked on its own without touching the ones already accepted
- Captures run in **chunks sized to free video memory**, so peak VRAM does not grow with the surface count and large models no longer risk a GPU device reset
- Baked textures are saved next to the low-poly asset; the material is created or updated automatically
- Drag & drop from World Nodes; per-slot viewport Hide buttons that never affect baking
- UI in English and Russian (follows the editor language)

## Requirements

- UNIGINE SDK **2.22** (float or double, x64), engine + editor headers and libraries
- Qt **6.5.3** (the version the 2.22 editor is built with): `msvc2019_64` on Windows, `gcc_64` on Linux
- Windows: MSVC (Visual Studio 2022+), CMake 3.19+, Ninja
- Linux: gcc 11+, CMake 3.19+, Ninja

## Building

The plugin is built inside a UNIGINE project (it needs the SDK's `include/` and `lib/`). Clone into your project:

```
<project>/source/plugins/zloy_pingvin/Baker/   <- this repository
```

Build **out of tree** — keep the build directory outside this folder, so packaging the
plugin (e.g. a UNIGINE Store `.upackage` export) cannot pull build artifacts in.

Windows (from a VS x64 developer prompt):

```bat
cmake -S . -B %USERPROFILE%/baker_build/win_x64 -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Qt/6.5.3/msvc2019_64
cmake --build %USERPROFILE%/baker_build/win_x64
```

Linux:

```sh
cmake -S . -B ~/baker_build/x64 -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/opt/qt/6.5.3/gcc_64
cmake --build ~/baker_build/x64
```

Options:

- `-DUNIGINE_DOUBLE=1` — link against the double-precision engine/editor libraries
- `-DCMAKE_BUILD_TYPE=RelWithDebInfo` — produces the `d`-suffixed binary linked against the `d`-suffixed SDK libraries (UNIGINE Store "debug" convention: optimized + debug info)
- `-DUNIGINE_SDK_PATH=<path>` — override the project/SDK root if building out of tree

The binary is written to `<project>/bin/plugins/zloy_pingvin/Baker/` and the editor loads it automatically at startup. `build_linux.sh` builds all four Linux variants from WSL.

## Installation (prebuilt)

Copy `bin/plugins/zloy_pingvin/Baker/` into your project's `bin` folder (keep the structure) and start the editor. The tool window is in the editor menu (Texture Baker).

## Quick start

- Open the tool: menu Windows -> Texture Baker
- Select the high-poly node in the scene and press Select (or drag nodes from World Nodes onto the High-poly section). Same for the low-poly.
- Pick the output resolution; set the global cage (front/back). It is both the default for every part and the ceiling for Fit cage.
- Optional: press Fit cage to tighten the cage per part, then adjust single values in the front/back columns of the low-poly rows.
A dimmed value follows the global one; an empty field resets that axis back to it.
- Press Bake. Textures are saved next to the low-poly mesh asset; the material is created/updated automatically.
- If details bake with a sideways slide, press Paint skew mask, paint the problem areas white, save, enable "use skew mask" and re-bake. 
If a neighboring part imprints onto another one, use Groups (manual pairs or by-name matching).

## Notes

- The low-poly UV0 must be a unique layout inside the 0..1 tile (the standard +1-offset overlap workflow is supported).
- This repository contains only the plugin source — no UNIGINE SDK files are included or required to browse it; the SDK is needed only to build.

## Debug tab

- As rendered (GPU) - the main bake mode: the engine renders the high-poly materials into the capture (layers, masks, tiling, colors). 
Disable to sample only the base material textures on the CPU (faster, but layered materials lose their layers).
- Capture unwrap (auto / UV0 / UV1) - which UV channel the GPU capture unwraps the high-poly into. Auto prefers the channel where more triangles actually receive atlas area 
(a tiling UV set often collapses part of the mesh to zero area, which renders nothing); ties go to less chart overlap, then fewer charts. The choice is logged to the console. 
Change only if a second-UV-driven layer bakes wrong.
- Capture size (auto / 512 / 1024 / 2048) - the GPU capture size per high-poly surface. It caps the detail of the "as rendered" mode regardless of the bake resolution, 
so baking at 4096 gains little if the capture is smaller. Auto budgets the size against free system RAM, which has to hold the whole capture set at once. Video memory is 
budgeted separately: captures are issued in chunks that fit free VRAM, so peak video memory does not grow with the surface count and a small GPU costs extra passes rather 
than quality. A manual size ignores both budgets, and if memory runs short the captures come back black and those surfaces bake from the material textures instead. 
The resolved size, the chunk size and the estimated usage are shown at the bottom of the window.
- Invert G (Y) - extra inversion of the normal map green channel. Normally not needed; enable only if the baked relief looks inverted.
- Colorize hit zones - bakes a diagnostic colorization instead of albedo: green - the ray hit a surface above the low-poly, blue - below, red - a back face, yellow - the skew mask area, 
black/stretched - a miss. Also dumps the GPU capture atlases to the system temp folder (baker_captures) for inspection.
- Rays along shading normals - cast rays along the shading normals instead of the position-smoothed cage normals. 
May remove projection skew on faceted surfaces, but produces gaps at hard edges.

## License

The plugin source code is licensed under the [MIT License](LICENSE). The UNIGINE SDK is proprietary software available at [unigine.com](https://unigine.com).

## Contact

Telegram: https://t.me/zloy_pingvin
