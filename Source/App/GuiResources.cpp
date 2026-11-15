#include "GuiResources.h"

#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>

namespace GuiResources
{
int gdiObjects()
{
    return (int) GetGuiResources (GetCurrentProcess(), GR_GDIOBJECTS);
}

int userObjects()
{
    return (int) GetGuiResources (GetCurrentProcess(), GR_USEROBJECTS);
}
} // namespace GuiResources
