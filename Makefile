# SoundBridge PS5: PS5 game sound -> UDP -> SoundBridge on a PC.
#
#   make ps5      dist/SoundBridge_PS5_13.60.elf (needs PS5_PAYLOAD_SDK)
#   PC apps:      powershell -File pc\build_pc.ps1   (Windows, LLVM + Windows SDK)
#                 -> dist\SoundBridge.exe (window) and dist\SoundBridge_Receiver.exe (text)

ELF   := dist/SoundBridge_PS5_13.60.elf
BUILD := build/ps5

SRCS := src/main.c src/avcap2.c src/creds.c src/diag.c src/util.c src/log.c src/lock.c \
	src/stop.c src/notify.c

.PHONY: all ps5 clean
all: ps5

ifdef PS5_PAYLOAD_SDK
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
endif

CFLAGS := -std=c11 -Wall -Wextra -O2 -Isrc
LDLIBS += -lSceSystemService -lpthread   # same DT_NEEDED list and order as BlueBridge

OBJS := $(patsubst src/%.c,$(BUILD)/%.o,$(SRCS))

ps5: $(ELF)

$(ELF): $(OBJS)
ifndef PS5_PAYLOAD_SDK
	$(error PS5_PAYLOAD_SDK is undefined)
endif
	@mkdir -p dist
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)
	PATH=$(PS5_PAYLOAD_SDK)/bin:$$PATH sh check_imports.sh $@ imports-1.0.2.txt || { rm -f $@; exit 1; }

$(BUILD)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf build dist
