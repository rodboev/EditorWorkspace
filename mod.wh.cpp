// ==WindhawkMod==
// @id              ep-taskbar-button-width
// @name            ExplorerPatcher Taskbar Button Width
// @description     Customize the minimum width of taskbar buttons when using ExplorerPatcher's Windows 10 taskbar
// @version         1.1.1
// @author          Rod Boev
// @github          https://github.com/rodboev
// @include         explorer.exe
// @architecture    x86-64
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# ExplorerPatcher Taskbar Button Width

This mod lets you change the width of taskbar items when using ExplorerPatcher's Windows 10 taskbar on Windows 11.

Before:

![Before](https://i.imgur.com/r82ISo3.png)

After (120%):

![After](https://i.imgur.com/qHaRdVt.png)

Enter a percentage where `100%` maps to `160px` before DPI scaling.

(The registry hack at
`HKCU\Control Panel\Desktop\WindowMetrics\MinWidth` doesn't work with
ExplorerPatcher.)

**Requirements:** Windows 11 with ExplorerPatcher, using the Windows 10 taskbar.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- buttonWidthPercent: 120
  $name: Button width (%)
  $description: "Default: 120%. 100% = 160px before DPI scaling"
*/
// ==/WindhawkModSettings==

#include <psapi.h>
#include <atomic>

std::atomic<int> g_settingsPercent{120};
std::atomic<int> g_logicalWidthPx{0};

std::atomic<bool> g_hooked{false};
std::atomic<int> g_normLogCount{0};
std::atomic<int> g_rowsLogCount{0};
std::atomic<int> g_rowLayoutLogCount{0};
std::atomic<int> g_recomputeLayoutLogCount{0};

thread_local int g_recomputeLayoutDepth = 0;
thread_local int g_recomputeLayoutRowCalls = 0;
thread_local int g_recomputeLayoutNormCalls = 0;
thread_local int g_recomputeLayoutRowsCalls = 0;
thread_local int g_layoutRequiredRows = 1;
thread_local int g_layoutRowCallIndex = 0;

using ComputeSingleButtonWidth_t = int(WINAPI*)(void* pThis, int groupType, void* pTaskBtnGroup, int* pWidth);
ComputeSingleButtonWidth_t ComputeSingleButtonWidth_Original;

using GetNormalizedButtonWidth_t = int(WINAPI*)(void* pThis, int groupType, int index);
GetNormalizedButtonWidth_t GetNormalizedButtonWidth_Original;

using GetRequiredRows_t = int(WINAPI*)(void* pThis, int availableSpace, int height);
GetRequiredRows_t GetRequiredRows_Original;

using RecomputeLayoutRow_t =
    int(WINAPI*)(void* pThis, int a1, int startIndex, int a3, int a4, int endIndex, RECT* rowRect);
RecomputeLayoutRow_t RecomputeLayoutRow_Original;

using RecomputeLayout_t = int(WINAPI*)(void* pThis);
RecomputeLayout_t RecomputeLayout_Original;

using LoadLibraryExW_t = decltype(&LoadLibraryExW);
LoadLibraryExW_t LoadLibraryExW_Original;

int GetTaskbarDpi() {
    HWND taskbar = FindWindow(L"Shell_TrayWnd", nullptr);
    if (taskbar) {
        UINT dpi = GetDpiForWindow(taskbar);
        if (dpi != 0) {
            return (int)dpi;
        }
    }

    return USER_DEFAULT_SCREEN_DPI;
}

int ScalePixelsForDpi(int pixels) {
    return MulDiv(pixels, GetTaskbarDpi(), USER_DEFAULT_SCREEN_DPI);
}

bool ShouldLogSample(std::atomic<int>& counter, int limit = 16) {
    return counter.fetch_add(1) < limit;
}

unsigned int PtrTail(const void* p) {
    return (unsigned int)((ULONG_PTR)p & 0xFFFF);
}

void ResetDebugLogCounters() {
    g_normLogCount.store(0);
    g_rowsLogCount.store(0);
    g_rowLayoutLogCount.store(0);
    g_recomputeLayoutLogCount.store(0);
}

int GetScaledWidthPx() {
    int pixels = g_logicalWidthPx.load();
    return pixels > 0 ? ScalePixelsForDpi(pixels) : 0;
}

int GetLogicalWidthPx() {
    return g_logicalWidthPx.load();
}

void* GetTaskListWndFromTaskBtnGroup(void* pTaskBtnGroup) {
    return pTaskBtnGroup ? *(void**)((BYTE*)pTaskBtnGroup + 0x28) : nullptr;
}

int GetTaskButtonGroupCount(void* pTaskListWnd) {
    if (!pTaskListWnd) {
        return -1;
    }

    void* list = *(void**)((BYTE*)pTaskListWnd + 0xD8);
    if (!list) {
        return -1;
    }

    return *(int*)list;
}

int WINAPI GetNormalizedButtonWidth_Hook(void* pThis, int groupType, int index) {
    if (g_recomputeLayoutDepth > 0) {
        g_recomputeLayoutNormCalls++;
    }

    int result = GetNormalizedButtonWidth_Original(pThis, groupType, index);
    int logicalPixels = GetLogicalWidthPx();
    if (logicalPixels > 0) {
        int scaledPixels = GetScaledWidthPx();
        int translatedWidth = result;
        void* taskListWnd = GetTaskListWndFromTaskBtnGroup(pThis);

        if (taskListWnd && scaledPixels > 0 && result > 0) {
            int baselineWidth = ComputeSingleButtonWidth_Original(taskListWnd, groupType, nullptr, nullptr);
            if (baselineWidth > 0) {
                translatedWidth = MulDiv(result, scaledPixels, baselineWidth);
                if (translatedWidth < 1) {
                    translatedWidth = 1;
                }
            }
        }

        if (ShouldLogSample(g_normLogCount, 24)) {
            Wh_Log(L"[DBG norm] this=%04X taskList=%04X groupType=%d index=%d original=%d final=%d scaled=%d",
                   PtrTail(pThis), PtrTail(taskListWnd), groupType, index, result, translatedWidth,
                   scaledPixels);
        }
        return translatedWidth;
    }

    return result;
}

int WINAPI GetRequiredRows_Hook(void* pThis, int availableSpace, int height) {
    if (g_recomputeLayoutDepth > 0) {
        g_recomputeLayoutRowsCalls++;
    }

    int result = GetRequiredRows_Original(pThis, availableSpace, height);
    if (g_recomputeLayoutDepth > 0) {
        g_layoutRequiredRows = result;
    }

    if (ShouldLogSample(g_rowsLogCount, 24)) {
        Wh_Log(L"[DBG rows] this=%04X available=%d height=%d result=%d scaled=%d",
               PtrTail(pThis), availableSpace, height, result, GetScaledWidthPx());
    }

    return result;
}

int WINAPI RecomputeLayoutRow_Hook(void* pThis, int a1, int a2, int a3, int a4,
                                   int a5, RECT* rowRect) {
    if (g_recomputeLayoutDepth > 0) {
        g_recomputeLayoutRowCalls++;
    }

    int originalA2 = a2;
    int originalA4 = a4;
    int originalA5 = a5;
    int rowIndex = g_layoutRowCallIndex++;
    int rowCount = g_layoutRequiredRows;
    int taskCount = GetTaskButtonGroupCount(pThis);
    int adjustedWidth = 0;
    RECT adjustedRect{};
    RECT* rowRectForOriginal = rowRect;
    int maxItemsPerRow = 0;
    int rowItems = 0;

    int logicalPixels = GetLogicalWidthPx();
    if (logicalPixels > 0 && rowCount > 1 && taskCount > rowCount &&
        rowIndex >= 0 && rowIndex < rowCount) {
        if (rowRect) {
            int rowWidth = rowRect->right - rowRect->left;
            int scaledPixels = GetScaledWidthPx();

            if (rowWidth > 0 && scaledPixels > 0) {
                maxItemsPerRow = rowWidth / scaledPixels;
                if (maxItemsPerRow < 1) {
                    maxItemsPerRow = 1;
                }
            }
        }

        if (maxItemsPerRow > 0) {
            int startIndex = rowIndex * maxItemsPerRow;
            if (startIndex < taskCount) {
                int remainingItems = taskCount - startIndex;
                int remainingRows = rowCount - rowIndex;
                rowItems = remainingItems;
                if (rowItems > maxItemsPerRow) {
                    rowItems = maxItemsPerRow;
                }
                int minItemsToLeave = remainingRows - 1;
                if (remainingItems - rowItems < minItemsToLeave) {
                    rowItems = remainingItems - minItemsToLeave;
                }
                if (rowItems < 1) {
                    rowItems = 1;
                }

                a2 = startIndex;
                a4 = startIndex + rowItems - 1;
                a5 = (rowIndex + 1 < rowCount) ? 0 : -1;
            }
        }

        if (rowRect) {
            int rowWidth = rowRect->right - rowRect->left;
            if (rowItems > 0 && maxItemsPerRow > 0 && rowItems < maxItemsPerRow &&
                rowWidth > 0) {
                adjustedWidth = MulDiv(rowWidth, rowItems, maxItemsPerRow);
                if (adjustedWidth > 0 && adjustedWidth < rowWidth) {
                    adjustedRect = *rowRect;
                    adjustedRect.right = adjustedRect.left + adjustedWidth;
                    rowRectForOriginal = &adjustedRect;
                }
            }
        }
    }

    int result = RecomputeLayoutRow_Original(pThis, a1, a2, a3, a4, a5, rowRectForOriginal);
    if (ShouldLogSample(g_rowLayoutLogCount, 24)) {
        RECT* loggedRect = rowRectForOriginal;
        if (loggedRect) {
            int rowWidth = loggedRect->right - loggedRect->left;
            Wh_Log(L"[DBG rowlayout] this=%04X row=%d/%d args=%d,%d,%d,%d,%d orig=%d,%d,%d cap=%d items=%d width=%d adjWidth=%d rect=(%ld,%ld)-(%ld,%ld) scaled=%d result=%d",
                   PtrTail(pThis), rowIndex + 1, rowCount, a1, a2, a3, a4, a5,
                   originalA2, originalA4, originalA5, maxItemsPerRow, rowItems,
                   rowWidth, adjustedWidth,
                   loggedRect->left, loggedRect->top, loggedRect->right, loggedRect->bottom,
                   GetScaledWidthPx(), result);
        } else {
            Wh_Log(L"[DBG rowlayout] this=%04X row=%d/%d args=%d,%d,%d,%d,%d orig=%d,%d,%d cap=%d items=%d rect=null scaled=%d result=%d",
                   PtrTail(pThis), rowIndex + 1, rowCount, a1, a2, a3, a4, a5,
                   originalA2, originalA4, originalA5, maxItemsPerRow, rowItems,
                   GetScaledWidthPx(), result);
        }
    }

    return result;
}

int WINAPI RecomputeLayout_Hook(void* pThis) {
    g_recomputeLayoutDepth++;
    int savedLayoutRequiredRows = g_layoutRequiredRows;
    int savedLayoutRowCallIndex = g_layoutRowCallIndex;
    g_layoutRequiredRows = 1;
    g_layoutRowCallIndex = 0;
    int savedRowCalls = g_recomputeLayoutRowCalls;
    int savedNormCalls = g_recomputeLayoutNormCalls;
    int savedRowsCalls = g_recomputeLayoutRowsCalls;

    int result = RecomputeLayout_Original(pThis);

    int rowCalls = g_recomputeLayoutRowCalls - savedRowCalls;
    int normCalls = g_recomputeLayoutNormCalls - savedNormCalls;
    int rowsCalls = g_recomputeLayoutRowsCalls - savedRowsCalls;
    int count = GetTaskButtonGroupCount(pThis);

    if (ShouldLogSample(g_recomputeLayoutLogCount, 24)) {
        Wh_Log(L"[DBG layout] this=%04X count=%d result=%d norm=%d rows=%d rowlayout=%d scaled=%d",
               PtrTail(pThis), count, result, normCalls, rowsCalls, rowCalls, GetScaledWidthPx());
    }

    g_layoutRequiredRows = savedLayoutRequiredRows;
    g_layoutRowCallIndex = savedLayoutRowCallIndex;
    g_recomputeLayoutDepth--;
    return result;
}

void RefreshTaskbar() {
    // Primary taskbar: Shell_TrayWnd -> ReBarWindow32 -> MSTaskSwWClass
    HWND taskbar = FindWindow(L"Shell_TrayWnd", nullptr);
    if (taskbar) {
        HWND hReBarWindow32 = FindWindowEx(taskbar, nullptr, L"ReBarWindow32", nullptr);
        if (hReBarWindow32) {
            HWND hMSTaskSwWClass = FindWindowEx(hReBarWindow32, nullptr, L"MSTaskSwWClass", nullptr);
            if (hMSTaskSwWClass) {
                SendMessage(hMSTaskSwWClass, 0x452, 3, 0);
            }
        }
    }

    // Secondary taskbars: Shell_SecondaryTrayWnd -> WorkerW -> MSTaskSwWClass
    HWND secondaryTaskbar = nullptr;
    while ((secondaryTaskbar = FindWindowEx(nullptr, secondaryTaskbar, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr) {
        HWND hWorkerW = FindWindowEx(secondaryTaskbar, nullptr, L"WorkerW", nullptr);
        if (hWorkerW) {
            HWND hMSTaskSwWClass = FindWindowEx(hWorkerW, nullptr, L"MSTaskSwWClass", nullptr);
            if (hMSTaskSwWClass) {
                SendMessage(hMSTaskSwWClass, 0x452, 3, 0);
            }
        }
    }
}

bool IsExplorerPatcherModule(HMODULE module) {
    WCHAR path[MAX_PATH];
    if (!GetModuleFileName(module, path, ARRAYSIZE(path))) {
        return false;
    }
    PCWSTR name = wcsrchr(path, L'\\');
    if (!name) return false;
    name++;
    return _wcsnicmp(L"ep_taskbar.", name, 11) == 0;
}

bool HookExplorerPatcher(HMODULE module, bool calledFromInit) {
    bool expected = false;
    if (!g_hooked.compare_exchange_strong(expected, true)) {
        return true;
    }

    void* computeSingleButtonWidthPtr = (void*)GetProcAddress(module,
        "?_ComputeSingleButtonWidth@CTaskListWnd@@IEAAHW4eTBGROUPTYPE@@PEAUITaskBtnGroup@@PEAH@Z");
    void* getNormalizedButtonWidthPtr = (void*)GetProcAddress(module,
        "?_GetNormalizedButtonWidth@CTaskBtnGroup@@AEAAHW4eTBGROUPTYPE@@H@Z");
    void* getRequiredRowsPtr = (void*)GetProcAddress(module,
        "?_GetRequiredRows@CTaskListWnd@@IEAAHHH@Z");
    void* recomputeLayoutRowPtr = (void*)GetProcAddress(module,
        "?_RecomputeLayoutRow@CTaskListWnd@@IEAAHHHHHHPEAUtagRECT@@@Z");
    void* recomputeLayoutPtr = (void*)GetProcAddress(module,
        "?_RecomputeLayout@CTaskListWnd@@IEAAHXZ");

    if (!computeSingleButtonWidthPtr) {
        Wh_Log(L"ERROR: _ComputeSingleButtonWidth not found");
        g_hooked = false;
        return false;
    }

    ComputeSingleButtonWidth_Original =
        (ComputeSingleButtonWidth_t)computeSingleButtonWidthPtr;

    if (getNormalizedButtonWidthPtr &&
        !Wh_SetFunctionHook(getNormalizedButtonWidthPtr, (void*)GetNormalizedButtonWidth_Hook,
                            (void**)&GetNormalizedButtonWidth_Original)) {
        Wh_Log(L"ERROR: Wh_SetFunctionHook failed for _GetNormalizedButtonWidth");
        g_hooked = false;
        return false;
    }

    if (getRequiredRowsPtr &&
        !Wh_SetFunctionHook(getRequiredRowsPtr, (void*)GetRequiredRows_Hook,
                            (void**)&GetRequiredRows_Original)) {
        Wh_Log(L"ERROR: Wh_SetFunctionHook failed for _GetRequiredRows");
        g_hooked = false;
        return false;
    }

    if (recomputeLayoutRowPtr &&
        !Wh_SetFunctionHook(recomputeLayoutRowPtr, (void*)RecomputeLayoutRow_Hook,
                            (void**)&RecomputeLayoutRow_Original)) {
        Wh_Log(L"ERROR: Wh_SetFunctionHook failed for _RecomputeLayoutRow");
        g_hooked = false;
        return false;
    }

    if (recomputeLayoutPtr &&
        !Wh_SetFunctionHook(recomputeLayoutPtr, (void*)RecomputeLayout_Hook,
                            (void**)&RecomputeLayout_Original)) {
        Wh_Log(L"ERROR: Wh_SetFunctionHook failed for _RecomputeLayout");
        g_hooked = false;
        return false;
    }

    if (!calledFromInit) {
        Wh_ApplyHookOperations();
        RefreshTaskbar();
    }

    int percent = g_settingsPercent.load();
    int logicalPixels = g_logicalWidthPx.load();
    Wh_Log(L"Hooked width pipeline: compute=%p norm=%p rows=%p rowlayout=%p layout=%p (%d%% -> %dpx -> %dpx at %d DPI)",
           computeSingleButtonWidthPtr, getNormalizedButtonWidthPtr, getRequiredRowsPtr,
           recomputeLayoutRowPtr, recomputeLayoutPtr,
           percent, logicalPixels, GetScaledWidthPx(), GetTaskbarDpi());
    return true;
}

HMODULE WINAPI LoadLibraryExW_Hook(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    HMODULE module = LoadLibraryExW_Original(lpLibFileName, hFile, dwFlags);
    if (module && !g_hooked && IsExplorerPatcherModule(module)) {
        Wh_Log(L"ExplorerPatcher loaded: %s", lpLibFileName);
        HookExplorerPatcher(module, false);
    }
    return module;
}

void LoadSettings() {
    int percent = 120;
    int rawPercent = Wh_GetIntSetting(L"buttonWidthPercent");
    if (rawPercent > 0) {
        percent = rawPercent;
    }
    if (percent < 50) percent = 50;
    else if (percent > 300) percent = 300;

    int logicalPixels = MulDiv(percent, 160, 100);
    int scaledPixels = ScalePixelsForDpi(logicalPixels);

    Wh_Log(L"Settings loaded: percent=%d logical=%d scaled=%d dpi=%d",
           percent, logicalPixels, scaledPixels, GetTaskbarDpi());

    g_settingsPercent.store(percent);
    g_logicalWidthPx.store(logicalPixels);
    ResetDebugLogCounters();
}

BOOL Wh_ModInit() {
    Wh_Log(L"=== EP Taskbar Button Width v1.0.1 ===");
    LoadSettings();

    HMODULE hMods[1024];
    DWORD cbNeeded;
    if (EnumProcessModules(GetCurrentProcess(), hMods, sizeof(hMods), &cbNeeded)) {
        size_t count = (cbNeeded < sizeof(hMods) ? cbNeeded : sizeof(hMods)) / sizeof(HMODULE);
        for (size_t i = 0; i < count; i++) {
            if (IsExplorerPatcherModule(hMods[i])) {
                HookExplorerPatcher(hMods[i], true);
                break;
            }
        }
    }

    HMODULE kb = GetModuleHandle(L"kernelbase.dll");
    if (kb) {
        auto pLoadLib = (LoadLibraryExW_t)GetProcAddress(kb, "LoadLibraryExW");
        if (pLoadLib) {
            Wh_SetFunctionHook((void*)pLoadLib, (void*)LoadLibraryExW_Hook, (void**)&LoadLibraryExW_Original);
        }
    }

    return TRUE;
}

void Wh_ModAfterInit() {
    if (g_hooked) {
        RefreshTaskbar();
    }
}

void Wh_ModUninit() {
    g_settingsPercent.store(100);
    g_logicalWidthPx.store(0);
    RefreshTaskbar();
}

void Wh_ModSettingsChanged() {
    LoadSettings();
    RefreshTaskbar();
}
