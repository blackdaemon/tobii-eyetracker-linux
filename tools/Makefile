# Default: build against the unpacked vendor tree (no install needed).
TOBII_INC ?= ../vendor/stream_engine/include
TOBII_LIB ?= ../vendor/stream_engine/lib/x64

CFLAGS  ?= -O2 -g -Wall -Wextra
LDFLAGS += -L$(TOBII_LIB) -Wl,-rpath,$(abspath $(TOBII_LIB))
LDLIBS  += -ltobii_stream_engine -lpthread

all: tobii-gaze tobii-setup

tobii-gaze: tobii-gaze.c
	$(CC) $(CFLAGS) -I$(TOBII_INC) -o $@ $< $(LDFLAGS) $(LDLIBS)

tobii-setup: tobii-setup.c
	$(CC) $(CFLAGS) -I$(TOBII_INC) $(shell pkg-config --cflags gtk4) -o $@ $< \
		$(LDFLAGS) $(LDLIBS) $(shell pkg-config --libs gtk4) -lm

clean:
	rm -f tobii-gaze tobii-setup

.PHONY: all clean
