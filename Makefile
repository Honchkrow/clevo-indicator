CC = gcc
CFLAGS = -c -Wall -std=gnu99 -Wno-deprecated-declarations
LDFLAGS =

DSTDIR := /usr/local
OBJDIR := obj
SRCDIR := src

SRC = clevo-indicator.c
OBJ = $(patsubst %.c,$(OBJDIR)/%.o,$(SRC))

TARGET = bin/clevo-indicator

PKG_CONFIG ?= pkg-config
INDICATOR_PKGS := ayatana-appindicator3-0.1 gtk+-3.0
INDICATOR_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(INDICATOR_PKGS))
INDICATOR_LIBS := $(shell $(PKG_CONFIG) --libs $(INDICATOR_PKGS))

ifeq ($(strip $(INDICATOR_CFLAGS)),)
$(error Missing $(INDICATOR_PKGS). Install: sudo apt install libayatana-appindicator3-dev libgtk-3-dev)
endif

CFLAGS += $(INDICATOR_CFLAGS)
LDFLAGS += $(INDICATOR_LIBS)

all: $(TARGET)

install: $(TARGET)
	@echo "Installing to ${DSTDIR}/bin/clevo-indicator, setuid root"
	@sudo install -o root -g adm -m 0750 $(TARGET) ${DSTDIR}/bin/clevo-indicator
	@sudo chmod u+s ${DSTDIR}/bin/clevo-indicator
	@ls -l ${DSTDIR}/bin/clevo-indicator

.PHONY: all install clean

$(TARGET): $(OBJ) Makefile
	@mkdir -p bin
	@echo linking $(TARGET) from $(OBJ)
	@$(CC) $(OBJ) -o $(TARGET) $(LDFLAGS) -lm

clean:
	rm -f $(OBJ) $(TARGET)

$(OBJDIR)/%.o : $(SRCDIR)/%.c Makefile
	@echo compiling $<
	@mkdir -p obj
	@$(CC) $(CFLAGS) -c $< -o $@
