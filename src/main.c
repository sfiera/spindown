#include <gba_input.h>
#include <gba_interrupt.h>
#include <gba_systemcalls.h>
#include <gba_video.h>
#include <string.h>
#include "gfx/tiles.h"

#define REG_IFBIOS (*(volatile u16*)(0x03007FF8))

void interrupt() {
    REG_IF = IRQ_VBLANK;
    REG_IFBIOS |= IRQ_VBLANK;
}

s16 sin_table[256] = {
    0,    6,    12,   18,   25,   31,   37,   43,   49,   56,   62,   68,   74,   80,   86,   92,
    97,   103,  109,  115,  120,  126,  131,  136,  142,  147,  152,  157,  162,  167,  171,  176,
    181,  185,  189,  193,  197,  201,  205,  209,  212,  216,  219,  222,  225,  228,  231,  234,
    236,  238,  241,  243,  244,  246,  248,  249,  251,  252,  253,  254,  254,  255,  255,  255,
    256,  255,  255,  255,  254,  254,  253,  252,  251,  249,  248,  246,  244,  243,  241,  238,
    236,  234,  231,  228,  225,  222,  219,  216,  212,  209,  205,  201,  197,  193,  189,  185,
    181,  176,  171,  167,  162,  157,  152,  147,  142,  136,  131,  126,  120,  115,  109,  103,
    97,   92,   86,   80,   74,   68,   62,   56,   49,   43,   37,   31,   25,   18,   12,   6,
    0,    -6,   -12,  -18,  -25,  -31,  -37,  -43,  -49,  -56,  -62,  -68,  -74,  -80,  -86,  -92,
    -97,  -103, -109, -115, -120, -126, -131, -136, -142, -147, -152, -157, -162, -167, -171, -176,
    -181, -185, -189, -193, -197, -201, -205, -209, -212, -216, -219, -222, -225, -228, -231, -234,
    -236, -238, -241, -243, -244, -246, -248, -249, -251, -252, -253, -254, -254, -255, -255, -255,
    -256, -255, -255, -255, -254, -254, -253, -252, -251, -249, -248, -246, -244, -243, -241, -238,
    -236, -234, -231, -228, -225, -222, -219, -216, -212, -209, -205, -201, -197, -193, -189, -185,
    -181, -176, -171, -167, -162, -157, -152, -147, -142, -136, -131, -126, -120, -115, -109, -103,
    -97,  -92,  -86,  -80,  -74,  -68,  -62,  -56,  -49,  -43,  -37,  -31,  -25,  -18,  -12,  -6,
};

void rotate(u8 angle) {
    s16 pa    = 2 * sin_table[angle];
    s16 pb    = 2 * -sin_table[(angle + 64) & 0xFF];
    s16 pc    = 2 * sin_table[(angle + 64) & 0xFF];
    s16 pd    = 2 * sin_table[angle];
    REG_BG2PA = pa;
    REG_BG2PB = pb;
    REG_BG2PC = pc;
    REG_BG2PD = pd;
    REG_BG2X  = 168 * 0x100 - (pa * 120 + pb * 80);
    REG_BG2Y  = 168 * 0x100 - (pc * 120 + pd * 80);
}

int main() {
    REG_DISPCNT = LCDC_OFF;  // Enable forced blank

    memcpy(CHAR_BASE_ADR(0), tilesTiles, tilesTilesLen);
    for (size_t i = 0; i < tilesPalLen / 2; ++i) {
        BG_COLORS[i] = tilesPal[i];
    }

    REG_BG2CNT = BG_SIZE_2 | BG_256_COLOR | CHAR_BASE(0) | SCREEN_BASE(8);
    u16 angle   = 0x4000;
    rotate(angle >> 8);

    for (size_t i = 1; i < tilesPalLen / 2; ++i) {
        BG_COLORS[i] = tilesPal[i];
    }
    volatile u16* map_out = MAP_BASE_ADR(8);
    const u16*    map_in  = tilesMap;
    for (int y = 0; y < 42; ++y) {
        for (int x = 0; x < 21; ++x) {
            *(map_out++) = *(map_in++);
        }
        map_out += (32 - 21);
    }

    REG_DISPCNT = MODE_2 | BG2_ON | OBJ_ON;

    // INT_VECTOR = interrupt;
    // REG_IE |= IRQ_VBLANK;
    // REG_DISPSTAT |= LCDC_VBL;
    // REG_IME = 1;

    u16 last_keys = REG_KEYINPUT;
    s16 turning = 0;
    while (true) {
        // VBlankIntrWait();
        while (REG_VCOUNT < 160) {
        }
        while (REG_VCOUNT == 160) {
        }
        if (turning) {
            angle += turning;
            rotate(angle >> 8);
            if (!(angle & 0x3FFF)) {
                turning = 0;
            }
        } else {
            u16 press = (~REG_KEYINPUT & last_keys);
            if (press & (KEY_L | KEY_LEFT)) {
                turning = -4;
            } else if (press & (KEY_R | KEY_RIGHT)) {
                turning = +4;
            }
            last_keys = REG_KEYINPUT;
        }
    }
}
