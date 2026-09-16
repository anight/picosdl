# Host tests for picosdl.
#
# Everything in src/ is portable C - the hardware lives behind the backend
# interface - so the blitters, the arena and the event ring can be tested with a
# plain compiler and a sanitiser, no board required. This is also the first
# piece of the host backend PLAN.md phase 3 calls for.
#
#   make -C test        build and run
#   make -C test asan   the same, under AddressSanitizer and UBSan
#
# These exercise picosdl alone and pull in no game: the tests that check Prince
# of Persia's own data - the sprites, the digi resampler, the mirroring and the
# music - live in picopop's tools/tests, because picosdl must not depend on any
# particular game.

CC     ?= cc
ROOT   := ..
# PSDL_HAVE_AUDIO: this backend implements the audio interface, so say so - see the
# note in picopop's tools/Makefile.
CFLAGS := -std=c11 -Wall -Wextra -g -DPSDL_HAVE_AUDIO=1 \
          -I $(ROOT)/include -I $(ROOT)/src
LIB    := $(wildcard $(ROOT)/src/psdl_*.c)
SRC    := test_picosdl.c host_backend.c $(LIB)
OUT    := /tmp/picosdl-test

.PHONY: all asan clean

all:
	$(CC) $(CFLAGS) -o $(OUT) $(SRC)
	$(OUT)

asan:
	$(MAKE) CFLAGS="$(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer" all

clean:
	rm -f $(OUT) $(OUT)-*
