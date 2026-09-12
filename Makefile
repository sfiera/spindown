.SUFFIXES:
#---------------------------------------------------------------------------------
ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM)
endif
ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>devkitPro)
endif

include $(DEVKITARM)/base_tools

TARGET      = spindown.gba
SRC         = src/levels.c \
              src/main.c
GRIT        = gfx/font.grit \
              gfx/logo.grit \
              gfx/orbs.grit \
              gfx/tiles.grit

.PHONY: all
all: $(TARGET)

BUILD       = build
GAME_CODE   = BSVE
MAKER_CODE  = SF
REVISION    = 00

ARCH        = -marm
LIBS        =
LIBDIRS     = $(DEVKITPRO)/libgba

DEFINES     =
INCLUDES    = -iquote $(BUILD) \
              -iquote include \
              $(LIBDIRS:%=-isystem %/include)
CFLAGS      = -g -Wall -Werror -Os \
              $(ARCH) -mcpu=arm7tdmi -mtune=arm7tdmi \
              -fomit-frame-pointer \
              -ffast-math \
              -ffunction-sections -fdata-sections  \
              $(DEFINES) \
              $(INCLUDES)

LD          = $(CC)
LDFLAGS     = -g $(ARCH) -Wl,-Map,$@.map \
              -Wl,--gc-sections \
              $(LIBDIRS:%=-L%/lib) $(LIBS)

OBJ         = $(SRC:%=$(BUILD)/%.o) $(GRIT:%.grit=$(BUILD)/$(BUILD)/%.s.o)

.PHONY: clean
clean:
	rm -fr $(BUILD) $(TARGET)

.PHONY: run
run: $(TARGET)
	open -a mGBA $(TARGET)

.PHONY: optipng
optipng:
	optipng -strip all -np -o7 gfx/*.png

%.gba: $(BUILD)/%.elf
	$(OBJCOPY) -O binary $< $(BUILD)/$*.gba
	gbafix $(BUILD)/$*.gba -t$* -c$(GAME_CODE) -m$(MAKER_CODE) -r$(REVISION)
	mv $(BUILD)/$*.gba .

$(BUILD)/spindown.elf: $(OBJ)
	$(LD) -specs=gba.specs $+ $(LDFLAGS) -o $@

$(BUILD)/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) -MMD -MP -MF $(BUILD)/$*.d $(CFLAGS) -c $< -o $@ $(ERROR_FILTER)

$(BUILD)/%.s.o: %.s
	@mkdir -p $(dir $@)
	$(CC) -MMD -MP -MF $(BUILD)/$*.d -c $< -o $@ $(ERROR_FILTER)

build/%.s build/%.h: %.grit %.png
	@mkdir -p $(dir $@)
	grit $*.png -ff $*.grit -fts -o build/$*.grit

.SECONDARY:

build/src/main.c.o: build/gfx/font.h
build/src/main.c.o: build/gfx/logo.h
build/src/main.c.o: build/gfx/orbs.h
build/src/main.c.o: build/gfx/tiles.h

-include $(OBJ:.o=.d)
