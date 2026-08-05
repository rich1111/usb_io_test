CC = gcc
CFLAGS = -Wall -Wextra -O2 $(shell pkg-config --cflags libusb-1.0)
LDFLAGS = $(shell pkg-config --libs libusb-1.0) -lpthread

# Automatically detect OS and set macOS version target if on Darwin
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    MAC_VER := $(shell sw_vers -productVersion | cut -d. -f1,2)
    CFLAGS += -mmacosx-version-min=$(MAC_VER)
endif

TARGET = usb_io_test_dodi
SRC = usb_io_test_dodi.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
