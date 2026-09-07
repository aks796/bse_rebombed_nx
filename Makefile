#---------------------------------------------------------------------------------
# BombSquad -- Nintendo Switch homebrew loader
#
# Loads the arm64-v8a libmain.so from a user-supplied BombSquad 1.7.62 APK
# inside a minimal Android environment built on libnx + Mesa.
#
# Requires devkitA64 plus:
#   pacman -S switch-mesa switch-libdrm_nouveau switch-sdl2 switch-zlib \
#             switch-freetype switch-libpng
#---------------------------------------------------------------------------------
.SUFFIXES:
ifeq ($(strip $(DEVKITPRO)),)
$(error "Set DEVKITPRO in your environment. (export DEVKITPRO=/opt/devkitpro)")
endif
TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET      := explodinary
APP_TITLE   := BombSquad: Explodinary Rebombed
APP_AUTHOR  := aks796, Square Hair Team
APP_VERSION := 1.0.0
APP_ICON    := $(TOPDIR)/resources/icon.jpg
export APP_TITLE APP_AUTHOR APP_VERSION APP_ICON
BUILD    := build
SOURCES  := source
INCLUDES := source

ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS  := -Wall -Wno-unused-function -O2 -DNDEBUG -ffunction-sections -fdata-sections \
           $(ARCH) $(DEFINES) $(INCLUDE) -D__SWITCH__
CFLAGS  += $(shell $(PREFIX)pkg-config --cflags freetype2 2>/dev/null)
CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++17
ASFLAGS  := $(ARCH)
LDFLAGS   = -specs=$(DEVKITPRO)/libnx/switch.specs $(ARCH) -Wl,--gc-sections

# Mesa GLES3 + EGL on nouveau, SDL2 for audio/HID, freetype for dynamic text.
# freetype and harfbuzz reference each other, so freetype is listed twice.
LIBS := -lSDL2 -lGLESv2 -lEGL -lglapi -ldrm_nouveau \
        -lfreetype -lharfbuzz -lfreetype -lpng -lbz2 -lz -lstdc++ -lnx -lm

LIBDIRS := $(PORTLIBS) $(LIBNX)

ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT  := $(CURDIR)/$(TARGET)
export TOPDIR  := $(CURDIR)
export VPATH   := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))

export LD := $(CC)
export OFILES := $(SFILES:.s=.o) $(CPPFILES:.cpp=.o) $(CFILES:.c=.o)
export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I$(PORTLIBS)/include/SDL2 \
                  -I$(PORTLIBS)/include/freetype2 \
                  -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: all clean
all: $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile
$(BUILD):
	@mkdir -p $@
clean:
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf
else
DEPENDS := $(OFILES:.o=.d)
NROFLAGS := --icon=$(APP_ICON) --nacp=$(OUTPUT).nacp
all : $(OUTPUT).nro
$(OUTPUT).nro : $(OUTPUT).elf $(OUTPUT).nacp
$(OUTPUT).elf : $(OFILES)
-include $(DEPENDS)
endif
