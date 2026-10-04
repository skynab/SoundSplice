#include "DarkTitleBar.h"

#if defined(_WIN32)
 #define WIN32_LEAN_AND_MEAN
 #define NOMINMAX
 #include <windows.h>
 #include <dwmapi.h>
 #pragma comment(lib, "dwmapi.lib")
#endif

namespace soundsplice
{
void useDarkTitleBar(void* nativeWindowHandle)
{
#if defined(_WIN32)
    if (nativeWindowHandle == nullptr)
        return;

    // DWMWA_USE_IMMERSIVE_DARK_MODE: 20 from Windows 10 20H1, 19 before it.
    // Whichever this Windows doesn't know, it refuses, so asking for both is
    // harmless.
    const BOOL  dark         = TRUE;
    const DWORD attributes[] = { 20u, 19u };
    for (const DWORD attribute : attributes)
        if (SUCCEEDED(DwmSetWindowAttribute((HWND) nativeWindowHandle, attribute, &dark, sizeof(dark))))
            break;
#else
    (void) nativeWindowHandle;
#endif
}
} // namespace soundsplice
