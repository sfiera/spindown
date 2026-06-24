#include <gba_input.h>
#include <gba_interrupt.h>
#include <gba_sprites.h>
#include <gba_systemcalls.h>
#include <gba_video.h>
#include <string.h>
#include "gfx/orbs.h"
#include "gfx/tiles.h"

#define REG_IFBIOS (*(volatile u16*)(0x03007FF8))

typedef struct level {
    u8         w, h;
    const char title[30];
    const char data[14 * 14];
} level_t;

static level_t level = {
    14,
    14,
    "PLUS",
    "....######...."
    "....#    #...."
    "....#    #...."
    "....#    #...."
    "#####    #####"
    "##          ##"
    "#A    #00   B#"
    "#B     #    A#"
    "##          ##"
    "#####    #####"
    "....#    #...."
    "....#    #...."
    "....#baba#...."
    "....######....",
};

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

typedef struct {
    s8 x, y;
} sprite_loc_t;
sprite_loc_t sprite_locs[128] = {
};

u8 sprite_count = 0;

void rotate(u8 angle) {
    s16 cos    = sin_table[(angle + 64) & 0xFF];
    s16 sin    = sin_table[angle];
    REG_BG2PA = 2 * cos;
    REG_BG2PB = 2 * sin;
    REG_BG2PC = 2 * -sin;
    REG_BG2PD = 2 * cos;
    REG_BG2X  = 168 * 0x100 - (cos * 2 * 120 + sin * 2 * 80);
    REG_BG2Y  = 168 * 0x100 - (-sin * 2 * 120 + cos * 2 * 80);

    for (u8 i = 0; i < sprite_count; ++i) {
        s8 x = sprite_locs[i].x, y = sprite_locs[i].y;
        OAM[i].attr0 = (74 - ((cos * y + sin * x) >> 8));
        OAM[i].attr1 = (114 - ((-sin * y + cos * x) >> 8)) | OBJ_SIZE(1);
    }
}

void set_tile(u8 x, u8 y, u8 value) {
    x *= 3;
    y *= 3;
    u16* map = MAP_BASE_ADR(8);
    for (u8 yy = y; yy < y + 3; ++yy) {
        for (u8 xx = x; xx < x + 3; ++xx) {
            u16* loc = &map[(yy << 5) | (xx >> 1)];
            if (xx & 1) {
                *loc = (*loc & 0x00FF) | (value << 8);
            } else {
                *loc = (*loc & 0xFF00) | (value << 0);
            }
        }
    }
}

void set_orb(u8 x, u8 y, u8 value) {
    set_tile(x, y, 2);
    u8 idx = sprite_count++;
    sprite_locs[idx].x = 78 - x * 12;
    sprite_locs[idx].y = 78 - y * 12;
    OAM[idx].attr2 = (value * 4) | ATTR2_PRIORITY(0) | ATTR2_PALETTE(0);
}

int main() {
    REG_DISPCNT = LCDC_OFF;  // Enable forced blank

    u32* tileset = CHAR_BASE_ADR(0);
    for (size_t i = 0; i < tilesTilesLen / 4; ++i) {
        tileset[i] = tilesTiles[i];
    }
    for (size_t i = 0; i < orbsTilesLen / 2; ++i) {
        SPRITE_GFX[i] = orbsTiles[i];
    }
    memcpy(CHAR_BASE_ADR(0), tilesTiles, tilesTilesLen);
    for (size_t i = 0; i < tilesPalLen / 2; ++i) {
        BG_COLORS[i] = tilesPal[i];
    }
    REG_BG2CNT = BG_SIZE_2 | BG_256_COLOR | CHAR_BASE(0) | SCREEN_BASE(8);

    for (size_t i = 4; i < 128; ++i) {
        OAM[i].attr0 = 191;
    }
    for (size_t i = 0; i < orbsPalLen / 2; ++i) {
        OBJ_COLORS[i] = orbsPal[i];
    }

    const char* tiles = level.data;
    for (u8 y = 0; y < level.h; ++y) {
        for (u8 x = 0; x < level.w; ++x) {
            switch (*(tiles++)) {
                case '#': set_tile(x, y, 1); break;
                case ' ': set_tile(x, y, 2); break;
                case '0': set_tile(x, y, 3); break;
                case 'A': set_tile(x, y, 4); break;
                case 'B': set_tile(x, y, 5); break;
                case 'a': set_orb(x, y, 0); break;
                case 'b': set_orb(x, y, 1); break;
            }
        }
    }

    u16 angle = 0;
    rotate(angle >> 8);

    REG_DISPCNT = MODE_2 | BG2_ON | OBJ_ON | OBJ_1D_MAP;

    // INT_VECTOR = interrupt;
    // REG_IE |= IRQ_VBLANK;
    // REG_DISPSTAT |= LCDC_VBL;
    // REG_IME = 1;

    u16 last_keys = REG_KEYINPUT;
    s16 turning   = 0;
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
