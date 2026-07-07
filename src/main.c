#include <gba_dma.h>
#include <gba_input.h>
#include <gba_interrupt.h>
#include <gba_sprites.h>
#include <gba_systemcalls.h>
#include <gba_types.h>
#include <gba_video.h>
#include <string.h>

#include "gfx/orbs.h"
#include "gfx/tiles.h"
#include "gfx/ui.h"
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

typedef union {
    u8 index;
    struct {
        u8 x : 4;  // lower nibble
        u8 y : 4;  // upper nibble
    };
} loc_t;

static inline bool loc_valid(loc_t l) { return (l.x < 14) && (l.y < 14); }

enum {
    ANGLE_UP = 0,
    ANGLE_RT = 1,
    ANGLE_DN = 2,
    ANGLE_LT = 3,
};

enum {
    LINK_UP = 1 << ANGLE_UP,
    LINK_RT = 1 << ANGLE_RT,
    LINK_DN = 1 << ANGLE_DN,
    LINK_LT = 1 << ANGLE_LT,
};

typedef struct {
    u8 color : 3;
    u8 solid : 1;
    u8 links : 4;

    u8 slides : 1;
    u8 has_sprite : 1;
    u8 supported : 1;
    u8 matched : 1;
} cell_t;

IWRAM_DATA cell_t tile_empty = {};

IWRAM_DATA cell_t level[16 * 16];
IWRAM_DATA u8     width, height;
IWRAM_DATA u8     off_x, off_y;

IWRAM_DATA struct {
    union {
        OBJATTR   sprites[128];
        OBJAFFINE affine[32];
    };
    u8  tilemap[56 * 64];
    u16 palette[tilesPalLen / 2];
} shadow;

IWRAM_DATA struct {
    s16 pa, pb, pc, pd;
    s32 x, y;
} bg2;

IWRAM_CODE void rotate(u8 angle, u8 matching) {
    s16 cos = sin_table[(angle + 64) & 0xFF];
    s16 sin = sin_table[angle];
    bg2.pa  = 2 * cos;
    bg2.pb  = 2 * -sin;
    bg2.pc  = 2 * sin;
    bg2.pd  = 2 * cos;
    bg2.x   = 12 * width * 0x100 - (cos * (2 * 120 - 1) + -sin * (2 * 80 - 1));
    bg2.y   = 12 * height * 0x100 - (sin * (2 * 120 - 1) + cos * (2 * 80 - 1));

    u8 a4 = (angle >> 6);
    u8 a8 = (angle >> 5);
    for (int i = 0; i < 96; i += 16) {
        for (int j = 0; j < 4; ++j) {
            shadow.palette[i + 2 + j] = tilesPal[i + 2 + ((j + 4 - a4) % 4)];
        }
        for (int j = 0; j < 8; ++j) {
            shadow.palette[i + 6 + j] = tilesPal[i + 6 + ((j + 8 - a8) % 8)];
        }
    }

    int idx = 0;
    s8  cx  = (6 * width) - 6;
    s8  cy  = (6 * height) - 6;
    for (loc_t l = {.index = 0}; l.index < 14 * 16; ++l.index) {
        const cell_t* cell = &level[l.index];
        if (!cell->has_sprite) {
            continue;
        }
        s8 x = cx - l.x * 12;
        s8 y = cy - l.y * 12;
        if (!cell->supported) {
            x += off_x;
            y += off_y;
        }
        u8 tile = 64, ox = 80 - 6, oy = 120 - 6;
        if (cell->matched) {
            tile += 12 - ((matching - 1) & 0x0C) + (cell->slides ? 0 : 16);
            ox -= 2;
            oy -= 2;
        }

        OBJATTR* s = &shadow.sprites[idx++];
        s->attr0   = (ox - ((cos * y + -sin * x) >> 8));
        s->attr1   = (oy - ((sin * y + cos * x) >> 8)) | OBJ_SIZE(1);
        if (cell->color >= 1) {
            s->attr2 = tile | ATTR2_PRIORITY(1) | ATTR2_PALETTE(cell->color - 1);
        } else {
            u8 links = ((cell->links | (cell->links << 4)) >> a4) & 0xF;
            s->attr2 = (links * 4) | ATTR2_PRIORITY(1) | ATTR2_PALETTE(5);
        }
    }
    while (idx < 128) {
        shadow.sprites[idx++].attr0 = 191;
    }
}

void fill(loc_t l, u8 value) {
    u8 x = l.x * 3;
    u8 y = l.y * 3;

    const u8* src = &tilesMetaTiles[tilesMetaMap[value * 2] * 9];
    for (u8 yy = y; yy < y + 3; ++yy) {
        for (u8 xx = x; xx < x + 3; ++xx) {
            shadow.tilemap[(yy << 6) | xx] = *(src++);
        }
    }
}

void set_tile(loc_t l, u8 value, u8 links) {
    cell_t* cell     = &level[l.index];
    cell->color      = (8 <= value && value < 13) ? (value - 7) : 0;
    cell->solid      = value != 0;
    cell->links      = links;
    cell->slides     = value >= 16;
    cell->matched    = false;
    cell->has_sprite = false;

    fill(l, value);
}

IWRAM_CODE void check_gravity_column(loc_t l, s8 up, s8 right, u8 link) {
    cell_t *prev, *cell = NULL;
    while (loc_valid(l)) {
        prev = cell;
        cell = &level[l.index];
        if (cell->slides && (!prev || prev->supported)) {
            cell->supported = true;
            if (cell->links & link) {
                loc_t l2 = {.index = l.index + right};
                if (loc_valid(l2) && !level[l2.index].supported) {
                    level[l2.index].supported = true;
                    check_gravity_column(l2, up, right, link);
                }
            }
            if (cell->links & (((link << 1) | (link >> 3)) & 0xF)) {
                loc_t l2 = {.index = l.index - up};
                if (loc_valid(l2) && !level[l2.index].supported) {
                    level[l2.index].supported = true;
                    check_gravity_column(l2, up, right, link);
                }
            }
            if (cell->links & (((link << 2) | (link >> 2)) & 0xF)) {
                loc_t l2 = {.index = l.index - right};
                if (loc_valid(l2) && !level[l2.index].supported) {
                    level[l2.index].supported = true;
                    check_gravity_column(l2, up, right, link);
                }
            }
        }
        l.index += up;
    }
}

IWRAM_CODE bool check_gravity(u8 angle) {
    loc_t l;
    for (l.y = 0; l.y < 14; ++l.y) {
        for (l.x = 0; l.x < 14; ++l.x) {
            level[l.index].supported = level[l.index].solid && !level[l.index].slides;
        }
    }

    s8 up, right;
    u8 link;
    switch (angle >> 6) {
        case ANGLE_UP: l.x = 0, l.y = 13, up = -16, right = +1, link = LINK_RT; break;
        case ANGLE_RT: l.x = 0, l.y = 0, up = +1, right = +16, link = LINK_DN; break;
        case ANGLE_DN: l.x = 13, l.y = 0, up = +16, right = -1, link = LINK_LT; break;
        case ANGLE_LT: l.x = 13, l.y = 13, up = -1, right = -16, link = LINK_UP; break;
    }
    while (loc_valid(l)) {
        check_gravity_column(l, up, right, link);
        l.index += right;
    }

    for (l.y = 0; l.y < 14; ++l.y) {
        for (l.x = 0; l.x < 14; ++l.x) {
            if (level[l.index].slides && !level[l.index].supported) {
                return true;
            }
        }
    }
    return false;
}

IWRAM_CODE void drop_column(loc_t l, u8 remainder, s8 up) {
    cell_t *prev, *cell = NULL;
    while (loc_valid(l)) {
        prev = cell;
        cell = &level[l.index];
        if (!cell->slides || cell->supported) {
            l.index += up;
            continue;
        }
        if (!cell->has_sprite) {
            cell->has_sprite = true;
            fill(l, 0);
        }
        if (!remainder) {
            loc_t l_prev = {.index = l.index - up};
            *prev        = *cell;
            *cell        = tile_empty;
            if (!prev->color) {
                prev->has_sprite = false;
                fill(l_prev, 16 | prev->links);
            }
        }
        l.index += up;
    }
}

IWRAM_CODE void drop(u8 angle, u8 remainder) {
    loc_t l;
    s8    up, right;
    s8    dx = 0, dy = 0;
    switch (angle >> 6) {
        case ANGLE_UP: l.x = 0, l.y = 13, up = -16, right = +1, dy = -2; break;
        case ANGLE_RT: l.x = 0, l.y = 0, up = +1, right = +16, dx = +2; break;
        case ANGLE_DN: l.x = 13, l.y = 0, up = +16, right = -1, dy = +2; break;
        case ANGLE_LT: l.x = 13, l.y = 13, up = -1, right = -16, dx = -2; break;
    }

    off_x += dx;
    off_y += dy;
    while (loc_valid(l)) {
        drop_column(l, remainder, up);
        l.index += right;
    }
}

IWRAM_CODE bool match() {
    bool any = false;
    for (int y = 0; y < 13; ++y) {
        for (int x = 0; x < 13; ++x) {
            loc_t   la = {.y = y, .x = x}, lb = {.y = y, .x = x + 1}, lc = {.y = y + 1, .x = x};
            cell_t *a = &level[la.index], *b = &level[lb.index], *c = &level[lc.index];
            if (!a->color) {
                continue;
            }
            if (a->color == b->color) {
                any = (a->matched = b->matched = true);
            }
            if (a->color == c->color) {
                any = (a->matched = c->matched = true);
            }
            if (a->matched && !a->has_sprite) {
                a->has_sprite = true;
                fill(la, 0);
            }
            if (b->matched && !b->has_sprite) {
                b->has_sprite = true;
                fill(lb, 0);
            }
            if (c->matched && !c->has_sprite) {
                c->has_sprite = true;
                fill(lc, 0);
            }
        }
    }
    return any;
}

IWRAM_CODE void clear() {
    for (int y = 0; y < 14; ++y) {
        for (int x = 0; x < 14; ++x) {
            loc_t l = {.y = y, .x = x};
            if (level[l.index].matched) {
                set_tile(l, 0, 0);
            }
        }
    }
}

IWRAM_CODE bool done() {
    loc_t l;
    for (l.y = 0; l.y < 14; ++l.y) {
        for (l.x = 0; l.x < 14; ++l.x) {
            if (level[l.index].color) {
                return false;
            }
        }
    }
    return true;
}

void set_orb(loc_t l, u8 color) {
    set_tile(l, 0, 0);
    cell_t* cell     = &level[l.index];
    cell->color      = color;
    cell->has_sprite = true;
    cell->slides     = true;
}

typedef enum {
    PLAY_EXIT,
    PLAY_WIN,
    PLAY_AGAIN,
} play_result_t;

void draw_str(int x, int y, const char* s, int color) {
    for (int i = 0; i < strlen(s); ++i) {
        char ch = (s[i] & 0x0F) | ((s[i] & 0xF0) << 1);

        MAP[10][y + 0][i + x] = 0x100 | ch | (color << 12);
        MAP[10][y + 1][i + x] = 0x110 | ch | (color << 12);
    }
}

void load(int lvl) {
    for (size_t i = 0; i < 128; ++i) {
        shadow.sprites[i].attr0 = 191;
    }
    bzero(level, sizeof(level));
    bzero(shadow.tilemap, sizeof(shadow.tilemap));

    const char* tiles = level_set[lvl].data;
    width             = level_set[lvl].w;
    height            = level_set[lvl].h;
    for (u8 y = 0; y < height; ++y) {
        for (u8 x = 0; x < width; ++x) {
            loc_t l     = {.x = x, .y = y};
            u8    links = 0;
            switch (*tiles) {
                case '.': set_tile(l, 2, 0); break;
                case '#': set_tile(l, 1, 0); break;
                case ' ': set_tile(l, 0, 0); break;
                case 'A': set_tile(l, 8, 0); break;
                case 'B': set_tile(l, 9, 0); break;
                case 'C': set_tile(l, 10, 0); break;
                case 'D': set_tile(l, 11, 0); break;
                case 'E': set_tile(l, 12, 0); break;
                case 'a': set_orb(l, 1); break;
                case 'b': set_orb(l, 2); break;
                case 'c': set_orb(l, 3); break;
                case 'd': set_orb(l, 4); break;
                case 'e': set_orb(l, 5); break;
                default:
                    links = (((y > 0) && (*tiles == tiles[-width])) ? LINK_UP : 0) |
                            (((x < width - 1) && (*tiles == tiles[1])) ? LINK_RT : 0) |
                            (((y < height - 1) && (*tiles == tiles[width])) ? LINK_DN : 0) |
                            (((x > 0) && (*tiles == tiles[-1])) ? LINK_LT : 0);
                    set_tile(l, 16 | links, links);
                    break;
            }
            ++tiles;
        }
    }

    int len   = strlen(level_set[lvl].title);
    int start = (31 - len) / 2;
    bzero(MAP[10][18], sizeof(MAP[10][0]));
    bzero(MAP[10][19], sizeof(MAP[10][1]));
    draw_str(start, 18, level_set[lvl].title, 6);
    rotate(0, 0);
}

play_result_t play_level(int lvl) {
    load(lvl);

    REG_DISPCNT = MODE_1 | BG2_ON | OBJ_ON | OBJ_1D_MAP;
    REG_BLDCNT  = 0;
    REG_BLDY    = 0;

    u16 last_keys = REG_KEYINPUT;
    s16 turning   = 0;
    u16 falling   = 0;
    u16 matching  = 0;

    u8 angle = 0;
    if (check_gravity(angle)) {
        falling = 6;
    }
    while (true) {
        if (turning) {
            angle += turning;
            if (!(angle & 0x3F)) {
                turning = 0;
                if (check_gravity(angle)) {
                    falling = 6;
                }
            }
        } else if (matching) {
            if (!--matching) {
                clear();
                if (done()) {
                    return PLAY_WIN;
                } else if (check_gravity(angle)) {
                    falling = 6;
                }
            }
        } else if (falling) {
            drop(angle, --falling);
            if (!falling) {
                off_x = off_y = 0;
                if (check_gravity(angle)) {
                    falling = 6;
                } else if (match()) {
                    matching = 12;
                }
            }
        } else {
            u16 press = (~REG_KEYINPUT & last_keys);
            if (press & (KEY_L | KEY_LEFT)) {
                turning = +4;
            } else if (press & (KEY_R | KEY_RIGHT)) {
                turning = -4;
            } else if (press & (KEY_SELECT)) {
                return PLAY_EXIT;
            } else if (press & (KEY_START)) {
                return PLAY_AGAIN;
            }
            last_keys = REG_KEYINPUT;
        }
        rotate(angle, matching);

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
        DMA3COPY(&shadow.palette, BG_COLORS, (tilesPalLen / 4) | DMA32 | DMA_IMMEDIATE);
    }
}

void highlight_level(int lvl, bool on) {
    int x = (lvl % 10) * 3;
    int y = (lvl / 10) * 3;

    MAP[10][y + 2][x + 0] = on ? 0x0101 : 0;
    MAP[10][y + 2][x + 3] = on ? 0x0501 : 0;
    MAP[10][y + 5][x + 0] = on ? 0x0901 : 0;
    MAP[10][y + 5][x + 3] = on ? 0x0D01 : 0;
}

bool valid_level(int lvl) { return level_set[lvl].w; }

bool change_level(int* lvl, int mod) {
    int lvl2 = *lvl + mod;
    if ((lvl2 < 0) || (50 <= lvl2)) {
        return false;
    }
    highlight_level(*lvl, false);
    highlight_level(lvl2, true);
    load(lvl2);
    *lvl = lvl2;
    return true;
}

IWRAM_CODE void select_level(int* lvl) {
    bzero(MAP[10], sizeof(MAP[10]));
    load(*lvl);

    draw_str(9, 0, "SELECT LEVEL", 6);
    draw_str(8, 21, "(C)2026 SFIERA", 6);

    int i = 0x01;
    int l = 0;
    for (int y = 0; y < 5; ++y) {
        for (int x = 0; x < 10; ++x) {
            int color                     = (valid_level(l++) ? 6 : 5) << 12;
            MAP[10][3 * y + 3][3 * x + 1] = 0x160 | (i >> 4) | color;
            MAP[10][3 * y + 4][3 * x + 1] = 0x170 | (i >> 4) | color;
            MAP[10][3 * y + 3][3 * x + 2] = 0x160 | (i & 0xF) | color;
            MAP[10][3 * y + 4][3 * x + 2] = 0x170 | (i & 0xF) | color;
            if ((++i & 0xF) == 10) {
                i += (0x10 - 10);
            }
        }
    }
    highlight_level(*lvl, true);
    REG_BG0HOFS = 4;

    REG_DISPCNT = MODE_1 | BG0_ON | BG2_ON | OBJ_ON | OBJ_1D_MAP;
    REG_BLDCNT  = 0x0D4;
    REG_BLDY    = 0x0A;

    u16 last_keys = REG_KEYINPUT;
    while (true) {
        u16 press = (~REG_KEYINPUT & last_keys);
        if (press & KEY_UP) {
            change_level(lvl, -10);
        } else if (press & KEY_DOWN) {
            change_level(lvl, +10);
        } else if (press & KEY_RIGHT) {
            change_level(lvl, +1);
        } else if (press & KEY_LEFT) {
            change_level(lvl, -1);
        } else if (press & (KEY_START | KEY_A)) {
            if (valid_level(*lvl)) {
                return;
            }
        }
        last_keys = REG_KEYINPUT;

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
        DMA3COPY(&shadow.palette, BG_COLORS, (tilesPalLen / 4) | DMA32 | DMA_IMMEDIATE);
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
    memcpy(PATRAM4(0, 256), uiTiles, uiTilesLen);
    for (size_t i = 0; i < tilesPalLen / 2; ++i) {
        BG_COLORS[i] = OBJ_COLORS[i] = shadow.palette[i] = tilesPal[i];
    }
    for (size_t i = 0; i < 128; ++i) {
        OAM[i].attr0 = 191;
    }
    REG_BG2CNT = BG_SIZE_2 | BG_256_COLOR | CHAR_BASE(0) | SCREEN_BASE(8) | BG_PRIORITY(1);
    REG_BG0CNT = BG_SIZE_0 | BG_16_COLOR | CHAR_BASE(0) | SCREEN_BASE(10);

    INT_VECTOR = interrupt;
    REG_DISPSTAT |= LCDC_VBL;
    REG_IE |= IRQ_VBLANK;
    REG_IME = 1;

    int lvl = 0;
    while (true) {
        select_level(&lvl);
        bool play = true;
        while (play) {
            switch (play_level(lvl)) {
                case PLAY_WIN: play = change_level(&lvl, 1) && valid_level(lvl); break;
                case PLAY_EXIT: play = false; break;
                case PLAY_AGAIN: continue;
            }
        }
    }
}
