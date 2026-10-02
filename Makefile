PREFIX   ?= /usr/local
CC       ?= cc
CFLAGS   ?= -O2
CFLAGS   += -std=gnu11 -Wall -Wextra -Wno-unused-parameter
LIBUSB_CFLAGS := $(shell pkg-config --cflags libusb-1.0)
LIBUSB_LIBS   := $(shell pkg-config --libs libusb-1.0)

UNAME := $(shell uname -s)

SRCS := src/main.c src/device.c src/ploytec.c src/midi.c
ifeq ($(UNAME),Darwin)
  SRCS   += src/port_coremidi.c
  LDLIBS += -framework CoreMIDI -framework CoreFoundation
else
  SRCS   += src/port_print.c
endif
LDLIBS += $(LIBUSB_LIBS) -lpthread

OBJS := $(SRCS:.c=.o)

all: djdriver

djdriver: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

src/%.o: src/%.c src/*.h
	$(CC) $(CFLAGS) $(LIBUSB_CFLAGS) -c -o $@ $<

tests/test_protocol: tests/test_protocol.c src/ploytec.c src/midi.c src/ploytec.h src/midi.h
	$(CC) $(CFLAGS) -o $@ tests/test_protocol.c src/ploytec.c src/midi.c -lpthread

test: tests/test_protocol
	./tests/test_protocol

install: djdriver
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 djdriver $(DESTDIR)$(PREFIX)/bin/djdriver

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/djdriver

clean:
	rm -f djdriver $(OBJS) tests/test_protocol

.PHONY: all test install uninstall clean
