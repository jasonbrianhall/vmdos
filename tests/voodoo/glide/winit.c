/* WINIT.EXE / AINIT.EXE: Glide 3 (h3) brought up and shut down with no test
   library around it: init, one board?, a 640x480 window, a red clear and a
   swap, shutdown. Built with Open Watcom for DOS/4GW (WINIT) and DOS/32A
   (AINIT, run as DOS32A AINIT). Exits 0 when all of it worked, 1-3 at the
   step that didn't: vmdos logs "DPMI: client exits (N)". glidetest.sh
   builds and runs it. */
#include <stdio.h>
#include <glide.h>

int main(void)
{
    GrContext_t ctx;
    FxI32 n = 0;

    grGlideInit();
    grGet(GR_NUM_BOARDS, sizeof n, &n);
    if (n < 1) { printf("no 3dfx board found\n"); return 1; }
    grSstSelect(0);
    ctx = grSstWinOpen(0, GR_RESOLUTION_640x480, GR_REFRESH_60Hz, GR_COLORFORMAT_ABGR,
                       GR_ORIGIN_UPPER_LEFT, 2, 1);
    if (!ctx) { printf("grSstWinOpen failed\n"); grGlideShutdown(); return 2; }
    grBufferClear(0xff0000ff, 0, 0);
    grBufferSwap(1);
    grFinish();
    grGlideShutdown();
    printf("Glide 3: %d board(s), window opened, cleared, swapped\n", (int)n);
    return 0;
}
