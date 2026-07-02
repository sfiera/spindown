#include <gba_dma.h>
#include <gba_input.h>
#include <gba_interrupt.h>
#include <gba_sprites.h>
#include <gba_systemcalls.h>
#include <gba_video.h>
#include <string.h>

#include "gfx/orbs.h"
#include "gfx/tiles.h"
#include "levels.h"

#define REG_IFBIOS (*(volatile u16*)(0x03007FF8))

IWRAM_CODE void interrupt() {
    REG_IF = IRQ_VBLANK;
    REG_IFBIOS |= IRQ_VBLANK;
}

IWRAM_DATA s16 sin_table[256] = {
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

IWRAM_DATA u8 sprite_count = 0;

typedef struct {
    u8 tile;

    u8 color : 3;
    u8 has_sprite : 1;
    u8 supported : 1;

    struct {
        s8 x, y;
        s8 index;
    } sprite;
} cell_t;

IWRAM_DATA cell_t tile_empty = {.tile = 2};

IWRAM_DATA cell_t level[16 * 16];
IWRAM_DATA u8     width, height;

IWRAM_DATA struct {
    union {
        OBJATTR   sprites[128];
        OBJAFFINE affine[32];
    };
    u8 tilemap[56 * 64];
} shadow;

IWRAM_DATA struct {
    s16 pa, pb, pc, pd;
    s32 x, y;
} bg2;

IWRAM_CODE void rotate(u8 angle) {
    s16 cos = sin_table[(angle + 64) & 0xFF];
    s16 sin = sin_table[angle];
    bg2.pa  = 2 * cos;
    bg2.pb  = 2 * sin;
    bg2.pc  = 2 * -sin;
    bg2.pd  = 2 * cos;
    bg2.x   = 12 * width * 0x100 - (cos * (2 * 120 - 1) + sin * (2 * 80 - 1));
    bg2.y   = 12 * height * 0x100 - (-sin * (2 * 120 - 1) + cos * (2 * 80 - 1));

    for (int i = 0; i < 256; ++i) {
        const cell_t* cell = &level[i];
        s8            x = cell->sprite.x, y = cell->sprite.y;
        if (cell->has_sprite) {
            shadow.sprites[cell->sprite.index].attr0 = (74 - ((cos * y + sin * x) >> 8));
            shadow.sprites[cell->sprite.index].attr1 =
                (114 - ((-sin * y + cos * x) >> 8)) | OBJ_SIZE(1);
        }
    }
}

void set_tile(u8 x, u8 y, u8 value) {
    cell_t* cell     = &level[(y << 4) | x];
    cell->tile       = value;
    cell->has_sprite = false;
    cell->color      = (value >= 8) ? (value - 7) : 0;

    x *= 3;
    y *= 3;

    const u8* src = &tilesMetaTiles[tilesMetaMap[value * 2] * 9];
    for (u8 yy = y; yy < y + 3; ++yy) {
        for (u8 xx = x; xx < x + 3; ++xx) {
            shadow.tilemap[(yy << 6) | xx] = *(src++);
        }
    }
}

IWRAM_CODE bool check_gravity(u8 angle) {
    u8 start;
    s8 next_cell, next_row;
    switch (angle >> 6) {
        case 0: start = (13 << 4) | 13, next_row = -1, next_cell = -16; break;
        case 1: start = (13 << 4) | 13, next_row = -16, next_cell = -1; break;
        case 2: start = 0, next_row = +1, next_cell = +16; break;
        case 3: start = 0, next_row = +16, next_cell = +1; break;
    }

    bool any  = false;
    u8   head = start;
    for (int i = 0; i < 14; ++i) {
        u8 index = head;
        head += next_row;
        cell_t *prev, *cell = NULL;
        for (int j = 0; j < 14; ++j) {
            prev = cell;
            cell = &level[index];
            index += next_cell;
            if (!cell->has_sprite) {
                cell->supported = (cell->tile != 2);
            } else {
                cell->supported = !prev || prev->supported;
                any             = any || !cell->supported;
            }
        }
    }
    return any;
}

IWRAM_CODE void fall(u8 angle, u8 remainder) {
    u8 start;
    s8 next_cell, next_row;
    s8 dx = 0, dy = 0;
    switch (angle >> 6) {
        case 0: start = (13 << 4) | 13, next_row = -1, next_cell = -16, dy = -2; break;
        case 1: start = (13 << 4) | 13, next_row = -16, next_cell = -1, dx = -2; break;
        case 2: start = 0, next_row = +1, next_cell = +16, dy = +2; break;
        case 3: start = 0, next_row = +16, next_cell = +1, dx = +2; break;
    }

    u8 head = start;
    for (int i = 0; i < 14; ++i) {
        u8 index = head;
        head += next_row;
        cell_t *prev, *cell = NULL;
        for (int j = 0; j < 14; ++j) {
            prev = cell;
            cell = &level[index];
            index += next_cell;
            if (!cell->has_sprite || cell->supported) {
                continue;
            }
            cell->sprite.x += dx;
            cell->sprite.y += dy;
            if (!remainder) {
                *prev = *cell;
                *cell = tile_empty;
            }
        }
    }
}

IWRAM_CODE bool match() {
    u16 match[14] = {};
    for (int y = 0; y < 13; ++y) {
        for (int x = 0; x < 13; ++x) {
            int     idx = (y << 4) | x;
            cell_t *a = &level[idx], *b = &level[idx + 1], *c = &level[idx + 16];
            if (!a->color) {
                continue;
            }
            if (a->color == b->color) {
                match[y] = (3 << x);
            }
            if (a->color == c->color) {
                match[y]     = (1 << x);
                match[y + 1] = (1 << x);
            }
        }
    }

    bool done = true;
    for (int y = 0; y < 14; ++y) {
        for (int x = 0; x < 14; ++x) {
            int     idx  = (y << 4) | x;
            cell_t* cell = &level[idx];
            if (!(match[y] & (1 << x))) {
                done = done && !cell->color;
                continue;
            }
            if (cell->has_sprite) {
                shadow.sprites[cell->sprite.index].attr0 = 191;
                cell->sprite.y                           = 127;
            }
            set_tile(x, y, 2);
        }
    }
    return done;
}

void set_orb(u8 x, u8 y, u8 color) {
    set_tile(x, y, 2);
    u8      idx        = sprite_count++;
    cell_t* cell       = &level[(y << 4) | x];
    cell->color        = color;
    cell->has_sprite   = true;
    cell->sprite.index = idx;
    cell->sprite.x     = 6 * width - 6 - x * 12;
    cell->sprite.y     = 6 * width - 6 - y * 12;
    if (color >= 1) {
        shadow.sprites[idx].attr2 = 64 | ATTR2_PRIORITY(0) | ATTR2_PALETTE(color - 1);
    } else {
        int links                 = 0;
        shadow.sprites[idx].attr2 = (links * 4) | ATTR2_PRIORITY(0) | ATTR2_PALETTE(0);
    }
}

void play_level(int lvl) {
    sprite_count = 0;
    for (size_t i = 0; i < 128; ++i) {
        OAM[i].attr0            = 191;
        shadow.sprites[i].attr0 = 191;
    }
    bzero(level, sizeof(level));
    bzero(shadow.tilemap, sizeof(shadow.tilemap));

    const char* tiles = level_set[lvl].data;
    width             = level_set[lvl].w;
    height            = level_set[lvl].h;
    for (u8 y = 0; y < level_set[lvl].h; ++y) {
        for (u8 x = 0; x < level_set[lvl].w; ++x) {
            switch (*(tiles++)) {
                case '.': set_tile(x, y, 0); break;
                case '#': set_tile(x, y, 1); break;
                case ' ': set_tile(x, y, 2); break;
                case '0': set_orb(x, y, 0); break;
                case 'A': set_tile(x, y, 8); break;
                case 'B': set_tile(x, y, 9); break;
                case 'C': set_tile(x, y, 10); break;
                case 'D': set_tile(x, y, 11); break;
                case 'E': set_tile(x, y, 12); break;
                case 'a': set_orb(x, y, 1); break;
                case 'b': set_orb(x, y, 2); break;
                case 'c': set_orb(x, y, 3); break;
                case 'd': set_orb(x, y, 4); break;
                case 'e': set_orb(x, y, 5); break;
            }
        }
    }

    u8 angle = 0;
    rotate(angle);

    REG_DISPCNT = MODE_2 | BG2_ON | OBJ_ON | OBJ_1D_MAP;

    INT_VECTOR = interrupt;
    REG_DISPSTAT |= LCDC_VBL;
    REG_IE |= IRQ_VBLANK;
    REG_IME = 1;

    u16 last_keys = REG_KEYINPUT;
    s16 turning   = 0;
    u16 falling   = 0;
    while (true) {
        if (turning) {
            angle += turning;
            rotate(angle);
            if (!(angle & 0x3F)) {
                turning = 0;
                if (check_gravity(angle)) {
                    falling = 6;
                }
            }
        } else if (falling) {
            fall(angle, --falling);
            rotate(angle);
            if (!falling) {
                if (check_gravity(angle)) {
                    falling = 6;
                } else if (match()) {
                    return;
                } else if (check_gravity(angle)) {
                    falling = 6;
                }
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

        VBlankIntrWait();

        REG_BG2PA = bg2.pa;
        REG_BG2PB = bg2.pb;
        REG_BG2PC = bg2.pc;
        REG_BG2PD = bg2.pd;
        REG_BG2X  = bg2.x;
        REG_BG2Y  = bg2.y;
        DMA3COPY(&shadow.sprites, OAM, 128 | DMA16 | DMA_IMMEDIATE);
        DMA3COPY(&shadow.tilemap[0], MAP_BASE_ADR(8), 224 | DMA32 | DMA_IMMEDIATE);
        DMA3COPY(&shadow.tilemap[896], MAP_BASE_ADR(8) + 896, 224 | DMA32 | DMA_IMMEDIATE);
        DMA3COPY(&shadow.tilemap[1792], MAP_BASE_ADR(8) + 1792, 224 | DMA32 | DMA_IMMEDIATE);
        DMA3COPY(&shadow.tilemap[2688], MAP_BASE_ADR(8) + 2688, 224 | DMA32 | DMA_IMMEDIATE);
    }
}

IWRAM_CODE int main() {
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
    for (size_t i = 0; i < tilesPalLen / 2; ++i) {
        OBJ_COLORS[i] = tilesPal[i];
    }
    REG_BG2CNT = BG_SIZE_2 | BG_256_COLOR | CHAR_BASE(0) | SCREEN_BASE(8);

    int i = 0;
    while (level_set[i].w) {
        play_level(i++);
    }
}
