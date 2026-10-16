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
PROBE_A    := kbm_probe_a.prx
PROBE_B    := kbm_probe_b.prx
PROBE_LDFLAGS := -m elf_x86_64 -pie --script $(TOOLCHAIN)/link.x -e _init \
                 --export-dynamic --eh-frame-hdr -L$(TOOLCHAIN)/lib

STAGE1     := kbm_s1.prx
STAGE2     := kbm_s2.prx
SDK_OBJS   := $(filter $(INTDIR)/sdk_%.o,$(OBJS))

all: $(TARGET) $(PROBE) $(PROBE_A) $(PROBE_B) $(STAGE1) $(STAGE2)

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

# Variante A: probe + las mismas librerias de sistema que kbm_pad (Pad, UserService, Sysmodule)
$(INTDIR)/probe_a.o: probe/probe.c | $(INTDIR)
	$(CC) $(CFLAGS) -DPROBE_VARIANT=1 -o $@ $<

$(PROBE_A): $(CRT_OBJ) $(INTDIR)/probe_a.o
	$(LD) $(INTDIR)/probe_a.o $(CRT_OBJ) -o $(INTDIR)/kbm_probe_a.elf \
	  $(PROBE_LDFLAGS) -lkernel -lSceLibcInternal -lSceSysmodule -lSceUserService -lScePad
	$(FSELF) -in=$(INTDIR)/kbm_probe_a.elf \
	  -out=$(INTDIR)/kbm_probe_a.oelf --lib=$(PROBE_A) --paid 0x3800000000000011

# Variante B: probe + los objetos del SDK (Detour, Patcher, HDE64, ...) sin librerias extra
$(INTDIR)/probe_b.o: probe/probe.c | $(INTDIR)
	$(CC) $(CFLAGS) -DPROBE_VARIANT=2 -o $@ $<

$(PROBE_B): $(CRT_OBJ) $(INTDIR)/probe_b.o $(filter $(INTDIR)/sdk_%.o,$(OBJS))
	$(LD) $(INTDIR)/probe_b.o $(filter $(INTDIR)/sdk_%.o,$(OBJS)) $(CRT_OBJ) \
	  -o $(INTDIR)/kbm_probe_b.elf $(PROBE_LDFLAGS) -lkernel -lSceLibcInternal
	$(FSELF) -in=$(INTDIR)/kbm_probe_b.elf \
	  -out=$(INTDIR)/kbm_probe_b.oelf --lib=$(PROBE_B) --paid 0x3800000000000011

# Etapas del plugin real (src/main.c) para diagnosticar. Mismo enlazado que kbm_pad.
#   kbm_s1: solo aviso y log.  kbm_s2: s1 + enganches que solo llaman al original.
$(INTDIR)/stage1.o: src/main.c | $(INTDIR)
	$(CC) $(CFLAGS) -DKBM_STAGE=1 '-DKBM_NAME="kbm_s1"' -o $@ $<

$(STAGE1): $(CRT_OBJ) $(INTDIR)/stage1.o $(SDK_OBJS)
	$(LD) $(INTDIR)/stage1.o $(SDK_OBJS) $(CRT_OBJ) -o $(INTDIR)/kbm_s1.elf $(LDFLAGS)
	$(FSELF) -in=$(INTDIR)/kbm_s1.elf \
	  -out=$(INTDIR)/kbm_s1.oelf --lib=$(STAGE1) --paid 0x3800000000000011

$(INTDIR)/stage2.o: src/main.c | $(INTDIR)
	$(CC) $(CFLAGS) -DKBM_STAGE=2 '-DKBM_NAME="kbm_s2"' -o $@ $<

$(STAGE2): $(CRT_OBJ) $(INTDIR)/stage2.o $(SDK_OBJS)
	$(LD) $(INTDIR)/stage2.o $(SDK_OBJS) $(CRT_OBJ) -o $(INTDIR)/kbm_s2.elf $(LDFLAGS)
	$(FSELF) -in=$(INTDIR)/kbm_s2.elf \
	  -out=$(INTDIR)/kbm_s2.oelf --lib=$(STAGE2) --paid 0x3800000000000011

clean:
	rm -rf $(INTDIR) $(TARGET) $(PROBE) $(PROBE_A) $(PROBE_B) $(STAGE1) $(STAGE2)

.PHONY: all clean
