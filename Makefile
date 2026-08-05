CC = gcc
CFLAGS = -Wall -Wextra -O2 $(shell pkg-config --cflags libusb-1.0)
LDFLAGS = $(shell pkg-config --libs libusb-1.0) -lpthread

# Automatically detect OS and set macOS version target if on Darwin
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    MAC_VER := $(shell sw_vers -productVersion | cut -d. -f1,2)
    CFLAGS += -mmacosx-version-min=$(MAC_VER)
endif

TARGETS = usb_io_test_dodi usb_io_test_ai
SRC_DODI = usb_io_test_dodi.c
SRC_AI = usb_io_test_ai.c

all: $(TARGETS)

usb_io_test_dodi: $(SRC_DODI)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

usb_io_test_ai: $(SRC_AI)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(TARGETS)

.PHONY: all clean
