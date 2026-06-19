# Makefile for YST unlock - MinGW-w64
# 用法：make

CC      = x86_64-w64-mingw32-gcc
WINDRES = x86_64-w64-mingw32-windres
TARGET  = yst_unlock.exe
SRC     = main.c decrypt.c gui.c
RES     = app.res

# -Os 优化体积，-s 去除符号，-mwindows 隐藏控制台
CFLAGS  = -Os -s -DUNICODE -D_UNICODE \
          -mwindows \
          -Wall -Wno-unused-parameter \
          -std=c11

LDFLAGS = -lshlwapi -lshell32 -lcomctl32 -lcomdlg32 -lole32

all: $(TARGET)

$(RES): app.rc app.manifest
	$(WINDRES) -O coff -i app.rc -o $(RES)

$(TARGET): $(SRC) $(RES)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "Build OK: $@"
	@ls -lh $@

clean:
	rm -f $(TARGET) $(RES)

.PHONY: all clean
