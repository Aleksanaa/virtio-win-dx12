// SPDX-License-Identifier: MIT
// The panel's tests: groups of tests (one per API) that each run on the VirtIO GPU as an application would. A test is
// a function that prints what it does and returns whether it passed; the panel runs each one in a child process
// (`venera-panel.exe /run-test <group>.<test>`, runner.cpp), so one that crashes or hangs takes only itself down.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

struct IDXGIAdapter1;

namespace panel {

struct Test {
    const char* id;  // within its group: the command line names it "<group>.<id>"
    const wchar_t* name;
    const wchar_t* desc;
    bool (*run)();
};
struct TestGroup {
    const char* id;
    const wchar_t* name;
    const wchar_t* desc;
    std::vector<Test> tests;
};

TestGroup d3d12Tests();
TestGroup d3d11Tests();
TestGroup d3d9Tests();
TestGroup vulkanTests();
// All of them, in the order the panel lists them.
const std::vector<TestGroup>& testGroups();

// `/debug`: D3D12 tests turn the debug layer on and print the runtime's messages.
extern bool testDebugLayer;

// The VirtIO GPU among the DXGI adapters, or null (probe.cpp).
IDXGIAdapter1* findAdapter();

// The process d3d11's shared-resource tests start to open their texture: `venera-panel.exe /shared-child <handle>`.
int runD3D11SharedChild(const wchar_t* handle);

}  // namespace panel
