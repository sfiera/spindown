#include <gba_dma.h>
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

static level_t level_set[] = {
    {
        7,
        7,
        "BASIC 1",
        "#######"
        "#     #"
        "#  a  #"
        "#  #  #"
        "#     #"
        "#  A  #"
        "#######",
    },
    {
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
    },
    {},
};

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

typedef struct {
    s8 x, y;
} sprite_loc_t;
IWRAM_DATA sprite_loc_t sprite_locs[128];
IWRAM_DATA u8           sprite_count = 0;

typedef struct {
    u8   tile;
    s8   sprite;
    bool supported : 1;
    bool matched : 1;
} cell_t;
IWRAM_DATA cell_t level[16 * 16];
IWRAM_DATA u8     width, height;

IWRAM_DATA union {
    OBJATTR   sprites[128];
    OBJAFFINE affine[32];
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

    for (u8 i = 0; i < sprite_count; ++i) {
        s8 x = sprite_locs[i].x, y = sprite_locs[i].y;
        if (y != 127) {
            shadow.sprites[i].attr0 = (74 - ((cos * y + sin * x) >> 8));
            shadow.sprites[i].attr1 = (114 - ((-sin * y + cos * x) >> 8)) | OBJ_SIZE(1);
        }
    }
}

void set_tile(u8 x, u8 y, u8 value) {
    level[(y << 4) | x].tile   = value;
    level[(y << 4) | x].sprite = -1;

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
            if (cell->sprite < 0) {
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
            if ((cell->sprite < 0) || cell->supported) {
                continue;
            }
            sprite_loc_t* loc = &sprite_locs[cell->sprite];
            loc->x += dx;
            loc->y += dy;
            if (!remainder) {
                prev->sprite = cell->sprite;
                cell->sprite = -1;
            }
        }
    }
}

IWRAM_CODE void match_cells(cell_t* a, cell_t* b) {
    int a_color = (a->tile >= 4) ? (a->tile - 4) : a->sprite;
    int b_color = (b->tile >= 4) ? (b->tile - 4) : b->sprite;
    if ((a_color < 0) || (a_color != b_color)) {
        return;
    }
    a->matched = true;
    b->matched = true;
}

IWRAM_CODE bool match() {
    for (int x = 0; x < 13; ++x) {
        for (int y = 0; y < 13; ++y) {
            int     idx = (y << 4) | x;
            cell_t *a = &level[idx], *b = &level[idx + 1], *c = &level[idx + 16];
            match_cells(a, b);
            match_cells(a, c);
        }
    }

    bool done = true;
    for (int x = 0; x < 14; ++x) {
        for (int y = 0; y < 14; ++y) {
            int     idx  = (y << 4) | x;
            cell_t* cell = &level[idx];
            if (!cell->matched) {
                if ((cell->tile >= 4) || (cell->sprite >= 0)) {
                    done = false;
                }
                continue;
            }
            if (cell->sprite >= 0) {
                shadow.sprites[cell->sprite].attr0 = 191;
                sprite_locs[cell->sprite].y        = 127;
            }
            set_tile(x, y, 2);
        }
    }
    return done;
}

void set_orb(u8 x, u8 y, u8 value) {
    set_tile(x, y, 2);
    u8 idx                     = sprite_count++;
    level[(y << 4) | x].sprite = idx;
    sprite_locs[idx].x         = 6 * width - 6 - x * 12;
    sprite_locs[idx].y         = 6 * width - 6 - y * 12;
    shadow.sprites[idx].attr2  = (value * 4) | ATTR2_PRIORITY(0) | ATTR2_PALETTE(0);
}

void play_level(int lvl) {
    sprite_count = 0;
    for (size_t i = 0; i < 128; ++i) {
        OAM[i].attr0            = 191;
        shadow.sprites[i].attr0 = 191;
    }
    for (u16 i = 0; i < 256; ++i) {
        level[i].sprite = -1;
    }

    const char* tiles = level_set[lvl].data;
    width             = level_set[lvl].w;
    height            = level_set[lvl].h;
    for (u8 y = 0; y < level_set[lvl].h; ++y) {
        for (u8 x = 0; x < level_set[lvl].w; ++x) {
            switch (*(tiles++)) {
                case '.': set_tile(x, y, 0); break;
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
                if (match()) {
                    return;
                }
                if (check_gravity(angle)) {
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
        DMA3COPY(&shadow, OAM, 128 | DMA16 | DMA_IMMEDIATE);
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
    for (size_t i = 0; i < orbsPalLen / 2; ++i) {
        OBJ_COLORS[i] = orbsPal[i];
    }
    REG_BG2CNT = BG_SIZE_2 | BG_256_COLOR | CHAR_BASE(0) | SCREEN_BASE(8);

    int i = 0;
    while (level_set[i].w) {
        play_level(i++);
    }
}
