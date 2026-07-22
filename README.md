# Texture Baker for UNIGINE

Plugin that bakes texture sets from a high-poly model (multi-material model) onto a low-poly model’s UV layout — albedo (_alb), shading (_sh: metalness/roughness/specular), normal (_n) and optional emission (_e).

Typical use: baking full texture sets for simplified distant LODs of multi-material models.

![UNIGINE](https://img.shields.io/badge/UNIGINE-2.21-blue) ![License](https://img.shields.io/badge/license-MIT-green)

## Features

- **"As rendered" GPU mode**: the engine itself renders the high-poly materials — layered materials, masks, per-layer tiling, second UV channels and material graphs all transfer correctly. Unwraps are chart-repacked into a unique atlas with mesh-area texel density equalization, so overlapping/tiled/mirrored UVs are handled.
- Multi-mesh high-poly and low-poly (parts share one UV layout and one texture set)
- **Bake groups** (manual or auto-matched by name): rays of each low-poly part see only its own high-poly part
- **Paintable skew mask** (Marmoset-style paint skew): one click creates the mask, assigns it and switches the editor into Texture Paint Mode
- Frontal/rear cage distances, 1–64 samples per texel, edge dilation
- Baked textures are saved next to the low-poly asset; the material is created or updated automatically
- Drag & drop from World Nodes; per-slot viewport Hide buttons that never affect baking
- UI in English and Russian (follows the editor language)

## Requirements

- UNIGINE SDK **2.21** (float or double, x64), engine + editor headers and libraries
- Qt **6.5.3** (the version the 2.21 editor is built with): `msvc2019_64` on Windows, `gcc_64` on Linux
- Windows: MSVC (Visual Studio 2022+), CMake 3.19+, Ninja
- Linux: gcc 11+, CMake 3.19+, Ninja

## Building

The plugin is built inside a UNIGINE project (it needs the SDK's `include/` and `lib/`). Clone into your project:

```
<project>/source/plugins/zloy_pingvin/Baker/   <- this repository
```

Windows (from a VS x64 developer prompt):

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Qt/6.5.3/msvc2019_64
cmake --build build
```

Linux:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/opt/qt/6.5.3/gcc_64
cmake --build build
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
- Pick the output resolution; adjust the frontal/rear distances in Settings if needed.
- Press Bake. Textures are saved next to the low-poly mesh asset; the material is created/updated automatically.
- If details bake with a sideways slide, press Paint skew mask, paint the problem areas white, save, enable "use skew mask" and re-bake. 
If a neighboring part imprints onto another one, use Groups (manual pairs or by-name matching).

## Notes

- The low-poly UV0 must be a unique layout inside the 0..1 tile (the standard +1-offset overlap workflow is supported).
- This repository contains only the plugin source — no UNIGINE SDK files are included or required to browse it; the SDK is needed only to build.

## Debug tab

- As rendered (GPU) - the main bake mode: the engine renders the high-poly materials into the capture (layers, masks, tiling, colors). 
Disable to sample only the base material textures on the CPU (faster, but layered materials lose their layers).
- Capture unwrap (auto / UV0 / UV1) - which UV channel the GPU capture unwraps the high-poly into. Auto picks the channel with less chart overlap per surface (the choice is logged to the console). 
Change only if a second-UV-driven layer bakes wrong.
- Invert G (Y) - extra inversion of the normal map green channel. Normally not needed; enable only if the baked relief looks inverted.
- Colorize hit zones - bakes a diagnostic colorization instead of albedo: green - the ray hit a surface above the low-poly, blue - below, red - a back face, yellow - the skew mask area, 
black/stretched - a miss. Also dumps the GPU capture atlases to the system temp folder (baker_captures) for inspection.
- Rays along shading normals - cast rays along the shading normals instead of the position-smoothed cage normals. 
May remove projection skew on faceted surfaces, but produces gaps at hard edges.

## License

The plugin source code is licensed under the [MIT License](LICENSE). The UNIGINE SDK is proprietary software available at [unigine.com](https://unigine.com).

## Contact

Telegram: https://t.me/zloy_pingvin
