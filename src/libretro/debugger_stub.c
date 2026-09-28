// Commander X16 Emulator - libretro core
// All rights reserved. License: 2-clause BSD
//
// The built-in debugger draws with an SDL renderer, which the libretro build
// does not have; debugger_enabled is never set, so these are not reached.

#include "../debugger.h"

int showDebugOnRender = 0;

void DEBUGRenderDisplay(int width, int height) { (void)width; (void)height; }
void DEBUGBreakToDebugger(void) {}
int DEBUGGetCurrentStatus(void) { return 0; }
void DEBUGSetBreakPoint(struct breakpoint newBreakPoint) { (void)newBreakPoint; }
void DEBUGFreeUI() {}
