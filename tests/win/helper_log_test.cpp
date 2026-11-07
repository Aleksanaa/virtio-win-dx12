// SPDX-License-Identifier: MIT
#include <windows.h>
#include <cstdio>
#include <fstream>
#include <string>
#include "log.h"

int wmain()
{
    WCHAR temp[MAX_PATH], directory[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, temp) || !GetTempFileNameW(temp, L"vgd", 0, directory))
        return 1;
    DeleteFileW(directory);
    if (!CreateDirectoryW(directory, nullptr) || !SetCurrentDirectoryW(directory))
        return 2;
    PrintMessage(L"progress=%u%% text=%ls\n", 100U, L"literal %% and %08x");
    PrintMessage(L"size=%ux%u\n", 1280U, 800U);
    std::ifstream file("vgpusvp.log");
    std::string text((std::istreambuf_iterator<char>(file)), {});
    file.close();
    const bool good = text == "progress=100% text=literal %% and %08x\nsize=1280x800\n";
    if (!good)
        std::fprintf(stderr, "Unexpected helper log: %s\n", text.c_str());
    DeleteFileW(L"vgpusvp.log");
    SetCurrentDirectoryW(temp);
    RemoveDirectoryW(directory);
    if (!good) return 3;
    std::puts("PASS formatted helper messages and literal percent sequences");
    return 0;
}
