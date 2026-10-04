#pragma once

namespace soundsplice
{
/** Asks the platform to draw @p nativeWindowHandle's title bar dark, so the
    window's frame matches the dark interface inside it. Windows draws a
    native title bar light unless asked; elsewhere this does nothing, since
    the platform follows the system's own appearance. */
void useDarkTitleBar(void* nativeWindowHandle);
} // namespace soundsplice
