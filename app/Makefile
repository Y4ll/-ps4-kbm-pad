# McStore - Makefile para el OpenOrbis PS4 Toolchain (https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain)
# Requisito: variable de entorno OO_PS4_TOOLCHAIN apuntando a la carpeta del toolchain (version release).
#   Linux/macOS:  export OO_PS4_TOOLCHAIN=$HOME/OpenOrbis/PS4Toolchain
#   Compilar:     make          -> genera IV0000-KBMM00001_00-KBMMANAGER000000.pkg

TITLE       := kbm_manager
VERSION     := 1.00
TITLE_ID    := KBMM00001
CONTENT_ID  := IV0000-KBMM00001_00-KBMMANAGER000000

LIBS        := -lc -lkernel -lc++ -lSceVideoOut -lScePad -lSceUserService -lSceNet -lSceSsl -lSceHttp -lSceSysmodule -lSceSystemService
EXTRAFLAGS  := -O2 -DAPP_VERSION='"$(VERSION)"'

TOOLCHAIN   := $(OO_PS4_TOOLCHAIN)
ifeq ($(TOOLCHAIN),)
$(error Define OO_PS4_TOOLCHAIN con la ruta del OpenOrbis PS4 Toolchain)
endif

SRCDIR      := src
INTDIR      := build
CPPFILES    := $(wildcard $(SRCDIR)/*.cpp)
OBJS        := $(patsubst $(SRCDIR)/%.cpp,$(INTDIR)/%.o,$(CPPFILES))
PKGFILES    := eboot.bin sce_sys/param.sfo sce_sys/icon0.png sce_module/libc.prx sce_module/libSceFios2.prx Media/jb.prx

# Modulos obligatorios del paquete (si faltan, la PS4 dice "los datos de la aplicacion estan danados").
# Se buscan dentro del toolchain: primero en bin/data/modules y, si no, en cualquier parte.
define findmod
$(firstword $(wildcard $(TOOLCHAIN)/bin/data/modules/$(1)) $(shell find $(TOOLCHAIN) -name $(1) 2>/dev/null))
endef
LIBC_SRC    := $(call findmod,libc.prx)
FIOS_SRC    := $(call findmod,libSceFios2.prx)
RIGHT_SRC   := $(call findmod,right.sprx)
ifneq ($(RIGHT_SRC),)
PKGFILES    += sce_sys/about/right.sprx
endif

CFLAGS      := --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -c $(EXTRAFLAGS) -isysroot $(TOOLCHAIN) -isystem $(TOOLCHAIN)/include
CXXFLAGS    := $(CFLAGS) -isystem $(TOOLCHAIN)/include/c++/v1
LDFLAGS     := -m elf_x86_64 -pie --script $(TOOLCHAIN)/link.x --eh-frame-hdr -L$(TOOLCHAIN)/lib $(LIBS) $(TOOLCHAIN)/lib/crt1.o

UNAME_S     := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
	CCX  := clang++
	LD   := ld.lld
	CDIR := linux
endif
ifeq ($(UNAME_S),Darwin)
	CCX  := /usr/local/opt/llvm/bin/clang++
	LD   := /usr/local/opt/llvm/bin/ld.lld
	CDIR := macos
endif

all: $(CONTENT_ID).pkg

$(CONTENT_ID).pkg: pkg.gp4
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core pkg_build $< .

pkg.gp4: $(PKGFILES)
	$(TOOLCHAIN)/bin/$(CDIR)/create-gp4 -out $@ --content-id=$(CONTENT_ID) --files "$(PKGFILES)"
	python3 fixgp4.py $@

sce_module/libc.prx:
	@test -n "$(LIBC_SRC)" || { echo "ERROR: no se encontro libc.prx en $(TOOLCHAIN)"; exit 1; }
	@mkdir -p sce_module
	cp "$(LIBC_SRC)" $@

sce_module/libSceFios2.prx:
	@test -n "$(FIOS_SRC)" || { echo "ERROR: no se encontro libSceFios2.prx en $(TOOLCHAIN)"; exit 1; }
	@mkdir -p sce_module
	cp "$(FIOS_SRC)" $@

sce_sys/about/right.sprx:
	@mkdir -p sce_sys/about
	cp "$(RIGHT_SRC)" $@

sce_sys/param.sfo: Makefile
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_new $@
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ APP_TYPE --type Integer --maxsize 4 --value 1
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ APP_VER --type Utf8 --maxsize 8 --value '$(VERSION)'
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ ATTRIBUTE --type Integer --maxsize 4 --value 32
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ CATEGORY --type Utf8 --maxsize 4 --value 'gde'
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ CONTENT_ID --type Utf8 --maxsize 48 --value '$(CONTENT_ID)'
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ SYSTEM_VER --type Integer --maxsize 4 --value 0
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ TITLE --type Utf8 --maxsize 128 --value '$(TITLE)'
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ TITLE_ID --type Utf8 --maxsize 12 --value '$(TITLE_ID)'
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ VERSION --type Utf8 --maxsize 8 --value '$(VERSION)'

eboot.bin: $(OBJS)
	$(LD) $(INTDIR)/*.o -o $(INTDIR)/kbm_manager.elf $(LDFLAGS)
	$(TOOLCHAIN)/bin/$(CDIR)/create-fself -in=$(INTDIR)/kbm_manager.elf -out=$(INTDIR)/kbm_manager.oelf --eboot "eboot.bin" --paid 0x3800000000000011

$(INTDIR)/%.o: $(SRCDIR)/%.cpp $(wildcard $(SRCDIR)/*.h)
	@mkdir -p $(INTDIR)
	$(CCX) $(CXXFLAGS) -o $@ $<

clean:
	rm -rf $(INTDIR) eboot.bin pkg.gp4 sce_sys/param.sfo sce_module sce_sys/about/right.sprx $(CONTENT_ID).pkg

.PHONY: all clean
