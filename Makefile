#---------------------------------------------------------------------------------
# PHIGROS -- Switch homebrew loader
# (forked from the FRUIT NINJA / PvZ FUSION / ZOOKEEPER DX lineage;
#  Unity 2022.3.62f2 rev 7670c08855a9 / IL2CPP, arm64 -- an EXACT version match
#  to the Zookeeper loader core, which is why the import gap here is 2 symbols)
# Requires devkitA64 + devkitPro pkgs: switch-mesa switch-libdrm_nouveau
#                                      switch-sdl2 switch-zlib switch-libpng
#
# All .c files in source/ are compiled automatically, so imports_phigros_extra.c,
# imports_phigros_plugin.c and phigros_jni.c are picked up without editing any
# list. nx_patch_phigros.h and unity_entrypoints.h are headers included by main.c.
#---------------------------------------------------------------------------------
.SUFFIXES:
ifeq ($(strip $(DEVKITPRO)),)
$(error "Set DEVKITPRO in your environment. (export DEVKITPRO=/opt/devkitpro)")
endif
TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET    := phigros_nx
APP_TITLE := Phigros
APP_AUTHOR := ChanseyIsTheBest
APP_VERSION := 1.0.0
APP_ICON  := $(TOPDIR)/icon.jpg
export APP_TITLE APP_AUTHOR APP_VERSION APP_ICON
BUILD     := build
SOURCES   := source
INCLUDES  := source

ARCH    := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS  := -g -Wall -O2 -ffunction-sections $(ARCH) $(DEFINES) \
           $(INCLUDE) -D__SWITCH__
CFLAGS  += -DLOAD_ADDRESS=0xC0000000
CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++17
ASFLAGS := -g $(ARCH)
LDFLAGS  = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map) \
           -Wl,--wrap,free -Wl,--wrap,malloc -Wl,--wrap,memalign -Wl,--wrap,calloc -Wl,--wrap,realloc -Wl,--wrap,memmove -Wl,--wrap,memcpy

# mesa GLES3 + EGL + nouveau, SDL2 for window/HID/audio, libpng for the optional
# cursor.png (nx_pointer.c), zlib. -lpng must precede -lz: libpng calls into it.
LIBS := -lSDL2 -lGLESv2 -lEGL -lglapi -ldrm_nouveau -lpng -lz -lnx -lm

LIBDIRS := $(PORTLIBS) $(LIBNX)

ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT  := $(CURDIR)/$(TARGET)
export TOPDIR  := $(CURDIR)
export VPATH   := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))

export LD := $(CXX)
export OFILES := $(addsuffix .o,$(SFILES)) $(CPPFILES:.cpp=.o) $(CFILES:.c=.o)
export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I$(PORTLIBS)/include/SDL2 -I$(CURDIR)/$(BUILD)
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
# embed the icon + NACP (title/author/version) into the NRO asset section
NROFLAGS := --icon=$(APP_ICON) --nacp=$(OUTPUT).nacp
all : $(OUTPUT).nro
$(OUTPUT).nro : $(OUTPUT).elf $(OUTPUT).nacp
$(OUTPUT).elf : $(OFILES)
-include $(DEPENDS)
endif
