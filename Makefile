CC = gcc
CFLAGS = -Wall -Wextra -O2 $(shell pkg-config --cflags libusb-1.0)
LDFLAGS = $(shell pkg-config --libs libusb-1.0) -lpthread

TARGET = usb_io_test_dodi
SRC = usb_io_test_dodi.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
