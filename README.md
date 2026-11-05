# SOUND VOLTEX — Main Screen Resolution & Refresh Rate Hook (`res_hook.dll`)

A dynamic Direct3D 9Ex resolution and refresh rate hook for **SOUND VOLTEX EXCEED GEAR (SDVX 6)** and **SOUND VOLTEX ∇ NABLA (SDVX 7)** that upgrades the primary game screen (**Main Screen**, natively `1080x1920` portrait) to a custom internal/output resolution (such as `1440x2560` 2K or `2160x3840` 4K) and custom refresh rate / Note FPS target (`60Hz`, `120Hz`, `144Hz`, `165Hz`, `240Hz`, `360Hz`, etc.), while keeping the touch/sub monitor (**Sub Screen**, `1920x1080` landscape) at its native resolution.

---

## Repository Structure

```text
├── .github/workflows/
│   └── build.yml          # GitHub Actions CI build workflow
├── config/
│   └── res_hook.ini       # Default configuration template
├── dist/
│   ├── res_hook.dll       # Prebuilt 64-bit hook binary
│   └── res_hook.ini       # Ready-to-copy configuration file
├── src/
│   └── res_hook.cpp       # C++17 source code for res_hook.dll
├── .gitignore             # Excludes local game files (contents/) and build intermediates
├── build.bat              # One-click Windows build script (MSVC / MinGW-w64)
├── CMakeLists.txt         # CMake build configuration
├── LICENSE                # MIT License
└── README.md              # Project documentation
```

---

## Features

- **True Internal 3D & 2D Upscaling**: Scales the Main Screen backbuffer, render targets (`CreateRenderTarget` / `CreateRenderTargetEx`), depth-stencil surfaces (`CreateDepthStencilSurface` / `CreateDepthStencilSurfaceEx`), and render-target textures (`CreateTexture` with `D3DUSAGE_RENDERTARGET`).
- **Sub Screen Preservation**: Leaves the `1920x1080` Sub Screen swapchain, render targets, viewports, and touch coordinates untouched.
- **2D / AFP Engine & UI Scaling**: Automatically scales viewports, scissor rectangles, and pre-transformed `D3DFVF_XYZRHW` vertices in `DrawPrimitiveUP` (preserving Direct3D 9's `-0.5f` pixel-center offset), while spoofing `GetDesc` / `GetLevelDesc` queries from `soundvoltex.dll` and `afp-core.dll` so game layout logic continues operating in virtual `1080x1920` coordinates.
- **Custom Refresh Rate & Note/Game FPS Target**:
  - **Display Refresh Rate & Software FrameLimiter (`RefreshRate`)**: Overrides the `InitD3D` priority display mode, `CreateDeviceEx` / `ResetEx` fullscreen refresh rate, `NvDisplayConfig` refresh rate, and internal software `FrameLimiter` (`RefreshRate * 1000` mHz).
  - **Refresh Rate Verification & VSync Bypass**: Prevents `InitD3D` from failing with `"Monitor refresh rate does not match!"` when running at non-standard refresh rates (`60Hz` in Valkyrie mode, `144Hz`, `165Hz`, `240Hz`, `360Hz`, etc.), preserving VSync and ensuring `GetMainDisplayRefreshRate()` is always invoked.
  - **Note & Game FPS Target (`NoteFpsTarget`)**: Dynamically patches `GetMainDisplayRefreshRate()` (both the NVIDIA NVAPI path and the `DUMMY` fallback path used on AMD/Intel GPUs or windowed mode) to return any custom FPS without modifying the shared `60.0` seconds-per-minute timer constant in `.rdata`.
  - **Valkyrie Mode 60Hz & Disable Monitor Change**: Supports running Valkyrie Mode on a 60Hz monitor (`RefreshRate=60`) and optionally skipping `NvDisplayConfig` startup display changes (`DisableMonitorChange=1`).
- **Fullscreen & Valkyrie Cabinet Support (`NvDisplayConfig`)**:
  - Patches `soundvoltex.dll`'s internal `NvDisplayConfig` NVAPI display mode table so fullscreen startup on NVIDIA GPUs configures the DisplayPort Main Display to your target resolution and refresh rate (e.g. `2560x1440` rotated `90°`/`270°` -> `1440x2560`) instead of forcing `1080x1920`.
  - Automatically maps target-resolution adapter modes during `InitD3D` refresh-rate detection and falls back to the closest supported refresh rate if the initial fullscreen mode switch fails.
  - Adjusts the fullscreen Sub Screen window's X offset (`1080` -> `Width`) so it aligns with Display 2 on multi-monitor setups.
- **Windowed Mode Support**: Resizes the `"SOUND VOLTEX EXCEED GEAR - Main Screen"` window and disables the per-frame windowed `BackBuffer0` `1080x1920` downscale/re-stretch pass.
- **Version-Independent (SDVX 6 & SDVX 7+)**: Uses dynamic PE import directory parsing and `.text`/`.rdata` byte-pattern scanning—no hardcoded RVAs.

---

## Installation

Copy `dist/res_hook.dll` and `dist/res_hook.ini` into your game `contents` folder (alongside `spice64.exe`). You can load the hook using either of the following methods:

### Method 1: SpiceTools `-k` Parameter (Recommended)
Add `-k res_hook.dll` to your `spice64.exe` launch arguments or batch script:
```bat
spice64.exe -k res_hook.dll
```

### Method 2: Automatic `version.dll` Proxy
If you do not want to modify launch arguments, rename `res_hook.dll` to `version.dll` in the same folder as `spice64.exe` (keep `res_hook.ini` named `res_hook.ini`). `res_hook.dll` exports full proxy forwarders for the Windows `version.dll` API and will load automatically when `spice64.exe` starts.

---

## Configuration (`res_hook.ini`)

`res_hook.dll` reads `res_hook.ini` from the same folder as the DLL on startup:

```ini
[Resolution]
; Enable or disable the resolution & refresh rate hook (1 = enabled, 0 = disabled)
Enabled=1

; Target internal rendering resolution for the Main Screen.
; Note: SOUND VOLTEX renders its Main Screen in portrait orientation (1080x1920 natively).
; If AutoPortrait=1, specifying 2560x1440 or 1440x2560 will both automatically orient to 1440x2560.
; Common presets:
;   1080p (Native): Width=1080, Height=1920
;   1440p (2K)    : Width=1440, Height=2560 (or Width=2560, Height=1440)
;   2160p (4K)    : Width=2160, Height=3840 (or Width=3840, Height=2160)
Width=1440
Height=2560

; Automatically orient Width x Height to portrait (Width < Height) to match the Main Screen (1 = yes, 0 = no)
AutoPortrait=1

; Resize the Main Screen window in windowed mode to match the target resolution (1 = yes, 0 = no)
ResizeMainWindow=1

; Optional custom window client size in windowed mode (0 = use target rendering resolution)
WindowWidth=0
WindowHeight=0

; Patch NvDisplayConfig table so fullscreen mode on NVIDIA GPUs sets MainDisplay to target resolution instead of 1080p (1 = yes, 0 = no)
PatchNvDisplayConfig=1

; Target display refresh rate & software frame limiter in Hz (0 = game default, or e.g. 60, 120, 144, 165, 240, 360)
; Setting 60 also enables Valkyrie Mode 60Hz support; setting >0 updates the D3D9 display mode, VSync check, and FrameLimiter.
RefreshRate=0

; Target Note / Game logic FPS in GetMainDisplayRefreshRate (0 = automatically match RefreshRate, or explicit e.g. 120, 144, 165, 240)
; Controls note scroll smoothness and timing target on both NVIDIA and non-NVIDIA (AMD/Intel) GPUs.
NoteFpsTarget=0

; Skip startup NvDisplayConfig monitor resolution/rotation switching (1 = disable monitor change, 0 = allow)
DisableMonitorChange=0

; Write diagnostic logs to res_hook.log in the same folder as res_hook.dll (1 = yes, 0 = no)
Log=1
```

### Option Reference

| Key | Default | Description |
| :--- | :--- | :--- |
| `Enabled` | `1` | Master toggle (`1` = active, `0` = completely bypassed). |
| `Width` | `1440` | Target Main Screen rendering width (`1440` for 1440p portrait, `2160` for 4K portrait). |
| `Height` | `2560` | Target Main Screen rendering height (`2560` for 1440p portrait, `3840` for 4K portrait). |
| `AutoPortrait` | `1` | Automatically swaps `Width` and `Height` if `Width > Height` so the Main Screen stays in `9:16` portrait orientation. |
| `ResizeMainWindow` | `1` | Resizes the Main Screen window client area in windowed mode. |
| `WindowWidth` | `0` | Custom windowed-mode width (`0` = match `Width`). Useful if you want supersampling (e.g. `2160x3840` internal render in a `1440x2560` window). |
| `WindowHeight` | `0` | Custom windowed-mode height (`0` = match `Height`). |
| `PatchNvDisplayConfig` | `1` | Patches `soundvoltex.dll`'s NVAPI `NvDisplayConfig` mode descriptors so fullscreen mode on NVIDIA GPUs sets the DisplayPort Main Display to the target resolution rather than `1080x1920`. |
| `RefreshRate` | `0` | Target display refresh rate and software `FrameLimiter` in Hz (`0` = game default, or e.g. `60`, `120`, `144`, `165`, `240`, `360`). Setting `60` also enables Valkyrie Mode 60Hz patches. |
| `NoteFpsTarget` | `0` | Target Note / Game FPS returned by `GetMainDisplayRefreshRate()` (`0` = automatically match `RefreshRate`, or set an explicit FPS like `120`, `144`, `165`, `240`). Works on both NVIDIA and AMD/Intel GPUs. |
| `DisableMonitorChange` | `0` | When set to `1`, skips `NvDisplayConfig` display mode/rotation changes at game startup. |
| `Log` | `1` | Writes diagnostic hook and surface-scaling logs to `res_hook.log`. |

---

## How It Works

1. **Dynamic Call-Site & IAT Resolution**:
   - Parses `soundvoltex.dll`'s PE Import Directory (`IMAGE_DIRECTORY_ENTRY_IMPORT`) to locate `d3d9.dll!Direct3DCreate9Ex`.
   - Scans `.text` for the `FF 15 <rel32>` indirect call instruction in `InitD3D` and redirects it to `Hook_Direct3DCreate9Ex` via a near-allocated stub page. Leaving the IAT slot itself untouched preserves `spice64.exe`'s own IAT hook regardless of load order.
2. **`NvDisplayConfig` Mode Table Patching**:
   - In fullscreen mode during `dll_entry_init`, `soundvoltex.dll` invokes `NvAPI_DISP_SetDisplayConfig` using `.rdata` mode structs `(width, height, rotation_deg, refresh_mHz)`.
   - `res_hook.dll` scans `.rdata` and updates the Main Display entries (`270° @ 60Hz` and `0°/90° @ 120Hz`) from `(1920, 1080)` to `(Height, Width)` (e.g. `2560x1440` unrotated panel resolution -> `1440x2560` portrait) and updates `refresh_mHz` when `RefreshRate > 0`, while leaving the HDMI Sub Display entry (`1920x1080 @ 0°, 60Hz`) untouched.
3. **Refresh Rate, VSync, `FrameLimiter` & `GetMainDisplayRefreshRate` Patching**:
   - **Startup Monitor Change (`dll_entry_init`)**: Optionally skips `NvDisplayConfig` (`DisableMonitorChange=1`) or forces the 60Hz `NvDisplayConfig(0)` path when `RefreshRate=60` in Valkyrie mode.
   - **Candidate Mode & Verification (`InitD3D`)**: Updates the first-priority Adapter 0 mode (`1080x1920 @ RefreshRate`), bypasses the strict `60Hz`/`120Hz` mismatch branch so VSync and `GetMainDisplayRefreshRate()` stay active at any refresh rate, and patches the software `FrameLimiter` target (`RefreshRate * 1000` mHz).
   - **Note / Game FPS Target (`GetMainDisplayRefreshRate`)**: Patches the NVAPI refresh rate conversion to load `NoteFpsTarget`, bypasses the `60Hz`/`120Hz` tolerance check (`Force Note FPS Target`), and redirects the `DUMMY` fallback `movsd` instructions to an IEEE-754 `double` allocated in the near stub page so non-NVIDIA GPUs also run at the exact target FPS without altering the shared `60.0` seconds-per-minute constant in `.rdata`.
4. **Window Creation & Per-Frame Downscale Bypass**:
   - Hooks `CreateGameWindow` via an 18-byte trampoline to resize `"Main Screen"` to the configured dimensions and shift `"Sub Screen"`'s fullscreen X position from `1080` to `Width`.
   - Scans `.text` for the per-frame `1080x1920` check on `BackBuffer0` in windowed mode and NOPs the re-stretch call so `BackBuffer0` remains at full target resolution.
5. **D3D9Ex Vtable Hooks**:
   - `IDirect3D9Ex`: `EnumAdapterModes`, `CreateDevice`, `CreateDeviceEx`
   - `IDirect3DDevice9Ex`: `Reset`, `ResetEx`, `GetBackBuffer`, `CreateTexture`, `CreateRenderTarget`, `CreateRenderTargetEx`, `CreateDepthStencilSurface`, `CreateDepthStencilSurfaceEx`, `SetRenderTarget`, `SetViewport`, `GetViewport`, `SetScissorRect`, `GetScissorRect`, `DrawPrimitiveUP`
   - `IDirect3DSurface9` & `IDirect3DTexture9`: `GetDesc`, `GetLevelDesc`, `GetSurfaceLevel`

### Dynamically Resolved Pattern Locations (SDVX 6 vs. SDVX 7)

| Hook / Patch Target | SDVX 6 (`2025-12-09`) | SDVX 7 (`2026-10-13`) |
| :--- | :--- | :--- |
| `d3d9.dll!Direct3DCreate9Ex` IAT & Call | IAT `0x821F38`, Call `0x546614` | IAT `0x884FB0`, Call `0x5A0984` |
| `CreateGameWindow` | `0x58AFE0` | `0x5E9850` |
| Windowed `BackBuffer0` Downscale Call | `0x54A4FB` | `0x5A460B` |
| `NvDisplayConfig` 60Hz & 120Hz `.rdata` Modes | `0x96A090`, `0x96A0C0` | `0x987FD0`, `0x988000` |
| Startup `NvDisplayConfig` Skip / Valkyrie 60Hz | `0x35F896` / `0x35F8BD` | `0x3E6311` / `0x3E6338` |
| `InitD3D` Candidate Mode Refresh Rate | `0x546810` | `0x5A0B80` |
| `InitD3D` Refresh Rate Verification Check | `0x546CD9` | `0x5A1049` |
| `InitD3D` Software `FrameLimiter` (mHz) | `0x5475F9` | `0x5A1969` |
| `GetMainDisplayRefreshRate` (Note/Game FPS) | `0x35EE78` | `0x3E58E8` |

---

## Building from Source

### Option 1: One-Click Build Script (`build.bat`)
Simply run `build.bat` from the repository root:
```bat
build.bat
```
It automatically locates Visual Studio / Build Tools (`cl.exe`) or MinGW-w64 (`g++.exe`), compiles `src/res_hook.cpp` into `dist/res_hook.dll`, and copies `config/res_hook.ini` to `dist/res_hook.ini`.

### Option 2: CMake
```bat
cmake -B build -A x64
cmake --build build --config Release
```

### Option 3: Manual Compiler Invocation
- **MSVC (x64 Native Tools Command Prompt)**:
  ```bat
  cl.exe /nologo /O2 /MT /EHsc /LD /W3 /D_CRT_SECURE_NO_WARNINGS src\res_hook.cpp /Fe:dist\res_hook.dll /link /INCREMENTAL:NO
  ```
- **MinGW-w64 (`g++` x86_64)**:
  ```bat
  g++.exe -shared -O2 -Wall -Wextra -std=c++17 -static-libgcc -static-libstdc++ -Wl,-Bstatic -lstdc++ -lpthread -Wl,-Bdynamic src/res_hook.cpp -o dist/res_hook.dll -ld3d9 -lgdi32 -luser32 -lpsapi
  ```
