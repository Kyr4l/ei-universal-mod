// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// d7vk-check.exe <d7vk.dll> [result file]: whether D7VK can run the game here. EIStarter-UM runs it before Play when the renderer is "Auto".
// It does what the game does at start, in a 32-bit process like the game (a PC can have 64-bit Vulkan drivers and no 32-bit ones): loads the
// file, creates a DirectDraw 7 object, sets the cooperative level on a hidden window, gets Direct3D 7, creates a small 3D surface and a
// hardware device on it. Exit code 0 = it works; 1..8 = the step that failed. The result file gets one line saying what happened.
#include <windows.h>
#include <ddraw.h>
#include <d3d.h>

#include <cstdio>

static const char* g_resultFile = nullptr;

static int Result(int code, const char* text, HRESULT hr = S_OK) {
    if (g_resultFile)
        if (FILE* f = std::fopen(g_resultFile, "w")) {
            if (hr != S_OK) std::fprintf(f, "%s (0x%08lX)\n", text, static_cast<unsigned long>(hr));
            else std::fprintf(f, "%s\n", text);
            std::fclose(f);
        }
    return code;
}

template <class T> static void Release(T*& p) { if (p) { p->Release(); p = nullptr; } }

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv || argc < 2) return Result(1, "usage: d7vk-check.exe <d7vk.dll> [result file]");
    static char resultPath[MAX_PATH];
    if (argc >= 3 && WideCharToMultiByte(CP_ACP, 0, argv[2], -1, resultPath, MAX_PATH, NULL, NULL) > 0) g_resultFile = resultPath;

    HMODULE module = LoadLibraryExW(argv[1], NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) return Result(2, "d7vk.dll could not be loaded", HRESULT_FROM_WIN32(GetLastError()));
    typedef HRESULT (WINAPI *CreateEx)(GUID*, void**, REFIID, IUnknown*);
    CreateEx create = reinterpret_cast<CreateEx>(reinterpret_cast<void*>(GetProcAddress(module, "DirectDrawCreateEx")));
    if (!create) return Result(3, "d7vk.dll has no DirectDrawCreateEx");

    WNDCLASSA wc = {};
    wc.lpfnWndProc = DefWindowProcA; wc.hInstance = instance; wc.lpszClassName = "d7vk-check";
    RegisterClassA(&wc);
    HWND window = CreateWindowExA(0, "d7vk-check", "d7vk-check", WS_POPUP, 0, 0, 64, 64, NULL, NULL, instance, NULL); // never shown

    IDirectDraw7* dd = nullptr; IDirect3D7* d3d = nullptr; IDirectDrawSurface7* surface = nullptr; IDirect3DDevice7* device = nullptr;
    HRESULT hr = create(NULL, reinterpret_cast<void**>(&dd), IID_IDirectDraw7, NULL);
    int code = 0;
    if (FAILED(hr) || !dd) code = Result(4, "no DirectDraw object (no Vulkan device?)", hr);
    else if (FAILED(hr = dd->SetCooperativeLevel(window, DDSCL_NORMAL))) code = Result(5, "SetCooperativeLevel failed", hr);
    else if (FAILED(hr = dd->QueryInterface(IID_IDirect3D7, reinterpret_cast<void**>(&d3d)))) code = Result(6, "no Direct3D 7", hr);
    else {
        DDSURFACEDESC2 sd = {};
        sd.dwSize = sizeof sd;
        sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
        sd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | DDSCAPS_VIDEOMEMORY;
        sd.dwWidth = 64; sd.dwHeight = 64;
        if (FAILED(hr = dd->CreateSurface(&sd, &surface, NULL))) code = Result(7, "no 3D surface", hr);
        else if (FAILED(hr = d3d->CreateDevice(IID_IDirect3DTnLHalDevice, surface, &device)) && FAILED(hr = d3d->CreateDevice(IID_IDirect3DHALDevice, surface, &device)))
            code = Result(8, "no hardware 3D device", hr);
        else code = Result(0, "OK");
    }
    Release(device); Release(surface); Release(d3d); Release(dd);
    if (window) DestroyWindow(window);
    return code;
}
