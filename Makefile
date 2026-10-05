# Build the C version of the DCI tool.
# Requires libusb-1.0 (brew install libusb).

LIBUSB_PREFIX := $(shell brew --prefix libusb 2>/dev/null)
CFLAGS  := -O2 -Wall -Wextra -I$(LIBUSB_PREFIX)/include/libusb-1.0
LDFLAGS := -L$(LIBUSB_PREFIX)/lib -lusb-1.0

dci: dci.c
	$(CC) $(CFLAGS) dci.c -o dci $(LDFLAGS)

clean:
	rm -f dci

.PHONY: clean
