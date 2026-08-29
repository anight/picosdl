# Host tests for picosdl.
#
# Everything in src/ is portable C - the hardware lives behind the backend
# interface - so the blitters, the arena and the event ring can be tested with a
# plain compiler and a sanitiser, no board required. This is also the first
# piece of the host backend PLAN.md phase 3 calls for.
#
#   make -C test        build and run
#   make -C test asan   the same, under AddressSanitizer and UBSan

CC     ?= cc
ROOT   := ..
CFLAGS := -std=c11 -Wall -Wextra -g -I $(ROOT)/include -I $(ROOT)/src
LIB    := $(wildcard $(ROOT)/src/psdl_*.c)
SRC    := test_picosdl.c host_backend.c $(LIB)
OUT    := /tmp/picosdl-test

# The generated game resources, and the reference the converter's output is
# checked against. Both come from PR/src/bin - see convert.py.
RESOURCES := $(ROOT)/../SDLPoP/src/resources.c
REFERENCE := $(ROOT)/../PR/src/bin/resources/reference.bin

# Which OPL emulator the music targets use, matching PICOPOP_OPL in
# picosdl/CMakeLists.txt. dbopl is what the firmware ships.
#   make music OPL=nuked
# C and C++ have to be compiled separately - resources.c uses C compound
# literals, which are not addressable rvalues in C++ - then linked with the C++
# driver when DBOPL is in play.
MUSIC_CSRC = host_backend.c $(ROOT)/demo/game_glue.c $(LIB) \
             $(ROOT)/../SDLPoP/src/midi.c $(RESOURCES)
MUSIC_INC  = -I $(ROOT)/include -I $(ROOT)/src -I $(ROOT)/demo \
             -I $(ROOT)/../SDLPoP/src
define build_music
	rm -rf $(1).objs && mkdir -p $(1).objs
	for f in $(2) $(MUSIC_CSRC); do \
	  $(CC) -std=c11 -O2 -g $(OPL_FLAGS) $(MUSIC_INC) -c $$f \
	    -o $(1).objs/$$(basename $$f .c).o || exit 1; done
	for f in $(OPL_SRC); do \
	  case $$f in \
	    *.cpp) $(CXX) -std=c++17 -O2 -g -fno-exceptions -fno-rtti $(OPL_FLAGS) \
	             $(MUSIC_INC) -c $$f -o $(1).objs/$$(basename $$f .cpp).o ;; \
	    *.c)   $(CC) -std=c11 -O2 -g $(OPL_FLAGS) $(MUSIC_INC) -c $$f \
	             -o $(1).objs/$$(basename $$f .c).o ;; \
	  esac || exit 1; done
	$(OPL_LINK) -O2 -o $(1) $(1).objs/*.o -lm
endef

OPL ?= dbopl
ifeq ($(OPL),dbopl)
  OPL_SRC   := $(ROOT)/../SDLPoP/src/dbopl.cpp $(ROOT)/../SDLPoP/src/dbopl_adapter.cpp
  OPL_FLAGS := -DPICOPOP_USE_DBOPL=1
  OPL_LINK  := $(CXX)
else
  OPL_SRC   := $(ROOT)/../SDLPoP/src/opl3.c
  OPL_FLAGS := -DPICOPOP_USE_DBOPL=0
  OPL_LINK  := $(CC)
endif

.PHONY: all asan verify music budget opl3 clean

all:
	$(CC) $(CFLAGS) -o $(OUT) $(SRC)
	$(OUT)

asan:
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o $(OUT)-asan $(SRC)
	$(OUT)-asan

# Check the build-time sprite conversion end to end: every sprite blitted from
# its const flash surface, mapped back through the global palette, and compared
# against the original BMPs. The scratch surface is bigger than any sprite, so
# the arena is enlarged for this build only - on the device nothing blits a
# 512x512 image.
verify: $(REFERENCE)
	$(CC) $(CFLAGS) -DPSDL_ARENA_BYTES=400000 -I $(ROOT)/../SDLPoP/src \
	    -o $(OUT)-verify verify_resources.c host_backend.c $(LIB) $(RESOURCES)
	$(OUT)-verify

# Render one of the game's Adlib tunes to a WAV, through the same midi.c and
# Nuked OPL3 the Pico runs. Checks the music path works and times the synthesis,
# which is the number PLAN.md's largest open question turns on.
#
#   make music              the title theme
#   make music ID=10056     the winning theme
ID  ?= 10054
WAV ?= /tmp/pop-music-$(ID).wav

music:
	$(call build_music,$(OUT)-music,render_music.c)
	$(OUT)-music $(ID) $(WAV)

# Render every tune in the game to the end and price the result under the
# encodings worth considering. Answers "what would pre-rendering the music cost
# in flash", which is the standing fallback if OPL3 is too expensive.
budget:
	$(call build_music,$(OUT)-budget,music_budget.c)
	$(OUT)-budget

# Check the OPL3 silent-slot optimisation against the stock emulator: render the
# same tune both ways and require the difference to stay inaudible. Guards the
# fidelity claim in opl3.c so it cannot silently regress.
OPL3_MIN_DB ?= 60

opl3:
	@for skip in 0 1; do \
	  $(CC) $(CFLAGS) -O2 -DOPL3_SKIP_SILENT_SLOTS=$$skip \
	    -I $(ROOT)/demo -I $(ROOT)/../SDLPoP/src \
	    -o $(OUT)-opl3-$$skip render_music.c host_backend.c \
	    $(ROOT)/demo/game_glue.c $(LIB) $(ROOT)/../SDLPoP/src/midi.c \
	    $(ROOT)/../SDLPoP/src/opl3.c $(RESOURCES) -lm || exit 1; \
	  echo "--- OPL3_SKIP_SILENT_SLOTS=$$skip ---"; \
	  $(OUT)-opl3-$$skip 10054 $(OUT)-opl3-$$skip.wav | grep -E "synthesis|RMS"; \
	done
	$(CC) $(CFLAGS) -o $(OUT)-wavdiff wav_diff.c -lm
	$(OUT)-wavdiff $(OUT)-opl3-0.wav $(OUT)-opl3-1.wav $(OPL3_MIN_DB)

$(REFERENCE):
	@echo "missing $@ - run: cd PR/src/bin && python3 dump_reference.py"
	@false

clean:
	rm -rf $(OUT) $(OUT)-asan $(OUT)-verify $(OUT)-music $(OUT)-budget \
	       $(OUT)-opl3-* $(OUT)-wavdiff $(OUT)-*.objs
