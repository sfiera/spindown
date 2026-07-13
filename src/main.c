#include <gba_dma.h>
#include <gba_input.h>
#include <gba_interrupt.h>
#include <gba_sound.h>
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

typedef enum {
    GAME_MENU,
    GAME_IDLE,
    GAME_TURN,
    GAME_FALL,
    GAME_CLEAR,
    GAME_WIN,
} game_state_t;

typedef enum {
    TILE_OPEN    = 1 << 0,
    TILE_TALL    = 1 << 1,
    TILE_COLORED = 1 << 2,
    TILE_SLIDES  = 1 << 3,
} tile_flag_t;

typedef enum {
    TILE_OUTSIDE = 0,
    TILE_EMPTY   = TILE_OPEN,
    TILE_WALL    = TILE_TALL,
    TILE_BLOCK   = TILE_SLIDES,
    TILE_TARGET  = TILE_COLORED | TILE_TALL,
    TILE_MARBLE  = TILE_COLORED | TILE_SLIDES,
} tile_type_t;

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
    union {
        struct {
            u8 type : 4;
            u8 flags : 4;
        };
        struct {
            u8 open : 1;
            u8 tall : 1;
            u8 colored : 1;
            u8 slides : 1;

            u8 has_sprite : 1;
            u8 falling : 1;
            u8 matched : 1;
        };
    };

    u8 color : 3;
    u8 links : 4;
} cell_t;

static const cell_t tile_empty = {.type = TILE_EMPTY};

typedef union {
    cell_t reserved[16 * 16];
    struct {
        cell_t       level[14 * 16];
        game_state_t state;
        u8           angle;
        u8           width, height;
        u8           off_x, off_y;
    };
} game_t;

IWRAM_DATA game_t game = {
    .state = GAME_MENU,
    .angle = 0,
};
EWRAM_DATA game_t init;
EWRAM_DATA game_t undo;

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

void fill(loc_t l, u8 value) {
    u8 x = l.x * 3;
    u8 y = l.y * 3;

    const u8* src               = &tilesMetaTiles[tilesMetaMap[value * 2] * 9];
    int       index             = (y << 6) | x;
    shadow.tilemap[index + 0]   = *(src++);
    shadow.tilemap[index + 1]   = *(src++);
    shadow.tilemap[index + 2]   = *(src++);
    shadow.tilemap[index + 64]  = *(src++);
    shadow.tilemap[index + 65]  = *(src++);
    shadow.tilemap[index + 66]  = *(src++);
    shadow.tilemap[index + 128] = *(src++);
    shadow.tilemap[index + 129] = *(src++);
    shadow.tilemap[index + 130] = *(src++);
}

IWRAM_CODE void rotate(u8 matching) {
    s16 cos = sin_table[(game.angle + 64) & 0xFF];
    s16 sin = sin_table[game.angle];
    bg2.pa  = 2 * cos;
    bg2.pb  = 2 * -sin;
    bg2.pc  = 2 * sin;
    bg2.pd  = 2 * cos;
    bg2.x   = 12 * game.width * 0x100 - (cos * (2 * 120 - 1) + -sin * (2 * 80 - 1));
    bg2.y   = 12 * game.height * 0x100 - (sin * (2 * 120 - 1) + cos * (2 * 80 - 1));

    u8 a4 = (game.angle >> 6);
    u8 a8 = (game.angle >> 5);
    for (int i = 0; i < 96; i += 16) {
        for (int j = 0; j < 4; ++j) {
            shadow.palette[i + 2 + j] = tilesPal[i + 2 + ((j + 4 - a4) % 4)];
        }
        for (int j = 0; j < 8; ++j) {
            shadow.palette[i + 6 + j] = tilesPal[i + 6 + ((j + 8 - a8) % 8)];
        }
    }

    int idx = 28;
    s8  cx  = (6 * game.width) - 6;
    s8  cy  = (6 * game.height) - 6;
    for (loc_t l = {.index = 0}; l.index < 14 * 16; ++l.index) {
        if (!loc_valid(l)) {
            continue;
        }
        const cell_t* cell = &game.level[l.index];
        if (!cell->has_sprite) {
            switch (cell->type) {
                case TILE_BLOCK: fill(l, 16 | cell->links); break;
                case TILE_TARGET: fill(l, 7 + cell->color); break;
                case TILE_MARBLE:
                case TILE_EMPTY: fill(l, 0); break;
                case TILE_WALL: fill(l, 1); break;
                case TILE_OUTSIDE: fill(l, 2); break;
            }
            continue;
        }
        fill(l, 0);
        s8 x = cx - l.x * 12;
        s8 y = cy - l.y * 12;
        if (cell->falling) {
            x += game.off_x;
            y += game.off_y;
        }
        u8 tile = 64, ox = 80 - 6, oy = 120 - 6;
        if (cell->matched) {
            tile += 6 - (((matching - 1) & 0x0C) >> 1) + (cell->slides ? 0 : 8);
            ox -= 2;
            oy -= 2;
        }

        OBJATTR* s = &shadow.sprites[idx++];
        s->attr0   = (ox - ((cos * y + -sin * x) >> 8));
        s->attr1   = (oy - ((sin * y + cos * x) >> 8)) | OBJ_SIZE(1);
        if (cell->color) {
            s->attr2 = tile | ATTR2_PRIORITY(1) |
                       ATTR2_PALETTE(cell->color - 1 + (game.state == GAME_MENU ? 8 : 0));
        } else {
            u8 links = ((cell->links | (cell->links << 4)) >> a4) & 0xF;
            s->attr2 = (links * 2) | ATTR2_PRIORITY(1) | ATTR2_PALETTE(5);
        }
    }
    while (idx < 128) {
        shadow.sprites[idx++].attr0 = 191;
    }
}

void set_tile(loc_t l, tile_type_t type, u8 color, u8 links) {
    cell_t* cell     = &game.level[l.index];
    cell->type       = type;
    cell->color      = color;
    cell->links      = links;
    cell->matched    = false;
    cell->has_sprite = type == TILE_MARBLE;
}

IWRAM_CODE void add_support(loc_t l, s8 up, s8 right, u8 link, bool sound) {
    cell_t* cell = &game.level[l.index];
    if (!loc_valid(l) || !cell->falling) {
        return;
    }

    cell->falling = false;
    loc_t l_up    = {.index = l.index + up};
    if (sound) {
        if (cell->color) {
            REG_SOUND1CNT_X = 0x87B4;  // frequency
        } else {
            REG_SOUND2CNT_H = 0x8180;  // frequency
        }
    }

    if (!cell->color) {
        cell->has_sprite = false;
    }

    add_support(l_up, up, right, link, false);
    if (cell->links & link) {
        loc_t l_right = {.index = l.index + right};
        add_support(l_right, up, right, link, false);
    }
    if (cell->links & (((link << 1) | (link >> 3)) & 0xF)) {
        loc_t l_down = {.index = l.index - up};
        add_support(l_down, up, right, link, false);
    }
    if (cell->links & (((link << 2) | (link >> 2)) & 0xF)) {
        loc_t l_left = {.index = l.index - right};
        add_support(l_left, up, right, link, false);
    }
}

IWRAM_CODE void check_gravity_column(loc_t l, s8 up, s8 right, u8 link, bool sound) {
    cell_t *prev, *cell = NULL;
    for (; loc_valid(l); l.index += up) {
        prev = cell;
        cell = &game.level[l.index];
        if (!cell->slides) {
            continue;  // cannot fall, not relevant
        } else if (prev && ((prev->type == TILE_EMPTY) || prev->falling)) {
            continue;  // tile below provides no support
        }
        add_support(l, up, right, link, sound);
    }
}

IWRAM_CODE bool recheck_gravity(u8 angle, bool sound) {
    loc_t l;
    s8    up, right;
    u8    link;
    switch (angle >> 6) {
        case ANGLE_UP: l.x = 0, l.y = 13, up = -16, right = +1, link = LINK_RT; break;
        case ANGLE_RT: l.x = 0, l.y = 0, up = +1, right = +16, link = LINK_DN; break;
        case ANGLE_DN: l.x = 13, l.y = 0, up = +16, right = -1, link = LINK_LT; break;
        case ANGLE_LT: l.x = 13, l.y = 13, up = -1, right = -16, link = LINK_UP; break;
    }
    while (loc_valid(l)) {
        check_gravity_column(l, up, right, link, sound);
        l.index += right;
    }

    for (l.y = 0; l.y < 14; ++l.y) {
        for (l.x = 0; l.x < 14; ++l.x) {
            if (game.level[l.index].falling) {
                return true;
            }
        }
    }
    return false;
}

IWRAM_CODE bool check_gravity(u8 angle) {
    loc_t l;
    for (l.y = 0; l.y < 14; ++l.y) {
        for (l.x = 0; l.x < 14; ++l.x) {
            game.level[l.index].falling = game.level[l.index].slides;
        }
    }
    return recheck_gravity(angle, false);
}

IWRAM_CODE void drop_column(loc_t l, bool done, s8 up) {
    cell_t *prev, *cell = NULL;
    for (; loc_valid(l); l.index += up) {
        prev = cell;
        cell = &game.level[l.index];
        if (!cell->falling) {
            continue;
        }
        if (!cell->has_sprite) {
            cell->has_sprite = true;
        }
        if (done) {
            *prev = *cell;
            *cell = tile_empty;
        }
    }
}

IWRAM_CODE void drop(u8 angle, bool done) {
    loc_t l;
    s8    up, right;
    switch (angle >> 6) {
        case ANGLE_UP: l.x = 0, l.y = 13, up = -16, right = +1, game.off_y -= 2; break;
        case ANGLE_RT: l.x = 0, l.y = 0, up = +1, right = +16, game.off_x += 2; break;
        case ANGLE_DN: l.x = 13, l.y = 0, up = +16, right = -1, game.off_y += 2; break;
        case ANGLE_LT: l.x = 13, l.y = 13, up = -1, right = -16, game.off_x -= 2; break;
    }

    while (loc_valid(l)) {
        drop_column(l, done, up);
        l.index += right;
    }
}

IWRAM_CODE bool match() {
    bool any = false;
    for (int y = 0; y < 13; ++y) {
        for (int x = 0; x < 13; ++x) {
            loc_t   la = {.y = y, .x = x}, lb = {.y = y, .x = x + 1}, lc = {.y = y + 1, .x = x};
            cell_t *a = &game.level[la.index], *b = &game.level[lb.index],
                   *c = &game.level[lc.index];
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
            }
            if (b->matched && !b->has_sprite) {
                b->has_sprite = true;
            }
            if (c->matched && !c->has_sprite) {
                c->has_sprite = true;
            }
        }
    }
    return any;
}

IWRAM_CODE void remove_matches() {
    for (int y = 0; y < 14; ++y) {
        for (int x = 0; x < 14; ++x) {
            loc_t l = {.y = y, .x = x};
            if (game.level[l.index].matched) {
                set_tile(l, TILE_EMPTY, 0, 0);
            }
        }
    }
}

IWRAM_CODE bool done() {
    loc_t l;
    for (l.y = 0; l.y < 14; ++l.y) {
        for (l.x = 0; l.x < 14; ++l.x) {
            if (game.level[l.index].color) {
                return false;
            }
        }
    }
    return true;
}

void draw_str(int x, int y, const char* s, int color) {
    for (int i = 0; i < strlen(s); ++i) {
        char ch = (s[i] & 0x1F) | ((s[i] & 0xE0) << 1);

        MAP[10][y + 0][i + x] = 0x100 | ch | (color << 12);
        MAP[10][y + 1][i + x] = 0x120 | ch | (color << 12);
    }
}

void load(int lvl) {
    game.angle = 0;
    for (size_t i = 28; i < 128; ++i) {
        shadow.sprites[i].attr0 = 191;
    }
    bzero(game.level, sizeof(game.level));
    bzero(shadow.tilemap, sizeof(shadow.tilemap));

    const char* tiles = level_set[lvl].data;
    game.width        = level_set[lvl].w;
    game.height       = level_set[lvl].h;
    for (u8 y = 0; y < game.height; ++y) {
        for (u8 x = 0; x < game.width; ++x) {
            loc_t l     = {.x = x, .y = y};
            u8    links = 0;
            switch (*tiles) {
                case '.': set_tile(l, TILE_OUTSIDE, 0, 0); break;
                case '#': set_tile(l, TILE_WALL, 0, 0); break;
                case ' ': set_tile(l, TILE_EMPTY, 0, 0); break;
                case 'A': set_tile(l, TILE_TARGET, 1, 0); break;
                case 'a': set_tile(l, TILE_MARBLE, 1, 0); break;
                case 'B': set_tile(l, TILE_TARGET, 2, 0); break;
                case 'b': set_tile(l, TILE_MARBLE, 2, 0); break;
                case 'C': set_tile(l, TILE_TARGET, 3, 0); break;
                case 'c': set_tile(l, TILE_MARBLE, 3, 0); break;
                case 'D': set_tile(l, TILE_TARGET, 4, 0); break;
                case 'd': set_tile(l, TILE_MARBLE, 4, 0); break;
                case 'E': set_tile(l, TILE_TARGET, 5, 0); break;
                case 'e': set_tile(l, TILE_MARBLE, 5, 0); break;
                default:
                    links =
                        (((y > 0) && (*tiles == tiles[-game.width])) ? LINK_UP : 0) |
                        (((x < game.width - 1) && (*tiles == tiles[1])) ? LINK_RT : 0) |
                        (((y < game.height - 1) && (*tiles == tiles[game.width])) ? LINK_DN : 0) |
                        (((x > 0) && (*tiles == tiles[-1])) ? LINK_LT : 0);
                    set_tile(l, TILE_BLOCK, 0, links);
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
    rotate(0);
}

bool play_level(int lvl) {
    game.state = GAME_IDLE;
    load(lvl);

    REG_DISPCNT = MODE_1 | BG2_ON | OBJ_ON;
    REG_BLDCNT  = 0;
    REG_BLDY    = 0;

    u16 last_keys = REG_KEYINPUT;
    s16 turning   = 0;
    u16 delay     = 0;

    undo = init = game;
    if (check_gravity(game.angle)) {
        game.state = GAME_FALL;
        delay      = 6;
    }
    while (true) {
        rotate(delay);

        VBlankIntrWait();

        REG_BG2PA = bg2.pa;
        REG_BG2PB = bg2.pb;
        REG_BG2PC = bg2.pc;
        REG_BG2PD = bg2.pd;
        REG_BG2X  = bg2.x;
        REG_BG2Y  = bg2.y;
        DMA3COPY(&shadow.sprites, OAM, 256 | DMA32 | DMA_IMMEDIATE);
        DMA3COPY(&shadow.tilemap[0], MAP_BASE_ADR(8), (56 * 64 / 4) | DMA32 | DMA_IMMEDIATE);
        DMA3COPY(&shadow.palette, BG_COLORS, (tilesPalLen / 4) | DMA32 | DMA_IMMEDIATE);

        last_keys |= REG_KEYINPUT;

        switch (game.state) {
            case GAME_MENU: return false;

            case GAME_TURN:
                game.angle += turning;
                if (game.angle & 0x3F) {
                    continue;
                }
                REG_SOUND4CNT_L = 0x0000;  // frequency
                REG_SOUND4CNT_H = 0x8000;  // frequency

                if (check_gravity(game.angle)) {
                    game.state = GAME_FALL;
                    delay      = 6;
                } else {
                    game.state = GAME_IDLE;
                }
                break;

            case GAME_CLEAR:
                if (--delay) {
                    continue;
                }

                remove_matches();
                if (done()) {
                    game.state = GAME_WIN;
                    delay      = 30;
                } else if (check_gravity(game.angle)) {
                    game.state = GAME_FALL;
                    delay      = 6;
                } else {
                    game.state = GAME_IDLE;
                }
                break;

            case GAME_FALL:
                drop(game.angle, --delay == 0);
                if (delay) {
                    continue;
                }

                game.off_x = game.off_y = 0;
                if (recheck_gravity(game.angle, true)) {
                    delay = 6;
                } else if (match()) {
                    game.state = GAME_CLEAR;
                    delay      = 12;
                } else {
                    game.state = GAME_IDLE;
                }
                break;

            case GAME_IDLE: {
                u16 press = (~REG_KEYINPUT & last_keys);
                if (press & (KEY_L | KEY_LEFT)) {
                    undo            = game;
                    game.state      = GAME_TURN;
                    turning         = +4;
                    REG_SOUND4CNT_L = 0x7000;  // frequency
                    REG_SOUND4CNT_H = 0x8067;  // frequency
                } else if (press & (KEY_R | KEY_RIGHT)) {
                    undo            = game;
                    game.state      = GAME_TURN;
                    turning         = -4;
                    REG_SOUND4CNT_L = 0x7000;  // frequency
                    REG_SOUND4CNT_H = 0x8067;  // frequency
                } else if (press & (KEY_B)) {
                    game = undo;
                } else if (press & (KEY_SELECT)) {
                    undo = game;
                    game = init;
                } else if (press & (KEY_START)) {
                    return false;
                }
                last_keys = REG_KEYINPUT;
                break;
            }

            case GAME_WIN:
                if (--delay) {
                    continue;
                }
                return true;
        }
    }
}

void highlight_level(int lvl, bool on) {
    if (on) {
        int x = (lvl % 10) * 24;
        int y = (lvl / 10) * 24 + 21;

        shadow.sprites[0].attr0 = y;
        shadow.sprites[0].attr1 = x | OBJ_SIZE(2);
        shadow.sprites[0].attr2 = 92;
    } else {
        shadow.sprites[0].attr0 = 191;
    }
}

bool valid_level(int lvl) { return level_set[lvl].w; }

bool change_level(int* lvl, int mod) {
    int lvl2 = *lvl + mod;
    if ((lvl2 < 0) || (50 <= lvl2)) {
        return false;
    }
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
            MAP[10][3 * y + 3][3 * x + 1] = 0x150 | (i >> 4) | color;
            MAP[10][3 * y + 4][3 * x + 1] = 0x170 | (i >> 4) | color;
            MAP[10][3 * y + 3][3 * x + 2] = 0x150 | (i & 0xF) | color;
            MAP[10][3 * y + 4][3 * x + 2] = 0x170 | (i & 0xF) | color;
            if ((++i & 0xF) == 10) {
                i += (0x10 - 10);
            }
        }
    }
    REG_BG0HOFS = 4;

    REG_DISPCNT = MODE_1 | BG0_ON | BG2_ON | OBJ_ON;
    REG_BLDCNT  = 0x0C4;
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
                highlight_level(*lvl, false);
                return;
            }
        }
        highlight_level(*lvl, true);
        last_keys = REG_KEYINPUT;

        VBlankIntrWait();

        REG_BG2PA = bg2.pa;
        REG_BG2PB = bg2.pb;
        REG_BG2PC = bg2.pc;
        REG_BG2PD = bg2.pd;
        REG_BG2X  = bg2.x;
        REG_BG2Y  = bg2.y;
        DMA3COPY(&shadow.sprites, OAM, 256 | DMA32 | DMA_IMMEDIATE);
        DMA3COPY(&shadow.tilemap[0], MAP_BASE_ADR(8), (56 * 64 / 4) | DMA32 | DMA_IMMEDIATE);
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
    memcpy(&SPRITE_GFX[0x1000], uiTiles, uiTilesLen);
    memcpy(CHAR_BASE_ADR(0), tilesTiles, tilesTilesLen);
    memcpy(PATRAM4(0, 256), uiTiles, uiTilesLen);
    for (size_t i = 0; i < tilesPalLen / 2; ++i) {
        BG_COLORS[i] = OBJ_COLORS[i] = shadow.palette[i] = tilesPal[i];
    }
    for (size_t i = 0; i < tilesPalLen / 2; ++i) {
        union {
            struct {
                u16 red : 5;
                u16 green : 5;
                u16 blue : 5;
                u16 x : 1;
            };
            u16 value;
        } color = {.value = OBJ_COLORS[i]};
        color.red *= 0.375;
        color.green *= 0.375;
        color.blue *= 0.375;
        OBJ_COLORS[i + 128] = color.value;
    }
    for (size_t i = 0; i < 128; ++i) {
        shadow.sprites[i].attr0 = OAM[i].attr0 = 191;
    }
    REG_BG2CNT = BG_SIZE_2 | BG_256_COLOR | CHAR_BASE(0) | SCREEN_BASE(8) | BG_PRIORITY(1);
    REG_BG0CNT = BG_SIZE_0 | BG_16_COLOR | CHAR_BASE(0) | SCREEN_BASE(10);

    REG_SOUNDCNT_X  = 0x80;
    REG_SOUNDCNT_L  = 0xFF77;
    REG_SOUNDCNT_H  = 0x0002;
    REG_SOUND1CNT_L = 0;       // sweep
    REG_SOUND1CNT_H = 0xF181;  // envelope, length
    REG_SOUND1CNT_X = 0;       // frequency
    REG_SOUND2CNT_L = 0xF181;  // envelope, length
    REG_SOUND2CNT_H = 0;       // frequency
    REG_SOUND4CNT_L = 0x3000;  // envelope, length
    REG_SOUND4CNT_H = 0;       // frequency

    INT_VECTOR = interrupt;
    REG_DISPSTAT |= LCDC_VBL;
    REG_IE |= IRQ_VBLANK;
    REG_IME = 1;

    int lvl = 0;
    while (true) {
        game.state = GAME_MENU;
        select_level(&lvl);
        bool play = true;
        while (play) {
            if (play_level(lvl)) {
                play = change_level(&lvl, 1) && valid_level(lvl);
            } else {
                play = false;
            }
        }
    }
}
