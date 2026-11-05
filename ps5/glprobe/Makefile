# GPU probe payload: make && send glprobe.elf to the PS5's ELF loader (port 9021)
PS5_HOST ?= ps5
PS5_PORT ?= 9021

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

ELF := glprobe.elf
CFLAGS := -Wall -O2 -g
LDLIBS := -lSceGLSlimVSH -lSceNotification

all: $(ELF)

$(ELF): main.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

test: $(ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $^

clean:
	rm -f $(ELF)
