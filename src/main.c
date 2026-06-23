#include <gba_interrupt.h>
#include <gba_systemcalls.h>
#include <gba_video.h>

#define REG_IFBIOS (*(volatile u16*)(0x03FFFFF8))
#define REG_ISR_MAIN (*(volatile void**)0x03007FFC)

void interrupt() {
    u16 ack = REG_IE | REG_IF;
    REG_IF  = ack;
    REG_IFBIOS |= ack;
    u16* tilemap = (u16*)0x06004000;
    (*tilemap)++;
}

int main() {
    REG_DISPCNT = LCDC_OFF;  // Enable forced blank

    u8* tileset = CHAR_BASE_ADR(0);
    for (int i = 0; i < 64; ++i) {
        *(tileset++) = 0x00;
    }
    for (int i = 0; i < 64; ++i) {
        *(tileset++) = 0x01;
    }

    volatile u16* bgpal = BG_COLORS;
    bgpal[0]            = RGB5(0, 0, 0);
    bgpal[1]            = RGB5(31, 31, 31);
    bgpal[2]            = RGB5(15, 15, 15);

    REG_BG2CNT = BG_SIZE_2 | BG_256_COLOR | CHAR_BASE(0) | SCREEN_BASE(8);
    REG_BG2PA  = +0x100 * 2.0 * 0.707;
    REG_BG2PB  = -0x100 * 2.0 * 0.707;
    REG_BG2PC  = +0x100 * 2.0 * 0.707;
    REG_BG2PD  = +0x100 * 2.0 * 0.707;
    REG_BG2X   = +0x6000 * 2.0;
    REG_BG2Y   = -0x0C00 * 2.0;

    typedef u16 map_t[64][32];
    map_t* tilemap = MAP_BASE_ADR(8);
    u16   n       = 0x0001;
    for (int i = 11; i < 53; ++i) {
        for (int j = 5; j < 26; ++j) {
            (*tilemap)[i][j] = n;
        }
        n ^= 0x0101;
    }

    REG_DISPCNT = MODE_2 | BG2_ON | OBJ_ON;

    REG_ISR_MAIN = interrupt;
    REG_IE |= IRQ_VBLANK;
    REG_DISPSTAT |= LCDC_VBL;

    while (true) {
        // VBlankIntrWait();
    }
}
