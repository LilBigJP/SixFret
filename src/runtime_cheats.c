/* Cheats the in-game menu can switch on while playing. None are written for Guitar Hero Live
 * yet: the menu's cheat bits (gpu/mtgpu.h) are reported as not found, so it offers none. */
#include "runtime.h"
#include "gpu/mtgpu.h"

void runtime_cheats_configure(unsigned char *image, uint64_t size) { (void)image; (void)size; mtgpu_cheats_found(0); }
void runtime_cheats_start(void) {}
