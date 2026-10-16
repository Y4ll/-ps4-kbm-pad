# kbm_pad - plugin GoldHEN (.prx)
# Requisitos: OpenOrbis PS4 Toolchain (variable OO_PS4_TOOLCHAIN), clang y ld.lld (LLVM 10+)
#   export OO_PS4_TOOLCHAIN=/ruta/al/OpenOrbis-PS4-Toolchain
#   make

TARGET     := kbm_pad.prx
TOOLCHAIN  := $(OO_PS4_TOOLCHAIN)
INTDIR     := build

SDK_SRC    := $(wildcard sdk/source/*.c)
APP_SRC    := $(wildcard src/*.c)
OBJS       := $(patsubst sdk/source/%.c,$(INTDIR)/sdk_%.o,$(SDK_SRC)) \
              $(patsubst src/%.c,$(INTDIR)/app_%.o,$(APP_SRC))
CRT_OBJ    := $(INTDIR)/crtprx.o

# Teclado y raton se cargan en tiempo de ejecucion (ver resolve_input_libs en main.c)
LIBS       := -lSceLibcInternal -lkernel -lSceSysmodule -lSceUserService \
              -lScePad

LOG_TYPE   := -D__USE_KLOG__
CFLAGS     := --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables \
              -fno-stack-protector -c $(LOG_TYPE) -DDEBUG=0 \
              -isysroot $(TOOLCHAIN) -isystem $(TOOLCHAIN)/include \
              -Isdk/include -Wall
LDFLAGS    := -m elf_x86_64 -pie --script $(TOOLCHAIN)/link.x -e _init \
              --export-dynamic --eh-frame-hdr -L$(TOOLCHAIN)/lib $(LIBS)

CC := clang
LD := ld.lld
ifeq ($(shell uname -s),Darwin)
  CC := /usr/local/opt/llvm/bin/clang
  LD := /usr/local/opt/llvm/bin/ld.lld
  CDIR := macos
else
  CDIR := linux
endif

# Herramienta que genera el .prx (se puede cambiar: make FSELF=/ruta/create-fself)
FSELF ?= $(TOOLCHAIN)/bin/$(CDIR)/create-fself

PROBE      := kbm_probe.prx

all: $(TARGET) $(PROBE)

$(INTDIR):
	@mkdir -p $(INTDIR)

$(CRT_OBJ): sdk/crt/crtprx.c | $(INTDIR)
	$(CC) -target x86_64-pc-linux-gnu -ffreestanding -nostdlib -fno-builtin -fPIC \
	  -isysroot $(TOOLCHAIN) -isystem $(TOOLCHAIN)/include -c $< -o $@

$(INTDIR)/sdk_%.o: sdk/source/%.c | $(INTDIR)
	$(CC) $(CFLAGS) -o $@ $<

$(INTDIR)/app_%.o: src/%.c | $(INTDIR)
	$(CC) $(CFLAGS) -o $@ $<

$(TARGET): $(CRT_OBJ) $(OBJS)
	$(LD) $(OBJS) $(CRT_OBJ) -o $(INTDIR)/kbm_pad.elf $(LDFLAGS)
	$(FSELF) -in=$(INTDIR)/kbm_pad.elf \
	  -out=$(INTDIR)/kbm_pad.oelf --lib=$(TARGET) --paid 0x3800000000000011

# Plugin minimo de diagnostico: solo libkernel (ver probe/probe.c)
$(INTDIR)/probe_%.o: probe/%.c | $(INTDIR)
	$(CC) $(CFLAGS) -o $@ $<

$(PROBE): $(CRT_OBJ) $(INTDIR)/probe_probe.o
	$(LD) $(INTDIR)/probe_probe.o $(CRT_OBJ) -o $(INTDIR)/kbm_probe.elf \
	  -m elf_x86_64 -pie --script $(TOOLCHAIN)/link.x -e _init \
	  --export-dynamic --eh-frame-hdr -L$(TOOLCHAIN)/lib -lkernel -lSceLibcInternal
	$(FSELF) -in=$(INTDIR)/kbm_probe.elf \
	  -out=$(INTDIR)/kbm_probe.oelf --lib=$(PROBE) --paid 0x3800000000000011

clean:
	rm -rf $(INTDIR) $(TARGET) $(PROBE)

.PHONY: all clean
