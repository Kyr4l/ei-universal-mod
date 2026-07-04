#include <windows.h>
#include <string>

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call != DLL_PROCESS_ATTACH) {
        return TRUE;
    }

    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);

    std::string gameDir(exePath);
    size_t lastSlash = gameDir.find_last_of('\\');
    if (lastSlash != std::string::npos) {
        gameDir.resize(lastSlash);
    }

    std::string asiPath = gameDir + "\\SpellAddonX.asi";
    if (GetFileAttributesA(asiPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxA(NULL,
            "SpellAddonX.asi is missing and required to run the game.",
            "Missing Required File",
            MB_ICONERROR | MB_OK);
        ExitProcess(1);
    }

    return TRUE;
}
