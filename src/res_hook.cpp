// ============================================================================
// SOUND VOLTEX EXCEED GEAR - Primary Window Resolution Hook (res_hook.dll)
// ============================================================================
// Alters the internal & output Direct3D9 rendering resolution of the primary
// game window ("SOUND VOLTEX EXCEED GEAR - Main Screen", native 1080x1920)
// to a custom higher resolution (default 1440x2560 / 2560x1440 portrait),
// while leaving the touch/sub display window ("SOUND VOLTEX EXCEED GEAR -
// Sub Screen", 1920x1080) completely untouched.
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define VER_H
#include <windows.h>
#include <d3d9.h>
#include <psapi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>
#include <string.h>
#include <vector>
#include <unordered_set>
#include <mutex>

#ifdef _MSC_VER
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)
#define __builtin_return_address(x) _ReturnAddress()
#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "psapi.lib")
#endif

// ----------------------------------------------------------------------------
// Native Main Screen Dimensions (Portrait 9:16)
// ----------------------------------------------------------------------------
static constexpr UINT NATIVE_MAIN_W = 1080;
static constexpr UINT NATIVE_MAIN_H = 1920;

// ----------------------------------------------------------------------------
// Configuration State
// ----------------------------------------------------------------------------
static bool  g_enabled                = true;
static UINT  g_target_w               = 1440;
static UINT  g_target_h               = 2560;
static bool  g_auto_portrait          = true;
static bool  g_resize_main_window     = true;
static UINT  g_window_w               = 1440;
static UINT  g_window_h               = 2560;
static bool  g_patch_nv_display       = true;
static UINT  g_refresh_rate           = 0;     // 0 = game default, or target Hz (e.g. 60, 120, 144, 165, 240)
static UINT  g_note_fps               = 0;     // 0 = match RefreshRate, or custom Note/Game FPS target
static bool  g_disable_monitor_change = false; // true = skip NvDisplayConfig mode switch at startup
static bool  g_log_enabled            = true;

static float g_scale_x            = 1440.0f / 1080.0f;
static float g_scale_y            = 2560.0f / 1920.0f;

static HMODULE   g_hSelfModule    = NULL;
static char      g_log_path[MAX_PATH] = {0};
static std::mutex g_log_mutex;

// Module ranges for caller filtering
static uintptr_t g_sdvx_base      = 0;
static uintptr_t g_sdvx_end       = 0;
static uintptr_t g_afp_base       = 0;
static uintptr_t g_afp_end        = 0;
static bool      g_sdvx_hooked    = false;
static std::mutex g_hook_mutex;

// Active RT0 state
static volatile bool g_current_rt0_is_scaled = false;

// Track scaled surfaces/textures
static std::mutex g_res_mutex;
static std::unordered_set<void*> g_scaled_surfaces;
static std::unordered_set<void*> g_scaled_textures;

// ----------------------------------------------------------------------------
// Logging Helper
// ----------------------------------------------------------------------------
static void Log(const char* fmt, ...) {
    if (!g_log_enabled || g_log_path[0] == '\0') return;
    std::lock_guard<std::mutex> lock(g_log_mutex);
    FILE* fp = fopen(g_log_path, "a");
    if (!fp) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(fp, "[%02d:%02d:%02d.%03d] [res_hook] ",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    vfprintf(fp, fmt, args);
    va_end(args);
    fprintf(fp, "\n");
    fclose(fp);
}

// ----------------------------------------------------------------------------
// Caller Check: True if return address belongs to soundvoltex.dll or afp-core.dll
// ----------------------------------------------------------------------------
static inline bool IsGameCaller(const void* ret_addr) {
    uintptr_t addr = reinterpret_cast<uintptr_t>(ret_addr);
    if (g_sdvx_base && addr >= g_sdvx_base && addr < g_sdvx_end) return true;
    if (g_afp_base  && addr >= g_afp_base  && addr < g_afp_end)  return true;
    return false;
}

static void RefreshModuleBounds() {
    if (!g_sdvx_base) {
        HMODULE hSdvx = GetModuleHandleA("soundvoltex.dll");
        if (!hSdvx) hSdvx = GetModuleHandleA("soundvoltex_7.dll");
        if (hSdvx) {
            MODULEINFO mi = {};
            if (GetModuleInformation(GetCurrentProcess(), hSdvx, &mi, sizeof(mi))) {
                g_sdvx_base = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
                g_sdvx_end  = g_sdvx_base + mi.SizeOfImage;
            }
        }
    }
    if (!g_afp_base) {
        HMODULE hAfp = GetModuleHandleA("afp-core.dll");
        if (hAfp) {
            MODULEINFO mi = {};
            if (GetModuleInformation(GetCurrentProcess(), hAfp, &mi, sizeof(mi))) {
                g_afp_base = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
                g_afp_end  = g_afp_base + mi.SizeOfImage;
            }
        }
    }
}

// ----------------------------------------------------------------------------
// Vtable Hook Helper
// ----------------------------------------------------------------------------
template <typename T>
static bool HookVtableSlot(void** vtable, size_t index, T detour, T* ppOrig) {
    if (!vtable || !vtable[index]) return false;
    void* current = vtable[index];
    if (current == reinterpret_cast<void*>(detour)) return true;

    DWORD oldProt = 0;
    if (!VirtualProtect(&vtable[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProt)) {
        return false;
    }
    if (ppOrig && *ppOrig == nullptr) {
        *ppOrig = reinterpret_cast<T>(current);
    }
    vtable[index] = reinterpret_cast<void*>(detour);
    VirtualProtect(&vtable[index], sizeof(void*), oldProt, &oldProt);
    return true;
}

// Allocate executable memory within +-2GB of targetAddr for rel32 jumps/calls
static void* AllocNearModule(uintptr_t targetAddr, size_t size) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t gran = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000;

    uintptr_t minAddr = (targetAddr > 0x70000000ULL) ? (targetAddr - 0x70000000ULL) : gran;
    uintptr_t maxAddr = targetAddr + 0x70000000ULL;

    for (uintptr_t addr = (targetAddr & ~(gran - 1)); addr >= minAddr; addr -= gran) {
        void* p = VirtualAlloc(reinterpret_cast<void*>(addr), size,
                               MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (p) return p;
        if (addr < gran) break;
    }
    for (uintptr_t addr = (targetAddr & ~(gran - 1)) + gran; addr < maxAddr; addr += gran) {
        void* p = VirtualAlloc(reinterpret_cast<void*>(addr), size,
                               MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (p) return p;
    }
    return VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
}

// Build a 14-byte absolute jump: FF 25 00 00 00 00 <64-bit target>
static void WriteAbsJump64(void* dst, const void* target) {
    uint8_t* p = reinterpret_cast<uint8_t*>(dst);
    p[0] = 0xFF;
    p[1] = 0x25;
    p[2] = 0x00;
    p[3] = 0x00;
    p[4] = 0x00;
    p[5] = 0x00;
    uint64_t addr = reinterpret_cast<uint64_t>(target);
    memcpy(p + 6, &addr, sizeof(addr));
}

// ----------------------------------------------------------------------------
// D3D9 Function Pointer Typedefs
// ----------------------------------------------------------------------------
typedef HRESULT (WINAPI *PFN_Direct3DCreate9Ex)(UINT SDKVersion, IDirect3D9Ex** ppD3D);

// IDirect3D9Ex methods
typedef UINT    (STDMETHODCALLTYPE *PFN_GetAdapterModeCount)(IDirect3D9Ex*, UINT, D3DFORMAT);
typedef HRESULT (STDMETHODCALLTYPE *PFN_EnumAdapterModes)(IDirect3D9Ex*, UINT, D3DFORMAT, UINT, D3DDISPLAYMODE*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateDevice)(IDirect3D9Ex*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateDeviceEx)(IDirect3D9Ex*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*, IDirect3DDevice9Ex**);

// IDirect3DDevice9Ex methods
typedef HRESULT (STDMETHODCALLTYPE *PFN_GetSwapChain)(IDirect3DDevice9Ex*, UINT, IDirect3DSwapChain9**);
typedef HRESULT (STDMETHODCALLTYPE *PFN_Reset)(IDirect3DDevice9Ex*, D3DPRESENT_PARAMETERS*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_GetBackBuffer)(IDirect3DDevice9Ex*, UINT, UINT, D3DBACKBUFFER_TYPE, IDirect3DSurface9**);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateTexture)(IDirect3DDevice9Ex*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateRenderTarget)(IDirect3DDevice9Ex*, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9**, HANDLE*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateDepthStencilSurface)(IDirect3DDevice9Ex*, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9**, HANDLE*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_SetRenderTarget)(IDirect3DDevice9Ex*, DWORD, IDirect3DSurface9*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_SetViewport)(IDirect3DDevice9Ex*, const D3DVIEWPORT9*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_GetViewport)(IDirect3DDevice9Ex*, D3DVIEWPORT9*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_SetScissorRect)(IDirect3DDevice9Ex*, const RECT*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_GetScissorRect)(IDirect3DDevice9Ex*, RECT*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_DrawPrimitiveUP)(IDirect3DDevice9Ex*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateRenderTargetEx)(IDirect3DDevice9Ex*, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9**, HANDLE*, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateDepthStencilSurfaceEx)(IDirect3DDevice9Ex*, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9**, HANDLE*, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *PFN_ResetEx)(IDirect3DDevice9Ex*, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*);

// IDirect3DSurface9 & IDirect3DTexture9 methods
typedef HRESULT (STDMETHODCALLTYPE *PFN_Surface_GetDesc)(IDirect3DSurface9*, D3DSURFACE_DESC*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_Texture_GetLevelDesc)(IDirect3DTexture9*, UINT, D3DSURFACE_DESC*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_Texture_GetSurfaceLevel)(IDirect3DTexture9*, UINT, IDirect3DSurface9**);

// Original function pointers
static PFN_EnumAdapterModes            orig_EnumAdapterModes            = nullptr;
static PFN_CreateDevice                orig_CreateDevice                = nullptr;
static PFN_CreateDeviceEx              orig_CreateDeviceEx              = nullptr;

static PFN_Reset                       orig_Reset                       = nullptr;
static PFN_GetBackBuffer               orig_GetBackBuffer               = nullptr;
static PFN_CreateTexture               orig_CreateTexture               = nullptr;
static PFN_CreateRenderTarget          orig_CreateRenderTarget          = nullptr;
static PFN_CreateDepthStencilSurface   orig_CreateDepthStencilSurface   = nullptr;
static PFN_SetRenderTarget             orig_SetRenderTarget             = nullptr;
static PFN_SetViewport                 orig_SetViewport                 = nullptr;
static PFN_GetViewport                 orig_GetViewport                 = nullptr;
static PFN_SetScissorRect              orig_SetScissorRect              = nullptr;
static PFN_GetScissorRect              orig_GetScissorRect              = nullptr;
static PFN_DrawPrimitiveUP             orig_DrawPrimitiveUP             = nullptr;
static PFN_CreateRenderTargetEx        orig_CreateRenderTargetEx        = nullptr;
static PFN_CreateDepthStencilSurfaceEx orig_CreateDepthStencilSurfaceEx = nullptr;
static PFN_ResetEx                     orig_ResetEx                     = nullptr;

// Support multiple surface/texture vtables if D3D9 uses distinct vtables
struct SurfaceVtableEntry {
    void** vtable;
    PFN_Surface_GetDesc orig_GetDesc;
};
struct TextureVtableEntry {
    void** vtable;
    PFN_Texture_GetLevelDesc orig_GetLevelDesc;
    PFN_Texture_GetSurfaceLevel orig_GetSurfaceLevel;
};
static std::vector<SurfaceVtableEntry> g_surface_vtables;
static std::vector<TextureVtableEntry> g_texture_vtables;

// ----------------------------------------------------------------------------
// Surface & Texture Hook Implementations
// ----------------------------------------------------------------------------
static PFN_Surface_GetDesc GetOrigSurfaceGetDesc(IDirect3DSurface9* pSurface) {
    if (!pSurface) return nullptr;
    void** vt = *reinterpret_cast<void***>(pSurface);
    std::lock_guard<std::mutex> lock(g_res_mutex);
    for (const auto& entry : g_surface_vtables) {
        if (entry.vtable == vt) return entry.orig_GetDesc;
    }
    return reinterpret_cast<PFN_Surface_GetDesc>(vt[12]);
}

static bool GetRealSurfaceSize(IDirect3DSurface9* pSurface, UINT* outW, UINT* outH) {
    if (!pSurface) return false;
    PFN_Surface_GetDesc fn = GetOrigSurfaceGetDesc(pSurface);
    if (!fn) return false;
    D3DSURFACE_DESC desc = {};
    if (FAILED(fn(pSurface, &desc))) return false;
    if (outW) *outW = desc.Width;
    if (outH) *outH = desc.Height;
    return true;
}

static HRESULT STDMETHODCALLTYPE Hook_Surface_GetDesc(IDirect3DSurface9* pThis, D3DSURFACE_DESC* pDesc) {
    PFN_Surface_GetDesc origFn = GetOrigSurfaceGetDesc(pThis);
    if (!origFn) return D3DERR_INVALIDCALL;

    HRESULT hr = origFn(pThis, pDesc);
    if (SUCCEEDED(hr) && pDesc) {
        bool isScaledSize = (pDesc->Width == g_target_w && pDesc->Height == g_target_h);
        if (!isScaledSize) {
            std::lock_guard<std::mutex> lock(g_res_mutex);
            g_scaled_surfaces.erase(pThis);
        } else if (IsGameCaller(__builtin_return_address(0))) {
            bool shouldSpoof = ((pDesc->Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) != 0);
            if (!shouldSpoof) {
                std::lock_guard<std::mutex> lock(g_res_mutex);
                shouldSpoof = (g_scaled_surfaces.count(pThis) > 0);
            }
            if (shouldSpoof) {
                pDesc->Width  = NATIVE_MAIN_W;
                pDesc->Height = NATIVE_MAIN_H;
            }
        }
    }
    return hr;
}

static void EnsureSurfaceHooked(IDirect3DSurface9* pSurface, bool markScaled) {
    if (!pSurface) return;
    void** vt = *reinterpret_cast<void***>(pSurface);
    if (!vt) return;

    std::lock_guard<std::mutex> lock(g_res_mutex);
    if (markScaled) {
        g_scaled_surfaces.insert(pSurface);
    }
    for (const auto& entry : g_surface_vtables) {
        if (entry.vtable == vt) return;
    }
    PFN_Surface_GetDesc origFn = nullptr;
    if (HookVtableSlot(vt, 12, Hook_Surface_GetDesc, &origFn)) {
        g_surface_vtables.push_back({vt, origFn});
        Log("Hooked IDirect3DSurface9::GetDesc on vtable %p (orig=%p)", vt, origFn);
    }
}

static PFN_Texture_GetLevelDesc GetOrigTextureGetLevelDesc(IDirect3DTexture9* pTex) {
    if (!pTex) return nullptr;
    void** vt = *reinterpret_cast<void***>(pTex);
    std::lock_guard<std::mutex> lock(g_res_mutex);
    for (const auto& entry : g_texture_vtables) {
        if (entry.vtable == vt) return entry.orig_GetLevelDesc;
    }
    return reinterpret_cast<PFN_Texture_GetLevelDesc>(vt[17]);
}

static PFN_Texture_GetSurfaceLevel GetOrigTextureGetSurfaceLevel(IDirect3DTexture9* pTex) {
    if (!pTex) return nullptr;
    void** vt = *reinterpret_cast<void***>(pTex);
    std::lock_guard<std::mutex> lock(g_res_mutex);
    for (const auto& entry : g_texture_vtables) {
        if (entry.vtable == vt) return entry.orig_GetSurfaceLevel;
    }
    return reinterpret_cast<PFN_Texture_GetSurfaceLevel>(vt[18]);
}

static HRESULT STDMETHODCALLTYPE Hook_Texture_GetLevelDesc(IDirect3DTexture9* pThis, UINT Level, D3DSURFACE_DESC* pDesc) {
    PFN_Texture_GetLevelDesc origFn = GetOrigTextureGetLevelDesc(pThis);
    if (!origFn) return D3DERR_INVALIDCALL;

    HRESULT hr = origFn(pThis, Level, pDesc);
    if (SUCCEEDED(hr) && pDesc && Level == 0) {
        bool isScaledSize = (pDesc->Width == g_target_w && pDesc->Height == g_target_h);
        if (!isScaledSize) {
            std::lock_guard<std::mutex> lock(g_res_mutex);
            g_scaled_textures.erase(pThis);
        } else if (IsGameCaller(__builtin_return_address(0))) {
            bool shouldSpoof = ((pDesc->Usage & D3DUSAGE_RENDERTARGET) != 0);
            if (!shouldSpoof) {
                std::lock_guard<std::mutex> lock(g_res_mutex);
                shouldSpoof = (g_scaled_textures.count(pThis) > 0);
            }
            if (shouldSpoof) {
                pDesc->Width  = NATIVE_MAIN_W;
                pDesc->Height = NATIVE_MAIN_H;
            }
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_Texture_GetSurfaceLevel(IDirect3DTexture9* pThis, UINT Level, IDirect3DSurface9** ppSurfaceLevel) {
    PFN_Texture_GetSurfaceLevel origFn = GetOrigTextureGetSurfaceLevel(pThis);
    if (!origFn) return D3DERR_INVALIDCALL;

    HRESULT hr = origFn(pThis, Level, ppSurfaceLevel);
    if (SUCCEEDED(hr) && ppSurfaceLevel && *ppSurfaceLevel && Level == 0) {
        bool isScaledTex = false;
        {
            std::lock_guard<std::mutex> lock(g_res_mutex);
            isScaledTex = (g_scaled_textures.count(pThis) > 0);
        }
        if (isScaledTex) {
            EnsureSurfaceHooked(*ppSurfaceLevel, true);
        }
    }
    return hr;
}

static void EnsureTextureHooked(IDirect3DTexture9* pTex, bool markScaled) {
    if (!pTex) return;
    void** vt = *reinterpret_cast<void***>(pTex);
    if (!vt) return;

    {
        std::lock_guard<std::mutex> lock(g_res_mutex);
        if (markScaled) {
            g_scaled_textures.insert(pTex);
        }
        bool found = false;
        for (const auto& entry : g_texture_vtables) {
            if (entry.vtable == vt) {
                found = true;
                break;
            }
        }
        if (!found) {
            PFN_Texture_GetLevelDesc origLevelDesc = nullptr;
            PFN_Texture_GetSurfaceLevel origSurfLevel = nullptr;
            HookVtableSlot(vt, 17, Hook_Texture_GetLevelDesc, &origLevelDesc);
            HookVtableSlot(vt, 18, Hook_Texture_GetSurfaceLevel, &origSurfLevel);
            g_texture_vtables.push_back({vt, origLevelDesc, origSurfLevel});
            Log("Hooked IDirect3DTexture9::GetLevelDesc/GetSurfaceLevel on vtable %p", vt);
        }
    }

    if (markScaled) {
        IDirect3DSurface9* pSurf = nullptr;
        PFN_Texture_GetSurfaceLevel origSurfLevel = GetOrigTextureGetSurfaceLevel(pTex);
        if (origSurfLevel && SUCCEEDED(origSurfLevel(pTex, 0, &pSurf)) && pSurf) {
            EnsureSurfaceHooked(pSurf, true);
            pSurf->Release();
        }
    }
}

// ----------------------------------------------------------------------------
// IDirect3DDevice9Ex Hook Implementations
// ----------------------------------------------------------------------------
static HRESULT STDMETHODCALLTYPE Hook_GetBackBuffer(
    IDirect3DDevice9Ex* pThis,
    UINT iSwapChain,
    UINT iBackBuffer,
    D3DBACKBUFFER_TYPE Type,
    IDirect3DSurface9** ppBackBuffer)
{
    HRESULT hr = orig_GetBackBuffer(pThis, iSwapChain, iBackBuffer, Type, ppBackBuffer);
    if (SUCCEEDED(hr) && ppBackBuffer && *ppBackBuffer && iSwapChain == 0) {
        UINT realW = 0, realH = 0;
        EnsureSurfaceHooked(*ppBackBuffer, false);
        if (GetRealSurfaceSize(*ppBackBuffer, &realW, &realH) &&
            realW == g_target_w && realH == g_target_h) {
            EnsureSurfaceHooked(*ppBackBuffer, true);
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_CreateRenderTarget(
    IDirect3DDevice9Ex* pThis,
    UINT Width,
    UINT Height,
    D3DFORMAT Format,
    D3DMULTISAMPLE_TYPE MultiSample,
    DWORD MultisampleQuality,
    BOOL Lockable,
    IDirect3DSurface9** ppSurface,
    HANDLE* pSharedHandle)
{
    bool scaled = false;
    if (Width == NATIVE_MAIN_W && Height == NATIVE_MAIN_H) {
        Width  = g_target_w;
        Height = g_target_h;
        scaled = true;
    }
    HRESULT hr = orig_CreateRenderTarget(
        pThis, Width, Height, Format, MultiSample, MultisampleQuality,
        Lockable, ppSurface, pSharedHandle);
    if (SUCCEEDED(hr) && ppSurface && *ppSurface) {
        EnsureSurfaceHooked(*ppSurface, scaled);
        if (scaled) {
            Log("CreateRenderTarget scaled: %ux%u -> %ux%u (surface=%p, fmt=%d)",
                NATIVE_MAIN_W, NATIVE_MAIN_H, Width, Height, *ppSurface, (int)Format);
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_CreateRenderTargetEx(
    IDirect3DDevice9Ex* pThis,
    UINT Width,
    UINT Height,
    D3DFORMAT Format,
    D3DMULTISAMPLE_TYPE MultiSample,
    DWORD MultisampleQuality,
    BOOL Lockable,
    IDirect3DSurface9** ppSurface,
    HANDLE* pSharedHandle,
    DWORD Usage)
{
    bool scaled = false;
    if (Width == NATIVE_MAIN_W && Height == NATIVE_MAIN_H) {
        Width  = g_target_w;
        Height = g_target_h;
        scaled = true;
    }
    HRESULT hr = orig_CreateRenderTargetEx(
        pThis, Width, Height, Format, MultiSample, MultisampleQuality,
        Lockable, ppSurface, pSharedHandle, Usage);
    if (SUCCEEDED(hr) && ppSurface && *ppSurface) {
        EnsureSurfaceHooked(*ppSurface, scaled);
        if (scaled) {
            Log("CreateRenderTargetEx scaled: %ux%u -> %ux%u (surface=%p)",
                NATIVE_MAIN_W, NATIVE_MAIN_H, Width, Height, *ppSurface);
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_CreateDepthStencilSurface(
    IDirect3DDevice9Ex* pThis,
    UINT Width,
    UINT Height,
    D3DFORMAT Format,
    D3DMULTISAMPLE_TYPE MultiSample,
    DWORD MultisampleQuality,
    BOOL Discard,
    IDirect3DSurface9** ppSurface,
    HANDLE* pSharedHandle)
{
    bool scaled = false;
    if (Width == NATIVE_MAIN_W && Height == NATIVE_MAIN_H) {
        Width  = g_target_w;
        Height = g_target_h;
        scaled = true;
    }
    HRESULT hr = orig_CreateDepthStencilSurface(
        pThis, Width, Height, Format, MultiSample, MultisampleQuality,
        Discard, ppSurface, pSharedHandle);
    if (SUCCEEDED(hr) && ppSurface && *ppSurface) {
        EnsureSurfaceHooked(*ppSurface, scaled);
        if (scaled) {
            Log("CreateDepthStencilSurface scaled: %ux%u -> %ux%u (surface=%p, fmt=%d)",
                NATIVE_MAIN_W, NATIVE_MAIN_H, Width, Height, *ppSurface, (int)Format);
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_CreateDepthStencilSurfaceEx(
    IDirect3DDevice9Ex* pThis,
    UINT Width,
    UINT Height,
    D3DFORMAT Format,
    D3DMULTISAMPLE_TYPE MultiSample,
    DWORD MultisampleQuality,
    BOOL Discard,
    IDirect3DSurface9** ppSurface,
    HANDLE* pSharedHandle,
    DWORD Usage)
{
    bool scaled = false;
    if (Width == NATIVE_MAIN_W && Height == NATIVE_MAIN_H) {
        Width  = g_target_w;
        Height = g_target_h;
        scaled = true;
    }
    HRESULT hr = orig_CreateDepthStencilSurfaceEx(
        pThis, Width, Height, Format, MultiSample, MultisampleQuality,
        Discard, ppSurface, pSharedHandle, Usage);
    if (SUCCEEDED(hr) && ppSurface && *ppSurface) {
        EnsureSurfaceHooked(*ppSurface, scaled);
        if (scaled) {
            Log("CreateDepthStencilSurfaceEx scaled: %ux%u -> %ux%u (surface=%p)",
                NATIVE_MAIN_W, NATIVE_MAIN_H, Width, Height, *ppSurface);
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_CreateTexture(
    IDirect3DDevice9Ex* pThis,
    UINT Width,
    UINT Height,
    UINT Levels,
    DWORD Usage,
    D3DFORMAT Format,
    D3DPOOL Pool,
    IDirect3DTexture9** ppTexture,
    HANDLE* pSharedHandle)
{
    bool scaled = false;
    if ((Usage & D3DUSAGE_RENDERTARGET) && Width == NATIVE_MAIN_W && Height == NATIVE_MAIN_H) {
        Width  = g_target_w;
        Height = g_target_h;
        scaled = true;
    }
    HRESULT hr = orig_CreateTexture(
        pThis, Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle);
    if (SUCCEEDED(hr) && ppTexture && *ppTexture && scaled) {
        EnsureTextureHooked(*ppTexture, true);
        Log("CreateTexture(RENDERTARGET) scaled: %ux%u -> %ux%u (tex=%p, fmt=%d)",
            NATIVE_MAIN_W, NATIVE_MAIN_H, Width, Height, *ppTexture, (int)Format);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_SetRenderTarget(
    IDirect3DDevice9Ex* pThis,
    DWORD RenderTargetIndex,
    IDirect3DSurface9* pRenderTarget)
{
    if (RenderTargetIndex == 0) {
        if (!pRenderTarget) {
            g_current_rt0_is_scaled = false;
        } else {
            EnsureSurfaceHooked(pRenderTarget, false);
            UINT realW = 0, realH = 0;
            if (GetRealSurfaceSize(pRenderTarget, &realW, &realH) &&
                realW == g_target_w && realH == g_target_h) {
                g_current_rt0_is_scaled = true;
                EnsureSurfaceHooked(pRenderTarget, true);
            } else {
                g_current_rt0_is_scaled = false;
            }
        }
    }
    return orig_SetRenderTarget(pThis, RenderTargetIndex, pRenderTarget);
}

static HRESULT STDMETHODCALLTYPE Hook_SetViewport(
    IDirect3DDevice9Ex* pThis,
    const D3DVIEWPORT9* pViewport)
{
    if (pViewport && IsGameCaller(__builtin_return_address(0))) {
        bool isFullNativeMain = (pViewport->Width == NATIVE_MAIN_W && pViewport->Height == NATIVE_MAIN_H);
        if (g_current_rt0_is_scaled || isFullNativeMain) {
            D3DVIEWPORT9 vp = *pViewport;
            vp.X      = static_cast<DWORD>(lroundf(static_cast<float>(pViewport->X)      * g_scale_x));
            vp.Y      = static_cast<DWORD>(lroundf(static_cast<float>(pViewport->Y)      * g_scale_y));
            vp.Width  = static_cast<DWORD>(lroundf(static_cast<float>(pViewport->Width)  * g_scale_x));
            vp.Height = static_cast<DWORD>(lroundf(static_cast<float>(pViewport->Height) * g_scale_y));
            return orig_SetViewport(pThis, &vp);
        }
    }
    return orig_SetViewport(pThis, pViewport);
}

static HRESULT STDMETHODCALLTYPE Hook_GetViewport(
    IDirect3DDevice9Ex* pThis,
    D3DVIEWPORT9* pViewport)
{
    HRESULT hr = orig_GetViewport(pThis, pViewport);
    if (SUCCEEDED(hr) && pViewport && IsGameCaller(__builtin_return_address(0))) {
        bool isFullScaledMain = (pViewport->Width == g_target_w && pViewport->Height == g_target_h);
        if (g_current_rt0_is_scaled || isFullScaledMain) {
            pViewport->X      = static_cast<DWORD>(lroundf(static_cast<float>(pViewport->X)      / g_scale_x));
            pViewport->Y      = static_cast<DWORD>(lroundf(static_cast<float>(pViewport->Y)      / g_scale_y));
            pViewport->Width  = static_cast<DWORD>(lroundf(static_cast<float>(pViewport->Width)  / g_scale_x));
            pViewport->Height = static_cast<DWORD>(lroundf(static_cast<float>(pViewport->Height) / g_scale_y));
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_SetScissorRect(
    IDirect3DDevice9Ex* pThis,
    const RECT* pRect)
{
    if (pRect && g_current_rt0_is_scaled && IsGameCaller(__builtin_return_address(0))) {
        RECT rc;
        rc.left   = static_cast<LONG>(lroundf(static_cast<float>(pRect->left)   * g_scale_x));
        rc.top    = static_cast<LONG>(lroundf(static_cast<float>(pRect->top)    * g_scale_y));
        rc.right  = static_cast<LONG>(lroundf(static_cast<float>(pRect->right)  * g_scale_x));
        rc.bottom = static_cast<LONG>(lroundf(static_cast<float>(pRect->bottom) * g_scale_y));
        return orig_SetScissorRect(pThis, &rc);
    }
    return orig_SetScissorRect(pThis, pRect);
}

static HRESULT STDMETHODCALLTYPE Hook_GetScissorRect(
    IDirect3DDevice9Ex* pThis,
    RECT* pRect)
{
    HRESULT hr = orig_GetScissorRect(pThis, pRect);
    if (SUCCEEDED(hr) && pRect && g_current_rt0_is_scaled && IsGameCaller(__builtin_return_address(0))) {
        pRect->left   = static_cast<LONG>(lroundf(static_cast<float>(pRect->left)   / g_scale_x));
        pRect->top    = static_cast<LONG>(lroundf(static_cast<float>(pRect->top)    / g_scale_y));
        pRect->right  = static_cast<LONG>(lroundf(static_cast<float>(pRect->right)  / g_scale_x));
        pRect->bottom = static_cast<LONG>(lroundf(static_cast<float>(pRect->bottom) / g_scale_y));
    }
    return hr;
}

static UINT GetVertexCount(D3DPRIMITIVETYPE type, UINT primCount) {
    switch (type) {
        case D3DPT_POINTLIST:     return primCount;
        case D3DPT_LINELIST:      return primCount * 2;
        case D3DPT_LINESTRIP:     return primCount + 1;
        case D3DPT_TRIANGLELIST:  return primCount * 3;
        case D3DPT_TRIANGLESTRIP: return primCount + 2;
        case D3DPT_TRIANGLEFAN:   return primCount + 2;
        default:                  return 0;
    }
}

static HRESULT STDMETHODCALLTYPE Hook_DrawPrimitiveUP(
    IDirect3DDevice9Ex* pThis,
    D3DPRIMITIVETYPE PrimitiveType,
    UINT PrimitiveCount,
    const void* pVertexStreamZeroData,
    UINT VertexStreamZeroStride)
{
    if (g_current_rt0_is_scaled && pVertexStreamZeroData &&
        VertexStreamZeroStride >= 16 && PrimitiveCount > 0 &&
        IsGameCaller(__builtin_return_address(0)))
    {
        DWORD fvf = 0;
        if (SUCCEEDED(pThis->GetFVF(&fvf)) && (fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW) {
            UINT vertCount = GetVertexCount(PrimitiveType, PrimitiveCount);
            if (vertCount > 0 && vertCount <= 65536) {
                static thread_local std::vector<uint8_t> s_scaledVerts;
                size_t totalBytes = static_cast<size_t>(vertCount) * VertexStreamZeroStride;
                if (s_scaledVerts.size() < totalBytes) {
                    s_scaledVerts.resize(totalBytes);
                }
                memcpy(s_scaledVerts.data(), pVertexStreamZeroData, totalBytes);
                uint8_t* ptr = s_scaledVerts.data();
                for (UINT i = 0; i < vertCount; ++i) {
                    float* xy = reinterpret_cast<float*>(ptr + i * VertexStreamZeroStride);
                    xy[0] = (xy[0] + 0.5f) * g_scale_x - 0.5f;
                    xy[1] = (xy[1] + 0.5f) * g_scale_y - 0.5f;
                }
                return orig_DrawPrimitiveUP(pThis, PrimitiveType, PrimitiveCount,
                                            s_scaledVerts.data(), VertexStreamZeroStride);
            }
        }
    }
    return orig_DrawPrimitiveUP(pThis, PrimitiveType, PrimitiveCount,
                                pVertexStreamZeroData, VertexStreamZeroStride);
}

static HRESULT STDMETHODCALLTYPE Hook_Reset(
    IDirect3DDevice9Ex* pThis,
    D3DPRESENT_PARAMETERS* pPresentationParameters)
{
    if (pPresentationParameters &&
        pPresentationParameters[0].BackBufferWidth  == NATIVE_MAIN_W &&
        pPresentationParameters[0].BackBufferHeight == NATIVE_MAIN_H)
    {
        D3DPRESENT_PARAMETERS origPP = pPresentationParameters[0];
        pPresentationParameters[0].BackBufferWidth  = g_target_w;
        pPresentationParameters[0].BackBufferHeight = g_target_h;
        if (!pPresentationParameters[0].Windowed && g_refresh_rate > 0) {
            pPresentationParameters[0].FullScreen_RefreshRateInHz = g_refresh_rate;
        }
        HRESULT hr = orig_Reset(pThis, pPresentationParameters);
        if (FAILED(hr) && !origPP.Windowed) {
            Log("Reset at %ux%u failed in fullscreen (hr=0x%08X), falling back to %ux%u backbuffer",
                g_target_w, g_target_h, (unsigned)hr, NATIVE_MAIN_W, NATIVE_MAIN_H);
            pPresentationParameters[0] = origPP;
            hr = orig_Reset(pThis, pPresentationParameters);
        }
        return hr;
    }
    return orig_Reset(pThis, pPresentationParameters);
}

static HRESULT STDMETHODCALLTYPE Hook_ResetEx(
    IDirect3DDevice9Ex* pThis,
    D3DPRESENT_PARAMETERS* pPresentationParameters,
    D3DDISPLAYMODEEX* pFullscreenDisplayMode)
{
    if (pPresentationParameters &&
        pPresentationParameters[0].BackBufferWidth  == NATIVE_MAIN_W &&
        pPresentationParameters[0].BackBufferHeight == NATIVE_MAIN_H)
    {
        D3DPRESENT_PARAMETERS origPP = pPresentationParameters[0];
        D3DDISPLAYMODEEX origMode = {};
        if (pFullscreenDisplayMode) origMode = pFullscreenDisplayMode[0];

        pPresentationParameters[0].BackBufferWidth  = g_target_w;
        pPresentationParameters[0].BackBufferHeight = g_target_h;
        if (!pPresentationParameters[0].Windowed && g_refresh_rate > 0) {
            pPresentationParameters[0].FullScreen_RefreshRateInHz = g_refresh_rate;
        }
        if (pFullscreenDisplayMode && !pPresentationParameters[0].Windowed) {
            pFullscreenDisplayMode[0].Width  = g_target_w;
            pFullscreenDisplayMode[0].Height = g_target_h;
            if (g_refresh_rate > 0) {
                pFullscreenDisplayMode[0].RefreshRate = g_refresh_rate;
            }
        }
        HRESULT hr = orig_ResetEx(pThis, pPresentationParameters, pFullscreenDisplayMode);
        if (FAILED(hr) && !origPP.Windowed) {
            Log("ResetEx at %ux%u failed in fullscreen (hr=0x%08X), falling back to %ux%u backbuffer",
                g_target_w, g_target_h, (unsigned)hr, NATIVE_MAIN_W, NATIVE_MAIN_H);
            pPresentationParameters[0] = origPP;
            if (pFullscreenDisplayMode) pFullscreenDisplayMode[0] = origMode;
            hr = orig_ResetEx(pThis, pPresentationParameters, pFullscreenDisplayMode);
        }
        return hr;
    }
    return orig_ResetEx(pThis, pPresentationParameters, pFullscreenDisplayMode);
}

static void HookD3DDevice9(IDirect3DDevice9Ex* pDevice) {
    if (!pDevice) return;
    void** vt = *reinterpret_cast<void***>(pDevice);
    if (!vt) return;

    HookVtableSlot(vt, 16,  Hook_Reset,                        &orig_Reset);
    HookVtableSlot(vt, 18,  Hook_GetBackBuffer,                &orig_GetBackBuffer);
    HookVtableSlot(vt, 23,  Hook_CreateTexture,                &orig_CreateTexture);
    HookVtableSlot(vt, 28,  Hook_CreateRenderTarget,           &orig_CreateRenderTarget);
    HookVtableSlot(vt, 29,  Hook_CreateDepthStencilSurface,    &orig_CreateDepthStencilSurface);
    HookVtableSlot(vt, 37,  Hook_SetRenderTarget,              &orig_SetRenderTarget);
    HookVtableSlot(vt, 47,  Hook_SetViewport,                  &orig_SetViewport);
    HookVtableSlot(vt, 48,  Hook_GetViewport,                  &orig_GetViewport);
    HookVtableSlot(vt, 75,  Hook_SetScissorRect,               &orig_SetScissorRect);
    HookVtableSlot(vt, 76,  Hook_GetScissorRect,               &orig_GetScissorRect);
    HookVtableSlot(vt, 83,  Hook_DrawPrimitiveUP,              &orig_DrawPrimitiveUP);
    HookVtableSlot(vt, 123, Hook_CreateRenderTargetEx,         &orig_CreateRenderTargetEx);
    HookVtableSlot(vt, 124, Hook_CreateDepthStencilSurfaceEx,  &orig_CreateDepthStencilSurfaceEx);
    HookVtableSlot(vt, 132, Hook_ResetEx,                      &orig_ResetEx);

    Log("Hooked IDirect3DDevice9Ex vtable at %p (Device=%p)", vt, pDevice);
}

// ----------------------------------------------------------------------------
// IDirect3D9Ex Hook Implementations
// ----------------------------------------------------------------------------
static HRESULT STDMETHODCALLTYPE Hook_EnumAdapterModes(
    IDirect3D9Ex* pThis,
    UINT Adapter,
    D3DFORMAT Format,
    UINT Mode,
    D3DDISPLAYMODE* pMode)
{
    HRESULT hr = orig_EnumAdapterModes(pThis, Adapter, Format, Mode, pMode);
    // In InitD3D (0x1805468b2), Adapter 0 is scanned for 1080x1920@120 or 1080x1920@60
    // (or 1080x1920@g_refresh_rate if RefreshRate is set).
    // If the primary monitor is natively TargetW x TargetH (e.g. 1440x2560) and does not
    // expose a 1080x1920 mode, map TargetW x TargetH -> 1080x1920 here so InitD3D picks
    // the monitor's native high refresh rate before CreateDeviceEx upgrades it.
    if (SUCCEEDED(hr) && pMode && Adapter == 0 && IsGameCaller(__builtin_return_address(0))) {
        if (pMode->Width == g_target_w && pMode->Height == g_target_h) {
            pMode->Width  = NATIVE_MAIN_W;
            pMode->Height = NATIVE_MAIN_H;
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_CreateDevice(
    IDirect3D9Ex* pThis,
    UINT Adapter,
    D3DDEVTYPE DeviceType,
    HWND hFocusWindow,
    DWORD BehaviorFlags,
    D3DPRESENT_PARAMETERS* pPresentationParameters,
    IDirect3DDevice9** ppReturnedDeviceInterface)
{
    RefreshModuleBounds();
    D3DPRESENT_PARAMETERS origPP = {};
    bool modified = false;

    if (pPresentationParameters &&
        pPresentationParameters[0].BackBufferWidth  == NATIVE_MAIN_W &&
        pPresentationParameters[0].BackBufferHeight == NATIVE_MAIN_H)
    {
        origPP = pPresentationParameters[0];
        pPresentationParameters[0].BackBufferWidth  = g_target_w;
        pPresentationParameters[0].BackBufferHeight = g_target_h;
        if (!pPresentationParameters[0].Windowed && g_refresh_rate > 0) {
            pPresentationParameters[0].FullScreen_RefreshRateInHz = g_refresh_rate;
        }
        modified = true;
        Log("CreateDevice: Scaling Main Screen BackBuffer[0] %ux%u -> %ux%u (Windowed=%d, Refresh=%uHz)",
            NATIVE_MAIN_W, NATIVE_MAIN_H, g_target_w, g_target_h,
            pPresentationParameters[0].Windowed,
            pPresentationParameters[0].FullScreen_RefreshRateInHz);
    }

    HRESULT hr = orig_CreateDevice(
        pThis, Adapter, DeviceType, hFocusWindow, BehaviorFlags,
        pPresentationParameters, ppReturnedDeviceInterface);

    if (FAILED(hr) && modified && !origPP.Windowed) {
        Log("CreateDevice at %ux%u failed in fullscreen (hr=0x%08X); retrying with %ux%u display mode (internal RTs will still render at %ux%u)",
            g_target_w, g_target_h, (unsigned)hr, NATIVE_MAIN_W, NATIVE_MAIN_H, g_target_w, g_target_h);
        pPresentationParameters[0] = origPP;
        hr = orig_CreateDevice(
            pThis, Adapter, DeviceType, hFocusWindow, BehaviorFlags,
            pPresentationParameters, ppReturnedDeviceInterface);
    }

    if (SUCCEEDED(hr) && ppReturnedDeviceInterface && *ppReturnedDeviceInterface) {
        HookD3DDevice9(reinterpret_cast<IDirect3DDevice9Ex*>(*ppReturnedDeviceInterface));
    }
    return hr;
}

// Helper to find an alternative supported fullscreen refresh rate for (w, h) on Adapter
static UINT FindSupportedRefreshRate(
    IDirect3D9Ex* pD3D,
    UINT Adapter,
    D3DFORMAT Format,
    UINT targetW,
    UINT targetH,
    UINT preferredHz)
{
    if (!pD3D || !orig_EnumAdapterModes) return 0;
    UINT bestHz = 0;
    UINT bestDiff = 0xFFFFFFFFU;
    for (UINT idx = 0; idx < 512; ++idx) {
        D3DDISPLAYMODE mode = {};
        if (FAILED(orig_EnumAdapterModes(pD3D, Adapter, Format, idx, &mode))) {
            break;
        }
        if (mode.Width == targetW && mode.Height == targetH && mode.RefreshRate > 0) {
            UINT diff = (mode.RefreshRate > preferredHz)
                ? (mode.RefreshRate - preferredHz)
                : (preferredHz - mode.RefreshRate);
            if (diff < bestDiff || (diff == bestDiff && mode.RefreshRate > bestHz)) {
                bestDiff = diff;
                bestHz   = mode.RefreshRate;
            }
        }
    }
    return bestHz;
}

static HRESULT STDMETHODCALLTYPE Hook_CreateDeviceEx(
    IDirect3D9Ex* pThis,
    UINT Adapter,
    D3DDEVTYPE DeviceType,
    HWND hFocusWindow,
    DWORD BehaviorFlags,
    D3DPRESENT_PARAMETERS* pPresentationParameters,
    D3DDISPLAYMODEEX* pFullscreenDisplayMode,
    IDirect3DDevice9Ex** ppReturnedDeviceInterface)
{
    RefreshModuleBounds();
    D3DPRESENT_PARAMETERS origPP = {};
    D3DDISPLAYMODEEX origMode = {};
    bool modified = false;

    if (pPresentationParameters &&
        pPresentationParameters[0].BackBufferWidth  == NATIVE_MAIN_W &&
        pPresentationParameters[0].BackBufferHeight == NATIVE_MAIN_H)
    {
        origPP = pPresentationParameters[0];
        pPresentationParameters[0].BackBufferWidth  = g_target_w;
        pPresentationParameters[0].BackBufferHeight = g_target_h;
        if (!pPresentationParameters[0].Windowed && g_refresh_rate > 0) {
            pPresentationParameters[0].FullScreen_RefreshRateInHz = g_refresh_rate;
        }
        if (pFullscreenDisplayMode && !pPresentationParameters[0].Windowed) {
            origMode = pFullscreenDisplayMode[0];
            pFullscreenDisplayMode[0].Width  = g_target_w;
            pFullscreenDisplayMode[0].Height = g_target_h;
            if (g_refresh_rate > 0) {
                pFullscreenDisplayMode[0].RefreshRate = g_refresh_rate;
            }
        }
        modified = true;
        Log("CreateDeviceEx: Scaling Main Screen BackBuffer[0] %ux%u -> %ux%u (Windowed=%d, Refresh=%uHz, MultiHead=%d)",
            NATIVE_MAIN_W, NATIVE_MAIN_H, g_target_w, g_target_h,
            pPresentationParameters[0].Windowed,
            pPresentationParameters[0].FullScreen_RefreshRateInHz,
            (BehaviorFlags & D3DCREATE_ADAPTERGROUP_DEVICE) ? 1 : 0);
    }

    HRESULT hr = orig_CreateDeviceEx(
        pThis, Adapter, DeviceType, hFocusWindow, BehaviorFlags,
        pPresentationParameters, pFullscreenDisplayMode, ppReturnedDeviceInterface);

    if (FAILED(hr) && modified && !origPP.Windowed) {
        UINT requestedHz = pPresentationParameters[0].FullScreen_RefreshRateInHz;
        UINT altHz = FindSupportedRefreshRate(
            pThis, Adapter, origPP.BackBufferFormat,
            g_target_w, g_target_h, requestedHz);
        if (altHz > 0 && altHz != requestedHz) {
            Log("CreateDeviceEx at %ux%u @ %uHz failed (hr=0x%08X); retrying at supported %ux%u @ %uHz",
                g_target_w, g_target_h, requestedHz, (unsigned)hr,
                g_target_w, g_target_h, altHz);
            pPresentationParameters[0] = origPP;
            pPresentationParameters[0].BackBufferWidth            = g_target_w;
            pPresentationParameters[0].BackBufferHeight           = g_target_h;
            pPresentationParameters[0].FullScreen_RefreshRateInHz = altHz;
            if (pFullscreenDisplayMode) {
                pFullscreenDisplayMode[0]             = origMode;
                pFullscreenDisplayMode[0].Width       = g_target_w;
                pFullscreenDisplayMode[0].Height      = g_target_h;
                pFullscreenDisplayMode[0].RefreshRate = altHz;
            }
            hr = orig_CreateDeviceEx(
                pThis, Adapter, DeviceType, hFocusWindow, BehaviorFlags,
                pPresentationParameters, pFullscreenDisplayMode, ppReturnedDeviceInterface);
        }
    }

    if (FAILED(hr) && modified && !origPP.Windowed) {
        Log("CreateDeviceEx at %ux%u failed in fullscreen (hr=0x%08X); falling back to %ux%u display mode while keeping Main Screen RenderTargets at %ux%u",
            g_target_w, g_target_h, (unsigned)hr, NATIVE_MAIN_W, NATIVE_MAIN_H, g_target_w, g_target_h);
        pPresentationParameters[0] = origPP;
        if (pFullscreenDisplayMode) {
            pFullscreenDisplayMode[0] = origMode;
        }
        hr = orig_CreateDeviceEx(
            pThis, Adapter, DeviceType, hFocusWindow, BehaviorFlags,
            pPresentationParameters, pFullscreenDisplayMode, ppReturnedDeviceInterface);
    }

    if (SUCCEEDED(hr) && ppReturnedDeviceInterface && *ppReturnedDeviceInterface) {
        HookD3DDevice9(*ppReturnedDeviceInterface);
    }
    return hr;
}

static void HookD3D9Ex(IDirect3D9Ex* pD3D) {
    if (!pD3D) return;
    void** vt = *reinterpret_cast<void***>(pD3D);
    if (!vt) return;

    HookVtableSlot(vt, 7,  Hook_EnumAdapterModes, &orig_EnumAdapterModes);
    HookVtableSlot(vt, 16, Hook_CreateDevice,     &orig_CreateDevice);
    HookVtableSlot(vt, 20, Hook_CreateDeviceEx,   &orig_CreateDeviceEx);

    Log("Hooked IDirect3D9Ex vtable at %p (D3D=%p)", vt, pD3D);
}

// ----------------------------------------------------------------------------
// soundvoltex.dll Direct3DCreate9Ex Call Interceptor & Window Hook
// ----------------------------------------------------------------------------
static PFN_Direct3DCreate9Ex* g_d3dcreate9ex_iat_slot = nullptr;
static PFN_Direct3DCreate9Ex  g_orig_Direct3DCreate9Ex = nullptr;

static HRESULT WINAPI Hook_Direct3DCreate9Ex(UINT SDKVersion, IDirect3D9Ex** ppD3D) {
    RefreshModuleBounds();
    PFN_Direct3DCreate9Ex fn = g_orig_Direct3DCreate9Ex;
    if (!fn && g_d3dcreate9ex_iat_slot) {
        PFN_Direct3DCreate9Ex iatFn = *g_d3dcreate9ex_iat_slot;
        if (iatFn && iatFn != &Hook_Direct3DCreate9Ex) {
            fn = iatFn;
        }
    }
    if (!fn) {
        HMODULE hD3D9 = GetModuleHandleA("d3d9.dll");
        if (!hD3D9) hD3D9 = LoadLibraryA("d3d9.dll");
        if (hD3D9) {
            fn = reinterpret_cast<PFN_Direct3DCreate9Ex>(
                reinterpret_cast<void*>(GetProcAddress(hD3D9, "Direct3DCreate9Ex")));
        }
    }
    if (!fn) return D3DERR_NOTAVAILABLE;

    HRESULT hr = fn(SDKVersion, ppD3D);
    Log("Direct3DCreate9Ex(SDKVersion=%u) -> hr=0x%08X, *ppD3D=%p",
        SDKVersion, (unsigned)hr, (ppD3D ? *ppD3D : nullptr));
    if (SUCCEEDED(hr) && ppD3D && *ppD3D) {
        HookD3D9Ex(*ppD3D);
    }
    return hr;
}

// bool __fastcall CreateGameWindow(void* this, HINSTANCE hInst, const char* title, int x, int y, int w, int h, bool fullscreen, WNDPROC wndproc)
typedef bool (__fastcall *PFN_CreateGameWindow)(
    void* pThis, HINSTANCE hInst, const char* title,
    int x, int y, int w, int h, bool fullscreen, WNDPROC wndproc);

static PFN_CreateGameWindow orig_CreateGameWindow = nullptr;

static bool __fastcall Hook_CreateGameWindow(
    void* pThis, HINSTANCE hInst, const char* title,
    int x, int y, int w, int h, bool fullscreen, WNDPROC wndproc)
{
    if (title && strstr(title, "Main Screen") != nullptr) {
        if (w == (int)NATIVE_MAIN_W && h == (int)NATIVE_MAIN_H) {
            if (fullscreen) {
                Log("CreateGameWindow(\"%s\"): Resizing fullscreen Main Screen window %dx%d -> %ux%u (pos=%d,%d)",
                    title, w, h, g_target_w, g_target_h, x, y);
                w = static_cast<int>(g_target_w);
                h = static_cast<int>(g_target_h);
            } else if (g_resize_main_window) {
                Log("CreateGameWindow(\"%s\"): Resizing windowed Main Screen window %dx%d -> %ux%u (pos=%d,%d)",
                    title, w, h, g_window_w, g_window_h, x, y);
                w = static_cast<int>(g_window_w);
                h = static_cast<int>(g_window_h);
            } else {
                Log("CreateGameWindow(\"%s\"): Keeping Main Screen window size %dx%d (fullscreen=%d, pos=%d,%d)",
                    title, w, h, (int)fullscreen, x, y);
            }
        } else {
            Log("CreateGameWindow(\"%s\"): Main Screen window size %dx%d (fullscreen=%d, pos=%d,%d)",
                title, w, h, (int)fullscreen, x, y);
        }
    } else if (title) {
        if (fullscreen && x == (int)NATIVE_MAIN_W) {
            Log("CreateGameWindow(\"%s\"): Adjusting fullscreen Sub Screen X offset %d -> %u (%dx%d at %u,%d)",
                title, x, g_target_w, w, h, g_target_w, y);
            x = static_cast<int>(g_target_w);
        } else {
            Log("CreateGameWindow(\"%s\"): Leaving Sub Screen untouched (%dx%d at %d,%d, fullscreen=%d)",
                title, w, h, x, y, (int)fullscreen);
        }
    }
    return orig_CreateGameWindow(pThis, hInst, title, x, y, w, h, fullscreen, wndproc);
}

// ----------------------------------------------------------------------------
// PE Section, Import Directory & Pattern Scanning Helpers
// ----------------------------------------------------------------------------
static bool GetPeSection(uintptr_t base, const char* secName, uint8_t** outStart, size_t* outSize) {
    if (!base) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        char name[9] = {0};
        memcpy(name, sec->Name, 8);
        if (_stricmp(name, secName) == 0) {
            *outStart = reinterpret_cast<uint8_t*>(base + sec->VirtualAddress);
            *outSize  = sec->Misc.VirtualSize ? sec->Misc.VirtualSize : sec->SizeOfRawData;
            return true;
        }
    }
    return false;
}

static uintptr_t* FindIatSlot(uintptr_t base, const char* dllName, const char* funcName) {
    if (!base) return nullptr;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || !dir.Size) return nullptr;

    const auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
    for (; desc->Name != 0; ++desc) {
        const char* modName = reinterpret_cast<const char*>(base + desc->Name);
        if (_stricmp(modName, dllName) != 0) continue;

        const auto* origThunk = reinterpret_cast<const IMAGE_THUNK_DATA64*>(
            base + (desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk));
        auto* iatThunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + desc->FirstThunk);

        for (; origThunk->u1.AddressOfData != 0; ++origThunk, ++iatThunk) {
            if (IMAGE_SNAP_BY_ORDINAL64(origThunk->u1.Ordinal)) continue;
            const auto* byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
                base + origThunk->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(byName->Name), funcName) == 0) {
                return reinterpret_cast<uintptr_t*>(&iatThunk->u1.Function);
            }
        }
    }
    return nullptr;
}

static uint8_t* ScanBytes(uint8_t* start, size_t size, const uint8_t* pattern, size_t patLen) {
    if (!start || !pattern || patLen == 0 || size < patLen) return nullptr;
    size_t limit = size - patLen;
    uint8_t first = pattern[0];
    for (size_t i = 0; i <= limit; ++i) {
        if (start[i] == first && memcmp(start + i, pattern, patLen) == 0) {
            return start + i;
        }
    }
    return nullptr;
}

static uint8_t* ScanPattern(uint8_t* start, size_t size, const uint8_t* pattern, const char* mask) {
    if (!start || !pattern || !mask) return nullptr;
    size_t patLen = strlen(mask);
    if (patLen == 0 || size < patLen) return nullptr;
    size_t limit = size - patLen;
    for (size_t i = 0; i <= limit; ++i) {
        bool match = true;
        for (size_t j = 0; j < patLen; ++j) {
            if (mask[j] == 'x' && start[i + j] != pattern[j]) {
                match = false;
                break;
            }
        }
        if (match) return start + i;
    }
    return nullptr;
}

// ----------------------------------------------------------------------------
// Install Patches on soundvoltex.dll (Version-Independent for SDVX6 & SDVX7+)
// ----------------------------------------------------------------------------
static bool InstallSdvxPatches() {
    std::lock_guard<std::mutex> lock(g_hook_mutex);
    if (g_sdvx_hooked) return true;

    RefreshModuleBounds();
    if (!g_sdvx_base) return false;

    Log("Found soundvoltex.dll at 0x%llX - 0x%llX",
        (unsigned long long)g_sdvx_base, (unsigned long long)g_sdvx_end);

    uint8_t* textStart = nullptr;
    size_t   textSize  = 0;
    if (!GetPeSection(g_sdvx_base, ".text", &textStart, &textSize)) {
        Log("ERROR: Failed to locate .text section in soundvoltex.dll");
        return false;
    }

    uint8_t* rdataStart = nullptr;
    size_t   rdataSize  = 0;
    GetPeSection(g_sdvx_base, ".rdata", &rdataStart, &rdataSize);

    // Allocate near stub page within +-2GB of soundvoltex.dll
    uint8_t* stubPage = reinterpret_cast<uint8_t*>(AllocNearModule(g_sdvx_base, 4096));
    if (!stubPage) {
        Log("ERROR: Failed to allocate near stub page for soundvoltex.dll");
        return false;
    }

    // ------------------------------------------------------------------------
    // 1. Dynamically resolve Direct3DCreate9Ex IAT slot & patch call site in InitD3D
    //    (SDVX6: IAT RVA 0x821F38, call RVA 0x546614;
    //     SDVX7: IAT RVA 0x884FB0, call RVA 0x5A0994)
    //    Redirecting the FF 15 call instruction in .text leaves the IAT slot itself
    //    untouched so spice64.exe's IAT hook on soundvoltex.dll is preserved.
    // ------------------------------------------------------------------------
    g_d3dcreate9ex_iat_slot = reinterpret_cast<PFN_Direct3DCreate9Ex*>(
        FindIatSlot(g_sdvx_base, "d3d9.dll", "Direct3DCreate9Ex"));

    if (g_d3dcreate9ex_iat_slot) {
        uintptr_t iatAddr = reinterpret_cast<uintptr_t>(g_d3dcreate9ex_iat_slot);
        Log("Resolved d3d9.dll!Direct3DCreate9Ex IAT slot at %p (RVA 0x%llX, current target=%p)",
            g_d3dcreate9ex_iat_slot,
            (unsigned long long)(iatAddr - g_sdvx_base),
            reinterpret_cast<void*>(*g_d3dcreate9ex_iat_slot));

        uint8_t* d3dStub = stubPage; // 14-byte jump to Hook_Direct3DCreate9Ex
        WriteAbsJump64(d3dStub, reinterpret_cast<const void*>(&Hook_Direct3DCreate9Ex));

        bool callPatched = false;
        for (size_t i = 0; i + 6 <= textSize; ++i) {
            uint8_t* insn = textStart + i;
            if (insn[0] == 0xFF && insn[1] == 0x15) {
                int32_t disp = 0;
                memcpy(&disp, insn + 2, sizeof(disp));
                uintptr_t target = reinterpret_cast<uintptr_t>(insn + 6) + static_cast<intptr_t>(disp);
                if (target == iatAddr) {
                    intptr_t rel = reinterpret_cast<intptr_t>(d3dStub) - reinterpret_cast<intptr_t>(insn + 5);
                    if (rel >= INT32_MIN && rel <= INT32_MAX) {
                        DWORD oldProt = 0;
                        VirtualProtect(insn, 6, PAGE_EXECUTE_READWRITE, &oldProt);
                        insn[0] = 0xE8; // call rel32
                        int32_t rel32 = static_cast<int32_t>(rel);
                        memcpy(insn + 1, &rel32, 4);
                        insn[5] = 0x90; // nop
                        VirtualProtect(insn, 6, oldProt, &oldProt);
                        FlushInstructionCache(GetCurrentProcess(), insn, 6);
                        Log("Patched InitD3D Direct3DCreate9Ex call site at %p (RVA 0x%llX) -> stub %p",
                            insn,
                            (unsigned long long)(reinterpret_cast<uintptr_t>(insn) - g_sdvx_base),
                            d3dStub);
                        callPatched = true;
                    }
                }
            }
        }

        if (!callPatched) {
            // Fallback: hook the IAT slot directly while chaining the existing pointer
            DWORD oldProt = 0;
            if (VirtualProtect(g_d3dcreate9ex_iat_slot, sizeof(void*), PAGE_READWRITE, &oldProt)) {
                g_orig_Direct3DCreate9Ex = *g_d3dcreate9ex_iat_slot;
                *g_d3dcreate9ex_iat_slot = &Hook_Direct3DCreate9Ex;
                VirtualProtect(g_d3dcreate9ex_iat_slot, sizeof(void*), oldProt, &oldProt);
                Log("Installed IAT fallback hook on Direct3DCreate9Ex at %p (orig=%p)",
                    g_d3dcreate9ex_iat_slot, reinterpret_cast<void*>(g_orig_Direct3DCreate9Ex));
            }
        }
    } else {
        Log("ERROR: Could not find d3d9.dll!Direct3DCreate9Ex in soundvoltex.dll import table");
    }

    // ------------------------------------------------------------------------
    // 2. Dynamically locate & hook CreateGameWindow
    //    (SDVX6: RVA 0x58AFE0; SDVX7: RVA 0x5E9850)
    //    25-byte signature:
    //      40 55                push rbp
    //      53                   push rbx
    //      56                   push rsi
    //      57                   push rdi
    //      41 54                push r12
    //      41 55                push r13
    //      41 56                push r14
    //      41 57                push r15
    //      48 8D 6C 24 F9       lea rbp, [rsp-7]    ; <-- 18-byte boundary
    //      48 81 EC E8 00 00 00 sub rsp, 0xE8
    // ------------------------------------------------------------------------
    static const uint8_t kCreateGameWindowSig[25] = {
        0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
        0x48, 0x8D, 0x6C, 0x24, 0xF9, 0x48, 0x81, 0xEC, 0xE8, 0x00, 0x00, 0x00
    };
    uint8_t* wndFunc = ScanBytes(textStart, textSize, kCreateGameWindowSig, sizeof(kCreateGameWindowSig));
    if (!wndFunc) {
        // Fallback to 21-byte prefix if stack frame size (0xE8) changes in a future build
        wndFunc = ScanBytes(textStart, textSize, kCreateGameWindowSig, 21);
    }
    if (wndFunc) {
        constexpr size_t kStolenBytes = 18; // ends cleanly after `lea rbp, [rsp-7]`
        uint8_t* trampoline = stubPage + 64;  // 18 stolen bytes + 14-byte jump back
        uint8_t* relay      = stubPage + 128; // 14-byte jump to Hook_CreateGameWindow

        memcpy(trampoline, wndFunc, kStolenBytes);
        WriteAbsJump64(trampoline + kStolenBytes, wndFunc + kStolenBytes);
        orig_CreateGameWindow = reinterpret_cast<PFN_CreateGameWindow>(trampoline);

        WriteAbsJump64(relay, reinterpret_cast<const void*>(&Hook_CreateGameWindow));
        intptr_t rel = reinterpret_cast<intptr_t>(relay) - reinterpret_cast<intptr_t>(wndFunc + 5);
        if (rel >= INT32_MIN && rel <= INT32_MAX) {
            DWORD oldProt = 0;
            VirtualProtect(wndFunc, kStolenBytes, PAGE_EXECUTE_READWRITE, &oldProt);
            wndFunc[0] = 0xE9; // jmp rel32
            int32_t rel32 = static_cast<int32_t>(rel);
            memcpy(wndFunc + 1, &rel32, 4);
            for (size_t i = 5; i < kStolenBytes; ++i) wndFunc[i] = 0x90;
            VirtualProtect(wndFunc, kStolenBytes, oldProt, &oldProt);
            FlushInstructionCache(GetCurrentProcess(), wndFunc, kStolenBytes);
            Log("Hooked CreateGameWindow at %p (RVA 0x%llX, trampoline=%p)",
                wndFunc,
                (unsigned long long)(reinterpret_cast<uintptr_t>(wndFunc) - g_sdvx_base),
                trampoline);
        }
    } else {
        Log("WARNING: Could not locate CreateGameWindow pattern in soundvoltex.dll");
    }

    // ------------------------------------------------------------------------
    // 3. Dynamically locate & disable per-frame windowed BackBuffer0 downscale call
    //    (SDVX6: RVA 0x54A4FB; SDVX7: RVA 0x5A460B)
    //    Pattern around the check `if (w != 1080 || h != 1920) ReStretchBackBuffer0()`:
    //      81 FA 38 04 00 00       cmp edx, 1080
    //      75 09                   jne +9
    //      41 81 F8 80 07 00 00    cmp r8d, 1920
    //      74 0C                   je +12
    //      48 8B 8F 88 00 00 00    mov rcx, [rdi+0x88]
    //      E8 xx xx xx xx          call ReStretchBackBuffer0   <-- offset +24 (5 bytes)
    // ------------------------------------------------------------------------
    static const uint8_t kDownscaleSig[25] = {
        0x81, 0xFA, 0x38, 0x04, 0x00, 0x00,
        0x75, 0x09,
        0x41, 0x81, 0xF8, 0x80, 0x07, 0x00, 0x00,
        0x74, 0x0C,
        0x48, 0x8B, 0x8F, 0x88, 0x00, 0x00, 0x00,
        0xE8
    };
    uint8_t* downscaleMatch = ScanBytes(textStart, textSize, kDownscaleSig, sizeof(kDownscaleSig));
    if (downscaleMatch) {
        uint8_t* downscaleCall = downscaleMatch + 24;
        DWORD oldProt = 0;
        VirtualProtect(downscaleCall, 5, PAGE_EXECUTE_READWRITE, &oldProt);
        memset(downscaleCall, 0x90, 5);
        VirtualProtect(downscaleCall, 5, oldProt, &oldProt);
        FlushInstructionCache(GetCurrentProcess(), downscaleCall, 5);
        Log("Disabled windowed BackBuffer0 downscale call at %p (RVA 0x%llX)",
            downscaleCall,
            (unsigned long long)(reinterpret_cast<uintptr_t>(downscaleCall) - g_sdvx_base));
    } else {
        Log("WARNING: Could not locate BackBuffer0 downscale pattern in soundvoltex.dll");
    }

    // ------------------------------------------------------------------------
    // 4. Patch NvDisplayConfig MainDisplay resolution table in .rdata & .text
    //    (SDVX6: RVA 0x96A080..0x96A0DF; SDVX7: RVA 0x987FC0..0x98801F)
    //    During dll_entry_init in fullscreen mode, soundvoltex.dll calls NvDisplayConfig
    //    (NvAPI_DISP_SetDisplayConfig) to configure the DisplayPort MainDisplay using
    //    unrotated landscape panel dimensions (1920x1080) with 90° or 270° rotation
    //    (producing 1080x1920 portrait). Patching the MainDisplay entries to
    //    (g_target_h, g_target_w) (e.g. 2560x1440 -> 1440x2560 portrait) prevents
    //    NvDisplayConfig from downgrading the monitor to 1080p before InitD3D runs,
    //    while leaving the HDMI SubDisplay entry (1920x1080 @ 0°, 60Hz) untouched.
    // ------------------------------------------------------------------------
    if (g_patch_nv_display && rdataStart && rdataSize > 0) {
        const uint32_t landscapeW = static_cast<uint32_t>(g_target_h); // e.g. 2560
        const uint32_t landscapeH = static_cast<uint32_t>(g_target_w); // e.g. 1440

        // Block A: SubDisplay (1920, 1080, 0, 60000) + MainDisplay 60Hz (1920, 1080, 270, 60000)
        static const uint8_t kNvMode60Hz[32] = {
            0x80, 0x07, 0x00, 0x00, 0x38, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x60, 0xEA, 0x00, 0x00,
            0x80, 0x07, 0x00, 0x00, 0x38, 0x04, 0x00, 0x00, 0x0E, 0x01, 0x00, 0x00, 0x60, 0xEA, 0x00, 0x00
        };
        // Block B: MainDisplay 120Hz timing (1920, 1080, 0, 120000) + MainDisplay 120Hz DP (1920, 1080, 90, 120000)
        static const uint8_t kNvMode120Hz[32] = {
            0x80, 0x07, 0x00, 0x00, 0x38, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC0, 0xD4, 0x01, 0x00,
            0x80, 0x07, 0x00, 0x00, 0x38, 0x04, 0x00, 0x00, 0x5A, 0x00, 0x00, 0x00, 0xC0, 0xD4, 0x01, 0x00
        };

        uint8_t* match60 = ScanBytes(rdataStart, rdataSize, kNvMode60Hz, sizeof(kNvMode60Hz));
        if (match60) {
            DWORD oldProt = 0;
            // Patch only the second 16-byte entry (+16: MainDisplay 270° 60Hz); keep +0 (SubDisplay 0° 60Hz) at 1920x1080
            if (VirtualProtect(match60 + 16, 8, PAGE_READWRITE, &oldProt)) {
                memcpy(match60 + 16, &landscapeW, 4);
                memcpy(match60 + 20, &landscapeH, 4);
                VirtualProtect(match60 + 16, 8, oldProt, &oldProt);
                Log("Patched NvDisplayConfig 60Hz MainDisplay mode at %p (RVA 0x%llX) -> %ux%u (270 deg)",
                    match60 + 16,
                    (unsigned long long)(reinterpret_cast<uintptr_t>(match60 + 16) - g_sdvx_base),
                    landscapeW, landscapeH);
            }
        }

        uint8_t* match120 = ScanBytes(rdataStart, rdataSize, kNvMode120Hz, sizeof(kNvMode120Hz));
        if (match120) {
            DWORD oldProt = 0;
            // Patch both 16-byte entries (+0: 120Hz timing, +16: MainDisplay 90° 120Hz DP)
            if (VirtualProtect(match120, 32, PAGE_READWRITE, &oldProt)) {
                memcpy(match120 + 0,  &landscapeW, 4);
                memcpy(match120 + 4,  &landscapeH, 4);
                memcpy(match120 + 16, &landscapeW, 4);
                memcpy(match120 + 20, &landscapeH, 4);
                if (g_refresh_rate > 0) {
                    const uint32_t targetMilliHz = static_cast<uint32_t>(g_refresh_rate * 1000);
                    memcpy(match120 + 12, &targetMilliHz, 4);
                    memcpy(match120 + 28, &targetMilliHz, 4);
                }
                VirtualProtect(match120, 32, oldProt, &oldProt);
                Log("Patched NvDisplayConfig 120Hz MainDisplay modes at %p (RVA 0x%llX) -> %ux%u (90 deg @ %uHz)",
                    match120,
                    (unsigned long long)(reinterpret_cast<uintptr_t>(match120) - g_sdvx_base),
                    landscapeW, landscapeH,
                    g_refresh_rate > 0 ? g_refresh_rate : 120U);
            }
        }

        // Also patch the two hardcoded 0x780 (1920) width immediates in NvDisplayConfig's custom timing path
        //   C7 45 A4 80 07 00 00 66 48 0F 7E F0  -> mov dword ptr [rbp-0x5c], 1920
        //   C7 45 84 80 07 00 00 89 45 88        -> mov dword ptr [rbp-0x7c], 1920
        static const uint8_t kNvTimingW1[12] = {
            0xC7, 0x45, 0xA4, 0x80, 0x07, 0x00, 0x00, 0x66, 0x48, 0x0F, 0x7E, 0xF0
        };
        static const uint8_t kNvTimingW2[10] = {
            0xC7, 0x45, 0x84, 0x80, 0x07, 0x00, 0x00, 0x89, 0x45, 0x88
        };
        uint8_t* tw1 = ScanBytes(textStart, textSize, kNvTimingW1, sizeof(kNvTimingW1));
        if (tw1) {
            DWORD oldProt = 0;
            if (VirtualProtect(tw1 + 3, 4, PAGE_EXECUTE_READWRITE, &oldProt)) {
                memcpy(tw1 + 3, &landscapeW, 4);
                VirtualProtect(tw1 + 3, 4, oldProt, &oldProt);
                FlushInstructionCache(GetCurrentProcess(), tw1 + 3, 4);
            }
        }
        uint8_t* tw2 = ScanBytes(textStart, textSize, kNvTimingW2, sizeof(kNvTimingW2));
        if (tw2) {
            DWORD oldProt = 0;
            if (VirtualProtect(tw2 + 3, 4, PAGE_EXECUTE_READWRITE, &oldProt)) {
                memcpy(tw2 + 3, &landscapeW, 4);
                VirtualProtect(tw2 + 3, 4, oldProt, &oldProt);
                FlushInstructionCache(GetCurrentProcess(), tw2 + 3, 4);
            }
        }
    }

    // ------------------------------------------------------------------------
    // 5. Refresh Rate, VSync, FrameLimiter & Note FPS Target Hooks/Patches
    //    Compatible with both SDVX6 and SDVX7+ (references Bemanipatcher & sp2xpatcher)
    // ------------------------------------------------------------------------
    const UINT effectiveNoteFps = (g_note_fps > 0) ? g_note_fps : g_refresh_rate;

    // 5a. Startup NvDisplayConfig Monitor Change & Valkyrie 60Hz Check in dll_entry_init
    //     (SDVX6: RVA 0x35F894; SDVX7: RVA 0x3E630F)
    //     Pattern: 85 C0 75 2C E8 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ?? 84 C0 74 05 E8
    //       +0x02: 75 2C (jne +0x2C -> EB 2C skips NvDisplayConfig completely)
    //       +0x29: 74 05 (je +5 -> 75 05 forces 60Hz NvDisplayConfig(0) in Valkyrie mode)
    if (g_disable_monitor_change || g_refresh_rate == 60) {
        static const uint8_t kMonChangeSig[22] = {
            0x85, 0xC0, 0x75, 0x2C,
            0xE8, 0x00, 0x00, 0x00, 0x00,
            0x48, 0x8B, 0xC8,
            0xE8, 0x00, 0x00, 0x00, 0x00,
            0x84, 0xC0, 0x74, 0x05, 0xE8
        };
        static const char kMonChangeMask[] = "xxxxx????xxxx????xxxxx";
        uint8_t* monMatch = ScanPattern(textStart, textSize, kMonChangeSig, kMonChangeMask);
        if (monMatch) {
            if (g_disable_monitor_change) {
                DWORD oldProt = 0;
                if (VirtualProtect(monMatch + 2, 2, PAGE_EXECUTE_READWRITE, &oldProt)) {
                    monMatch[2] = 0xEB; // jmp +0x2C
                    VirtualProtect(monMatch + 2, 2, oldProt, &oldProt);
                    FlushInstructionCache(GetCurrentProcess(), monMatch + 2, 2);
                    Log("Disabled startup NvDisplayConfig monitor change at %p (RVA 0x%llX)",
                        monMatch + 2,
                        (unsigned long long)(reinterpret_cast<uintptr_t>(monMatch + 2) - g_sdvx_base));
                }
            } else if (g_refresh_rate == 60 && monMatch[0x29] == 0x74 && monMatch[0x2A] == 0x05) {
                DWORD oldProt = 0;
                if (VirtualProtect(monMatch + 0x29, 2, PAGE_EXECUTE_READWRITE, &oldProt)) {
                    monMatch[0x29] = 0x75; // jne +5 (Valkyrie Mode 60Hz #1)
                    VirtualProtect(monMatch + 0x29, 2, oldProt, &oldProt);
                    FlushInstructionCache(GetCurrentProcess(), monMatch + 0x29, 2);
                    Log("Patched startup NvDisplayConfig Valkyrie check to 60Hz at %p (RVA 0x%llX)",
                        monMatch + 0x29,
                        (unsigned long long)(reinterpret_cast<uintptr_t>(monMatch + 0x29) - g_sdvx_base));
                }
            }
        } else {
            Log("WARNING: Could not locate startup NvDisplayConfig check pattern in soundvoltex.dll");
        }
    }

    // 5b. InitD3D Candidate Mode Table (Adapter 0 first-priority refresh rate)
    //     (SDVX6: RVA 0x5467F6, imm32 at +26 = 0x546810;
    //      SDVX7: RVA 0x5A0B66, imm32 at +26 = 0x5A0B80)
    //     mov dword ptr [rbp+0x140], 1080; mov dword ptr [rbp+0x144], 1920; mov dword ptr [rbp+0x148], 120
    if (g_refresh_rate > 0) {
        static const uint8_t kCandidateMode120Sig[30] = {
            0xC7, 0x85, 0x40, 0x01, 0x00, 0x00, 0x38, 0x04, 0x00, 0x00,
            0xC7, 0x85, 0x44, 0x01, 0x00, 0x00, 0x80, 0x07, 0x00, 0x00,
            0xC7, 0x85, 0x48, 0x01, 0x00, 0x00, 0x78, 0x00, 0x00, 0x00
        };
        uint8_t* candMatch = ScanBytes(textStart, textSize, kCandidateMode120Sig, sizeof(kCandidateMode120Sig));
        if (candMatch) {
            DWORD oldProt = 0;
            uint32_t targetHz = static_cast<uint32_t>(g_refresh_rate);
            if (VirtualProtect(candMatch + 26, 4, PAGE_EXECUTE_READWRITE, &oldProt)) {
                memcpy(candMatch + 26, &targetHz, 4);
                VirtualProtect(candMatch + 26, 4, oldProt, &oldProt);
                FlushInstructionCache(GetCurrentProcess(), candMatch + 26, 4);
                Log("Patched InitD3D priority candidate refresh rate at %p (RVA 0x%llX): 120Hz -> %uHz",
                    candMatch + 26,
                    (unsigned long long)(reinterpret_cast<uintptr_t>(candMatch + 26) - g_sdvx_base),
                    targetHz);
            }
        } else {
            Log("WARNING: Could not locate InitD3D candidate mode table in soundvoltex.dll");
        }
    }

    // 5c. InitD3D Refresh Rate Verification & VSync Check Bypass
    //     (SDVX6: RVA 0x546CD9; SDVX7: RVA 0x5A1049)
    //     When InitD3D sees a refresh rate != 120Hz (Valkyrie) or != 60Hz (Standard),
    //     it logs "Monitor refresh rate does not match!", disables VSync, and skips
    //     calling GetMainDisplayRefreshRate(). Bypassing the mismatch branch allows
    //     any custom refresh rate (60, 120, 144, 165, 240, 360Hz, etc.) to succeed
    //     with VSync intact and always invokes GetMainDisplayRefreshRate().
    if (g_refresh_rate > 0 || effectiveNoteFps > 0) {
        static const uint8_t kRefreshVerifySig[11] = {
            0x84, 0xC0,                         // test al, al
            0x74, 0x07,                         // je +7       (+2)
            0x83, 0x7C, 0x24, 0x54, 0x78,       // cmp dword ptr [rsp+0x54], 120 (+4..+8)
            0x75, 0x18                          // jne mismatch (+9..+10)
        };
        uint8_t* verifyMatch = ScanBytes(textStart, textSize, kRefreshVerifySig, sizeof(kRefreshVerifySig));
        if (verifyMatch) {
            DWORD oldProt = 0;
            if (VirtualProtect(verifyMatch, 36, PAGE_EXECUTE_READWRITE, &oldProt)) {
                // NOP out `jne mismatch` at +9 (75 18 -> 90 90)
                verifyMatch[9]  = 0x90;
                verifyMatch[10] = 0x90;
                // Change `je ok_block` at +33 (74 37 -> EB 37) to unconditional jmp to ok_block
                if (verifyMatch[33] == 0x74 && verifyMatch[34] == 0x37) {
                    verifyMatch[33] = 0xEB;
                }
                VirtualProtect(verifyMatch, 36, oldProt, &oldProt);
                FlushInstructionCache(GetCurrentProcess(), verifyMatch, 36);
                Log("Patched InitD3D refresh rate verification check at %p (RVA 0x%llX) to accept custom Hz",
                    verifyMatch,
                    (unsigned long long)(reinterpret_cast<uintptr_t>(verifyMatch) - g_sdvx_base));
            }
        } else {
            Log("WARNING: Could not locate InitD3D refresh rate verification check in soundvoltex.dll");
        }
    }

    // 5d. InitD3D Software FrameLimiter Target (mHz)
    //     (SDVX6: RVA 0x5475F9; SDVX7: RVA 0x5A1969)
    //     Pattern: BF 60 EA 00 00 84 C0 74 31 E8 ?? ?? ?? ?? BB C0 D4 01 00
    //       +1 : uint32_t 60000  (mov edi, 60000)
    //       +15: uint32_t 120000 (mov ebx, 120000)
    if (g_refresh_rate > 0) {
        static const uint8_t kFrameLimiterSig[19] = {
            0xBF, 0x60, 0xEA, 0x00, 0x00,       // mov edi, 60000
            0x84, 0xC0,                         // test al, al
            0x74, 0x31,                         // je +0x31
            0xE8, 0x00, 0x00, 0x00, 0x00,       // call GetFrameLimiter
            0xBB, 0xC0, 0xD4, 0x01, 0x00        // mov ebx, 120000
        };
        static const char kFrameLimiterMask[] = "xxxxxxxxxx????xxxxx";
        uint8_t* limiterMatch = ScanPattern(textStart, textSize, kFrameLimiterSig, kFrameLimiterMask);
        if (limiterMatch) {
            DWORD oldProt = 0;
            uint32_t targetMilliHz = static_cast<uint32_t>(g_refresh_rate * 1000);
            if (VirtualProtect(limiterMatch, 19, PAGE_EXECUTE_READWRITE, &oldProt)) {
                memcpy(limiterMatch + 1,  &targetMilliHz, 4);
                memcpy(limiterMatch + 15, &targetMilliHz, 4);
                VirtualProtect(limiterMatch, 19, oldProt, &oldProt);
                FlushInstructionCache(GetCurrentProcess(), limiterMatch, 19);
                Log("Patched InitD3D FrameLimiter target at %p (RVA 0x%llX) -> %u mHz (%u Hz)",
                    limiterMatch,
                    (unsigned long long)(reinterpret_cast<uintptr_t>(limiterMatch) - g_sdvx_base),
                    targetMilliHz, g_refresh_rate);
            }
        } else {
            Log("WARNING: Could not locate InitD3D FrameLimiter pattern in soundvoltex.dll");
        }
    }

    // 5e. GetMainDisplayRefreshRate (Note FPS Target & Game FPS Target)
    //     (SDVX6: RVA 0x35EE78; SDVX7: RVA 0x3E58E8)
    //     1) +0x00 (16 bytes): `movd xmm6, eax; cvtdq2pd xmm6, xmm6; mulsd xmm6, [rip+rel32]`
    //        -> `mov eax, effectiveNoteFps; cvtsi2sd xmm6, eax; nop...`
    //     2) +0x7A (2 bytes) : `jbe dummy_60hz` (`76 29`) -> `90 90` ("Force Note FPS Target")
    //     3) +0xA5 & +0xC6   : Redirect the two `movsd xmm2/xmm0, [rip+rel32]` instructions in the
    //        DUMMY fallback path (used when NvAPI fails or on AMD/Intel GPUs) to read
    //        `double(effectiveNoteFps)` from `stubPage + 256` without altering the shared 60.0
    //        seconds-per-minute constant in .rdata.
    if (effectiveNoteFps > 0) {
        static const uint8_t kNoteFpsSig[12] = {
            0x66, 0x0F, 0x6E, 0xF0,             // movd xmm6, eax
            0xF3, 0x0F, 0xE6, 0xF6,             // cvtdq2pd xmm6, xmm6
            0xF2, 0x0F, 0x59, 0x35              // mulsd xmm6, qword ptr [rip + rel32]
        };
        uint8_t* noteMatch = ScanBytes(textStart, textSize, kNoteFpsSig, sizeof(kNoteFpsSig));
        if (noteMatch) {
            // Store IEEE-754 double constant in stubPage + 256
            double* pCustomFpsDouble = reinterpret_cast<double*>(stubPage + 256);
            *pCustomFpsDouble = static_cast<double>(effectiveNoteFps);

            DWORD oldProt = 0;
            if (VirtualProtect(noteMatch, 0xD0, PAGE_EXECUTE_READWRITE, &oldProt)) {
                // (1) Patch +0x00 (16 bytes) -> mov eax, imm32; cvtsi2sd xmm6, eax; 7-byte nop
                uint8_t notePatch[16] = {
                    0xB8, 0x00, 0x00, 0x00, 0x00,
                    0xF2, 0x0F, 0x2A, 0xF0,
                    0x66, 0x90, 0x66, 0x90, 0x66, 0x90, 0x90
                };
                uint32_t fpsImm = static_cast<uint32_t>(effectiveNoteFps);
                memcpy(notePatch + 1, &fpsImm, 4);
                memcpy(noteMatch, notePatch, 16);

                // (2) Patch +0x7A (`76 29` jbe dummy) -> `90 90` (Force Note FPS Target)
                if (noteMatch[0x76] == 0x66 && noteMatch[0x77] == 0x0F &&
                    noteMatch[0x78] == 0x2F && noteMatch[0x79] == 0xD1 &&
                    noteMatch[0x7A] == 0x76)
                {
                    noteMatch[0x7A] = 0x90;
                    noteMatch[0x7B] = 0x90;
                }

                // (3) Redirect DUMMY fallback `movsd xmm2, [rip+rel32]` (+0xA5) & `movsd xmm0, [rip+rel32]` (+0xC6)
                if (noteMatch[0xA5] == 0xF2 && noteMatch[0xA6] == 0x0F &&
                    noteMatch[0xA7] == 0x10 && noteMatch[0xA8] == 0x15)
                {
                    intptr_t rel = reinterpret_cast<intptr_t>(pCustomFpsDouble) -
                                   reinterpret_cast<intptr_t>(noteMatch + 0xA5 + 8);
                    if (rel >= INT32_MIN && rel <= INT32_MAX) {
                        int32_t rel32 = static_cast<int32_t>(rel);
                        memcpy(noteMatch + 0xA5 + 4, &rel32, 4);
                    }
                }
                if (noteMatch[0xC6] == 0xF2 && noteMatch[0xC7] == 0x0F &&
                    noteMatch[0xC8] == 0x10 && noteMatch[0xC9] == 0x05)
                {
                    intptr_t rel = reinterpret_cast<intptr_t>(pCustomFpsDouble) -
                                   reinterpret_cast<intptr_t>(noteMatch + 0xC6 + 8);
                    if (rel >= INT32_MIN && rel <= INT32_MAX) {
                        int32_t rel32 = static_cast<int32_t>(rel);
                        memcpy(noteMatch + 0xC6 + 4, &rel32, 4);
                    }
                }

                VirtualProtect(noteMatch, 0xD0, oldProt, &oldProt);
                FlushInstructionCache(GetCurrentProcess(), noteMatch, 0xD0);
                Log("Patched GetMainDisplayRefreshRate Note/Game FPS target at %p (RVA 0x%llX) -> %u FPS (%.2f Hz)",
                    noteMatch,
                    (unsigned long long)(reinterpret_cast<uintptr_t>(noteMatch) - g_sdvx_base),
                    effectiveNoteFps, *pCustomFpsDouble);
            }
        } else {
            Log("WARNING: Could not locate GetMainDisplayRefreshRate Note FPS pattern in soundvoltex.dll");
        }
    }

    g_sdvx_hooked = true;
    return true;
}

// ----------------------------------------------------------------------------
// Configuration Loader (res_hook.ini)
// ----------------------------------------------------------------------------
static void LoadConfig() {
    char dllPath[MAX_PATH] = {0};
    GetModuleFileNameA(g_hSelfModule, dllPath, MAX_PATH);

    char* lastSlash = strrchr(dllPath, '\\');
    char dirPath[MAX_PATH] = {0};
    if (lastSlash) {
        size_t len = static_cast<size_t>(lastSlash - dllPath + 1);
        memcpy(dirPath, dllPath, len);
        dirPath[len] = '\0';
    }

    char iniPath[MAX_PATH] = {0};
    snprintf(iniPath, sizeof(iniPath), "%sres_hook.ini", dirPath);
    snprintf(g_log_path, sizeof(g_log_path), "%sres_hook.log", dirPath);

    g_enabled                = GetPrivateProfileIntA("Resolution", "Enabled",              1,    iniPath) != 0;
    g_target_w               = GetPrivateProfileIntA("Resolution", "Width",                1440, iniPath);
    g_target_h               = GetPrivateProfileIntA("Resolution", "Height",               2560, iniPath);
    g_auto_portrait          = GetPrivateProfileIntA("Resolution", "AutoPortrait",         1,    iniPath) != 0;
    g_resize_main_window     = GetPrivateProfileIntA("Resolution", "ResizeMainWindow",     1,    iniPath) != 0;
    g_window_w               = GetPrivateProfileIntA("Resolution", "WindowWidth",          0,    iniPath);
    g_window_h               = GetPrivateProfileIntA("Resolution", "WindowHeight",         0,    iniPath);
    g_patch_nv_display       = GetPrivateProfileIntA("Resolution", "PatchNvDisplayConfig", 1,    iniPath) != 0;
    g_refresh_rate           = GetPrivateProfileIntA("Resolution", "RefreshRate",          0,    iniPath);
    g_note_fps               = GetPrivateProfileIntA("Resolution", "NoteFpsTarget",        0,    iniPath);
    g_disable_monitor_change = GetPrivateProfileIntA("Resolution", "DisableMonitorChange", 0,    iniPath) != 0;
    g_log_enabled            = GetPrivateProfileIntA("Resolution", "Log",                  1,    iniPath) != 0;

    if (g_auto_portrait && g_target_w > g_target_h) {
        UINT tmp   = g_target_w;
        g_target_w = g_target_h;
        g_target_h = tmp;
    }
    if (g_target_w < 100) g_target_w = NATIVE_MAIN_W;
    if (g_target_h < 100) g_target_h = NATIVE_MAIN_H;

    if (g_auto_portrait && g_window_w > g_window_h && g_window_h > 0) {
        UINT tmp   = g_window_w;
        g_window_w = g_window_h;
        g_window_h = tmp;
    }
    if (g_window_w == 0) g_window_w = g_target_w;
    if (g_window_h == 0) g_window_h = g_target_h;

    g_scale_x = static_cast<float>(g_target_w) / static_cast<float>(NATIVE_MAIN_W);
    g_scale_y = static_cast<float>(g_target_h) / static_cast<float>(NATIVE_MAIN_H);

    if (g_log_enabled) {
        FILE* fp = fopen(g_log_path, "w");
        if (fp) fclose(fp);
    }

    const UINT effectiveNoteFps = (g_note_fps > 0) ? g_note_fps : g_refresh_rate;
    Log("================================================================");
    Log("SOUND VOLTEX EXCEED GEAR - Main Screen Resolution & Refresh Hook");
    Log("Config file      : %s", iniPath);
    Log("Enabled          : %d", (int)g_enabled);
    Log("Main Render Res  : %ux%u (native %ux%u, scale %.4fx%.4f)",
        g_target_w, g_target_h, NATIVE_MAIN_W, NATIVE_MAIN_H, g_scale_x, g_scale_y);
    Log("Main Window Res  : %ux%u (ResizeMainWindow=%d, PatchNvDisplayConfig=%d)",
        g_window_w, g_window_h, (int)g_resize_main_window, (int)g_patch_nv_display);
    Log("Refresh / FPS    : RefreshRate=%uHz, NoteFpsTarget=%u, DisableMonitorChange=%d",
        g_refresh_rate, effectiveNoteFps, (int)g_disable_monitor_change);
    Log("Sub Screen Res   : 1920x1080 (untouched)");
    Log("================================================================");
}

// Background watcher thread in case DLL is injected early (-ke or proxy DLL)
// before soundvoltex.dll is loaded into memory.
static DWORD WINAPI WatcherThread(LPVOID) {
    for (int i = 0; i < 3000; ++i) { // up to 30 seconds
        if (InstallSdvxPatches()) {
            return 0;
        }
        Sleep(10);
    }
    Log("ERROR: Timed out waiting for soundvoltex.dll to load");
    return 0;
}

// ----------------------------------------------------------------------------
// Optional VERSION.dll Proxy Exports (allows renaming res_hook.dll -> version.dll
// for automatic loading without needing -k command-line arguments)
// ----------------------------------------------------------------------------
static HMODULE GetSystemVersionDll() {
    static HMODULE hVer = NULL;
    if (!hVer) {
        char sysDir[MAX_PATH] = {0};
        GetSystemDirectoryA(sysDir, MAX_PATH);
        char fullPath[MAX_PATH] = {0};
        snprintf(fullPath, sizeof(fullPath), "%s\\version.dll", sysDir);
        hVer = LoadLibraryA(fullPath);
    }
    return hVer;
}

extern "C" {

__declspec(dllexport) BOOL WINAPI GetFileVersionInfoA(LPCSTR lptstrFilename, DWORD dwHandle, DWORD dwLen, LPVOID lpData) {
    typedef BOOL (WINAPI *PFN)(LPCSTR, DWORD, DWORD, LPVOID);
    static PFN fn = reinterpret_cast<PFN>(reinterpret_cast<void*>(GetProcAddress(GetSystemVersionDll(), "GetFileVersionInfoA")));
    return fn ? fn(lptstrFilename, dwHandle, dwLen, lpData) : FALSE;
}

__declspec(dllexport) DWORD WINAPI GetFileVersionInfoSizeA(LPCSTR lptstrFilename, LPDWORD lpdwHandle) {
    typedef DWORD (WINAPI *PFN)(LPCSTR, LPDWORD);
    static PFN fn = reinterpret_cast<PFN>(reinterpret_cast<void*>(GetProcAddress(GetSystemVersionDll(), "GetFileVersionInfoSizeA")));
    return fn ? fn(lptstrFilename, lpdwHandle) : 0;
}

__declspec(dllexport) DWORD WINAPI GetFileVersionInfoSizeW(LPCWSTR lptstrFilename, LPDWORD lpdwHandle) {
    typedef DWORD (WINAPI *PFN)(LPCWSTR, LPDWORD);
    static PFN fn = reinterpret_cast<PFN>(reinterpret_cast<void*>(GetProcAddress(GetSystemVersionDll(), "GetFileVersionInfoSizeW")));
    return fn ? fn(lptstrFilename, lpdwHandle) : 0;
}

__declspec(dllexport) BOOL WINAPI GetFileVersionInfoW(LPCWSTR lptstrFilename, DWORD dwHandle, DWORD dwLen, LPVOID lpData) {
    typedef BOOL (WINAPI *PFN)(LPCWSTR, DWORD, DWORD, LPVOID);
    static PFN fn = reinterpret_cast<PFN>(reinterpret_cast<void*>(GetProcAddress(GetSystemVersionDll(), "GetFileVersionInfoW")));
    return fn ? fn(lptstrFilename, dwHandle, dwLen, lpData) : FALSE;
}

__declspec(dllexport) BOOL WINAPI VerQueryValueA(LPCVOID pBlock, LPCSTR lpSubBlock, LPVOID* lplpBuffer, PUINT puLen) {
    typedef BOOL (WINAPI *PFN)(LPCVOID, LPCSTR, LPVOID*, PUINT);
    static PFN fn = reinterpret_cast<PFN>(reinterpret_cast<void*>(GetProcAddress(GetSystemVersionDll(), "VerQueryValueA")));
    return fn ? fn(pBlock, lpSubBlock, lplpBuffer, puLen) : FALSE;
}

__declspec(dllexport) BOOL WINAPI VerQueryValueW(LPCVOID pBlock, LPCWSTR lpSubBlock, LPVOID* lplpBuffer, PUINT puLen) {
    typedef BOOL (WINAPI *PFN)(LPCVOID, LPCWSTR, LPVOID*, PUINT);
    static PFN fn = reinterpret_cast<PFN>(reinterpret_cast<void*>(GetProcAddress(GetSystemVersionDll(), "VerQueryValueW")));
    return fn ? fn(pBlock, lpSubBlock, lplpBuffer, puLen) : FALSE;
}

// ----------------------------------------------------------------------------
// DLL Entry Point
// ----------------------------------------------------------------------------
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        g_hSelfModule = hModule;
        DisableThreadLibraryCalls(hModule);
        LoadConfig();
        if (g_enabled) {
            if (!InstallSdvxPatches()) {
                HANDLE hThread = CreateThread(NULL, 0, WatcherThread, NULL, 0, NULL);
                if (hThread) CloseHandle(hThread);
            }
        }
    }
    return TRUE;
}

} // extern "C"
