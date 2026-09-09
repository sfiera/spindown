#include <gba_dma.h>
#include <gba_input.h>
#include <gba_interrupt.h>
#include <gba_sound.h>
#include <gba_sprites.h>
#include <gba_systemcalls.h>
#include <gba_types.h>
#include <gba_video.h>
#include <string.h>

#include "gfx/font.h"
#include "gfx/logo.h"
#include "gfx/orbs.h"
#include "gfx/tiles.h"
#include "gfx/ui2.h"
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
    if (REG_IF & IRQ_VBLANK) {
        REG_IF = IRQ_VBLANK;
        REG_IFBIOS |= IRQ_VBLANK;
    }
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
    LOC_UP = -16,
    LOC_RT = +1,
    LOC_DN = +16,
    LOC_LT = -1,

    LOC_UL = (0 * LOC_DN) + (0 * LOC_RT),
    LOC_UR = (0 * LOC_DN) + (13 * LOC_RT),
    LOC_LL = (13 * LOC_DN) + (0 * LOC_RT),
    LOC_LR = (13 * LOC_DN) + (13 * LOC_RT),
};

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
        cell_t       cells[14 * 16];
        game_state_t state;
        u8           angle;
        u8           width, height;
        u8           off_x, off_y;
        u16          steps;
    };
} game_t;

IWRAM_DATA u8     level_index;
IWRAM_DATA game_t game = {
    .state = GAME_MENU,
    .angle = 0,
};

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

static inline void sprite_ch(u8 n, u16 x, u16 y, char ch) {
    u16 value               = ((ch & 0xF0) << 2) | ((ch & 0x0F) << 1);
    shadow.sprites[n].attr0 = y;
    shadow.sprites[n].attr1 = x | OBJ_SIZE(1);
    shadow.sprites[n].attr2 = (0x200 + value) | ATTR2_PALETTE(6);
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

    if (game.state != GAME_MENU) {
        u16 steps = game.steps;
        sprite_ch(0, 230, 143, '0' | (steps % 10));
        steps /= 10;
        if (steps) {
            sprite_ch(1, 222, 143, '0' | (steps % 10));
            steps /= 10;
        } else {
            shadow.sprites[1].attr0 = 191;
        }
        if (steps) {
            sprite_ch(2, 214, 143, '0' | (steps % 10));
        } else {
            shadow.sprites[2].attr0 = 191;
        }
        sprite_ch(3, 1, 0, '9' + 1);
        sprite_ch(4, 9, 0, '0' | ((level_index + 1) / 10));
        sprite_ch(5, 17, 0, '0' | ((level_index + 1) % 10));
    }

    int idx = 64;
    s8  cx  = (6 * game.width) - 6;
    s8  cy  = (6 * game.height) - 6;
    for (loc_t l = {.index = 0}; l.index < 14 * 16; ++l.index) {
        if (!loc_valid(l)) {
            continue;
        }
        const cell_t* cell = &game.cells[l.index];
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
    cell_t* cell     = &game.cells[l.index];
    cell->type       = type;
    cell->color      = color;
    cell->links      = links;
    cell->matched    = false;
    cell->has_sprite = type == TILE_MARBLE;
}

IWRAM_CODE void add_support(loc_t l, s8 up, s8 right, u8 link, bool sound) {
    cell_t* cell = &game.cells[l.index];
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
        cell = &game.cells[l.index];
        if (!cell->slides) {
            continue;  // cannot fall, not relevant
        } else if (prev && ((prev->type == TILE_EMPTY) || prev->falling)) {
            continue;  // tile below provides no support
        }
        add_support(l, up, right, link, sound);
    }
}

IWRAM_CODE bool recheck_gravity(bool sound) {
    loc_t l;
    s8    up, right;
    u8    link;
    switch (game.angle >> 6) {
        case ANGLE_UP: l.index = LOC_LL, up = LOC_UP, right = LOC_RT, link = LINK_RT; break;
        case ANGLE_RT: l.index = LOC_UL, up = LOC_RT, right = LOC_DN, link = LINK_DN; break;
        case ANGLE_DN: l.index = LOC_UR, up = LOC_DN, right = LOC_LT, link = LINK_LT; break;
        case ANGLE_LT: l.index = LOC_LR, up = LOC_LT, right = LOC_UP, link = LINK_UP; break;
    }
    while (loc_valid(l)) {
        check_gravity_column(l, up, right, link, sound);
        l.index += right;
    }

    for (l.y = 0; l.y < 14; ++l.y) {
        for (l.x = 0; l.x < 14; ++l.x) {
            if (game.cells[l.index].falling) {
                return true;
            }
        }
    }
    return false;
}

IWRAM_CODE bool check_gravity() {
    loc_t l;
    for (l.y = 0; l.y < 14; ++l.y) {
        for (l.x = 0; l.x < 14; ++l.x) {
            game.cells[l.index].falling = game.cells[l.index].slides;
        }
    }
    return recheck_gravity(false);
}

IWRAM_CODE void drop_column(loc_t l, bool done, s8 up) {
    cell_t *prev, *cell = NULL;
    for (; loc_valid(l); l.index += up) {
        prev = cell;
        cell = &game.cells[l.index];
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

IWRAM_CODE void drop(bool done) {
    loc_t l;
    s8    up, right;
    switch (game.angle >> 6) {
        case ANGLE_UP: l.index = LOC_LL, up = LOC_UP, right = LOC_RT, game.off_y -= 2; break;
        case ANGLE_RT: l.index = LOC_UL, up = LOC_RT, right = LOC_DN, game.off_x += 2; break;
        case ANGLE_DN: l.index = LOC_UR, up = LOC_DN, right = LOC_LT, game.off_y += 2; break;
        case ANGLE_LT: l.index = LOC_LR, up = LOC_LT, right = LOC_UP, game.off_x -= 2; break;
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
            cell_t *a = &game.cells[la.index], *b = &game.cells[lb.index],
                   *c = &game.cells[lc.index];
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
            if (game.cells[l.index].matched) {
                set_tile(l, TILE_EMPTY, 0, 0);
            }
        }
    }
}

IWRAM_CODE bool done() {
    loc_t l;
    for (l.y = 0; l.y < 14; ++l.y) {
        for (l.x = 0; l.x < 14; ++l.x) {
            if (game.cells[l.index].color) {
                return false;
            }
        }
    }
    return true;
}

void draw_str(int y, const char* s, int color) {
    for (int i = 0; i < strlen(s); ++i) {
        int idx = (s[i] - ' ') * 64;
        for (int j = 0; j < 32; ++j) {
            SPRITE_GFX[0x3000 + (32 * 16 * 2 * y) + (16 * i) + j] |= fontTiles[idx + j];
            SPRITE_GFX[0x3000 + (32 * 16 * (2 * y + 1)) + (16 * i) + j] |= fontTiles[idx + j + 32];
        }
    }
}

void load() {
    game.angle = 0;
    for (size_t i = 64; i < 128; ++i) {
        shadow.sprites[i].attr0 = 191;
    }
    bzero(game.cells, sizeof(game.cells));
    bzero(shadow.tilemap, sizeof(shadow.tilemap));

    const level_t* level = &level_set[level_index];
    const char*    tiles = level->data;
    game.width           = level->w;
    game.height          = level->h;
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

    int len = strlen(level->title);
    bzero(MAP[10][18], sizeof(MAP[10][0]));
    bzero(MAP[10][19], sizeof(MAP[10][1]));
    bzero(&SPRITE_GFX[0x3000], 32 * 32 * 2);
    for (int i = 0; i < 5; ++i) {
        OAM[i + 5].attr1 = shadow.sprites[i + 5].attr1 =
            ((32 * i) + (240 / 2) - (len * 8 / 2)) | OBJ_SIZE(2);
    }
    draw_str(0, level->title, 6);
    rotate(0);
}

enum {
    UNDO_MAX = 128,
};
EWRAM_BSS game_t init;
EWRAM_BSS game_t undo_history[UNDO_MAX];
IWRAM_DATA u8    undo_oldest;
IWRAM_DATA u8    undo_current;
IWRAM_DATA u8    undo_newest;

u8 undo_size() { return (u8)(undo_newest - undo_oldest) % UNDO_MAX; }
u8 available_undo_count() { return (u8)(undo_current - undo_oldest) % UNDO_MAX; }
u8 available_redo_count() { return (u8)(undo_newest - undo_current) % UNDO_MAX; }

void init_undo() {
    undo_history[0] = init = game;
    undo_oldest = undo_current = undo_newest = 0;
}

void record_undo() {
    if (undo_size() == (UNDO_MAX - 1)) {
        undo_oldest = (undo_oldest + 1) % UNDO_MAX;
    }
    undo_history[undo_current] = game;
    undo_newest = undo_current = (undo_current + 1) % UNDO_MAX;
}

void undo() {
    if (!available_undo_count()) {
        return;
    }
    undo_current    = (u8)(undo_current - 1) % UNDO_MAX;
    game_t* curr    = &undo_history[undo_current];
    game_t  temp    = game;
    game            = *curr;
    *curr           = temp;
    REG_SOUND1CNT_X = 0x86B4;  // frequency
}

void redo() {
    if (!available_redo_count()) {
        return;
    }
    game_t* curr    = &undo_history[undo_current];
    game_t  temp    = game;
    game            = *curr;
    *curr           = temp;
    undo_current    = (undo_current + 1) % UNDO_MAX;
    REG_SOUND1CNT_X = 0x86B4;  // frequency
}

void turn(s16* turning, s16 dir) {
    record_undo();
    game.state = GAME_TURN;
    ++game.steps;
    *turning        = dir;
    REG_SOUND4CNT_L = 0x7000;  // frequency
    REG_SOUND4CNT_H = 0x8067;  // frequency
}

void restart() {
    record_undo();
    game = init;
}

bool play_level() {
    game.state = GAME_IDLE;
    game.steps = 0;
    for (size_t i = 0; i < 128; ++i) {
        shadow.sprites[i].attr0 = 191;
    }
    load();

    REG_IE      = IRQ_VBLANK;
    REG_DISPCNT = MODE_1 | BG2_ON | OBJ_ON | BIT(5);
    REG_BLDCNT  = 0;
    REG_BLDY    = 0;

    u16 last_keys = REG_KEYINPUT;
    s16 turning   = 0;
    u16 delay     = 0;

    init_undo();
    if (check_gravity()) {
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

                if (check_gravity()) {
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
                } else if (check_gravity()) {
                    game.state = GAME_FALL;
                    delay      = 6;
                } else {
                    game.state = GAME_IDLE;
                }
                break;

            case GAME_FALL:
                drop(--delay == 0);
                if (delay) {
                    continue;
                }

                game.off_x = game.off_y = 0;
                if (recheck_gravity(true)) {
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
                    turn(&turning, +4);
                } else if (press & (KEY_R | KEY_RIGHT)) {
                    turn(&turning, -4);
                } else if (press & (KEY_B)) {
                    undo();
                } else if (press & (KEY_A)) {
                    redo();
                } else if (press & (KEY_SELECT)) {
                    restart();
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

void highlight_level(bool on) {
    if (on) {
        int x = (level_index % 10) * 24;
        int y = (level_index / 10) * 22 + 25;

        shadow.sprites[0].attr0 = y;
        shadow.sprites[0].attr1 = x | OBJ_SIZE(2);
        shadow.sprites[0].attr2 = 92;
    } else {
        shadow.sprites[0].attr0 = 191;
    }
}

bool change_level(int mod) {
    int lvl2 = level_index + mod;
    if ((lvl2 < 0) || (50 <= lvl2)) {
        return false;
    }
    level_index = lvl2;
    load();
    return true;
}

IWRAM_CODE void select_level() {
    bzero(MAP[10], sizeof(MAP[10]));
    load();

    for (int i = 0; i < 4; ++i) {
        shadow.sprites[i + 1].attr0 = 4 | ATTR0_WIDE;
        shadow.sprites[i + 1].attr1 = ((32 * i) + ((240 - 96) / 2)) | OBJ_SIZE(2);
        shadow.sprites[i + 1].attr2 = (0x340 + (4 * i)) | ATTR2_PALETTE(6);
    }
    int len = strlen(level_set[level_index].title);
    for (int i = 0; i < 5; ++i) {
        shadow.sprites[i + 5].attr0 = (160 - 20) | ATTR0_WIDE;
        shadow.sprites[i + 5].attr1 = OAM[i + 5].attr1 =
            ((32 * i) + (240 / 2) - (len * 8 / 2)) | OBJ_SIZE(2);
        shadow.sprites[i + 5].attr2 = (0x300 + (4 * i)) | ATTR2_PALETTE(6);
    }

    for (int x = 0; x < 50; ++x) {
        u16 value                    = ((x & 0xF8) << 3) | ((x & 0x07) << 2);
        shadow.sprites[x + 10].attr0 = (((x / 10) * 22) + 28) | ATTR0_WIDE;
        shadow.sprites[x + 10].attr1 = (((x % 10) * 24) + 4) | OBJ_SIZE(2);
        shadow.sprites[x + 10].attr2 = (0x100 + value) | ATTR2_PALETTE(6);
    }
    REG_BG0HOFS = 4;

    REG_IE      = IRQ_VBLANK;
    REG_DISPCNT = MODE_1 | BG0_ON | BG2_ON | OBJ_ON | BIT(5);
    REG_BLDCNT  = 0x0C4;
    REG_BLDY    = 0x0A;

    u16 last_keys = REG_KEYINPUT;
    while (true) {
        u16 press = (~REG_KEYINPUT & last_keys);
        if (press & KEY_UP) {
            change_level(-10);
        } else if (press & KEY_DOWN) {
            change_level(+10);
        } else if (press & KEY_RIGHT) {
            change_level(+1);
        } else if (press & KEY_LEFT) {
            change_level(-1);
        } else if (press & (KEY_START | KEY_A)) {
            return;
        }
        highlight_level(true);
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

IWRAM_CODE int play() {
    u32* tileset = CHAR_BASE_ADR(0);
    for (size_t i = 0; i < tilesTilesLen / 4; ++i) {
        tileset[i] = tilesTiles[i];
    }
    for (size_t i = 0; i < orbsTilesLen / 2; ++i) {
        SPRITE_GFX[i] = orbsTiles[i];
    }
    memcpy(&SPRITE_GFX[0x1000], ui2Tiles, ui2TilesLen);
    memcpy(CHAR_BASE_ADR(0), tilesTiles, tilesTilesLen);
    for (size_t i = 0; i < 128; ++i) {
        shadow.sprites[i].attr0 = OAM[i].attr0 = 191;
    }

    level_index = 0;
    while (true) {
        game.state = GAME_MENU;
        select_level();
        bool play = true;
        while (play) {
            if (play_level()) {
                play = change_level(1);
            } else {
                play = false;
            }
        }
    }
}

IWRAM_CODE void logo() {
    memcpy(CHAR_BASE_ADR(0), logoTiles, logoTilesLen);
    DMA3COPY(&logoMap, MAP_BASE_ADR(8), (logoMapLen / 4) | DMA32 | DMA_IMMEDIATE);
    REG_DISPCNT = MODE_1 | BG2_ON | BIT(5);

    REG_BLDCNT = 0x00FF;
    REG_BLDY   = 0x10;
    int delay  = 0x20;

    int state     = 0;
    u16 last_keys = REG_KEYINPUT;
    while (true) {
        VBlankIntrWait();

        switch (state) {
            case 0: {
                REG_BLDY = (--delay / 2);
                if (!delay) {
                    state = 1;
                }
                break;
            }
            case 1: {
                u16 press = (~REG_KEYINPUT & last_keys);
                if (press & (KEY_A | KEY_START)) {
                    delay = 0x20;
                    state = 2;
                }
                last_keys = REG_KEYINPUT;
                break;
            }
            case 2: {
                REG_BLDY = (32 - --delay) / 2;
                if (!delay) {
                    return;
                }
                break;
            }
        }
    }
}

IWRAM_CODE int main() {
    REG_DISPCNT = LCDC_OFF;  // Enable forced blank

    draw_str(1, "SELECT LEVEL", 6);
    draw_str(3, "(C)2026 SFIERA", 6);

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

    REG_BG2CNT = BG_SIZE_2 | BG_256_COLOR | CHAR_BASE(0) | SCREEN_BASE(8) | BG_PRIORITY(1);
    REG_BG0CNT = BG_SIZE_0 | BG_16_COLOR | CHAR_BASE(0) | SCREEN_BASE(10);

    INT_VECTOR = interrupt;
    REG_DISPSTAT |= LCDC_VBL;
    REG_IE  = IRQ_VBLANK;
    REG_IME = 1;

    logo();
    play();
}
