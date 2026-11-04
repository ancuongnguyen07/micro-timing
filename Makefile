# RDPRU micro-benchmark: C driver + assembly (AT&T) measurement loops
#
#   make          build
#   make run      build and run
#   make dis      disassemble bench_body (check the timed instructions)
#   make clean

CC      := gcc
# EXTRA_CFLAGS: per-build overrides, e.g. make EXTRA_CFLAGS=-DSIBLING_LOAD=2
# (run `make clean` first when changing it; make does not track it)
EXTRA_CFLAGS ?=
CFLAGS  := -O2 -g -Wall -Wextra -std=gnu11 -pthread -I include -MMD -MP $(EXTRA_CFLAGS)
ASFLAGS := -g -I include -MMD -MP
LDFLAGS := -pthread
LDLIBS  := -lm

CSRC := $(wildcard src/*.c)
SSRC := $(wildcard src/*.S)
OBJ  := $(patsubst src/%.c,build/%.o,$(CSRC)) $(patsubst src/%.S,build/%.o,$(SSRC))
BIN  := build/bench

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -c -o $@ $<

# capital .S: gcc runs the C preprocessor first, so #include/#define work
build/%.o: src/%.S | build
	$(CC) $(ASFLAGS) -c -o $@ $<

build:
	mkdir -p build

run: $(BIN)
	@./$(BIN)

dis: $(BIN)
	@objdump -d --no-show-raw-insn $(BIN) | sed -n '/<bench_body>:/,/^$$/p'

clean:
	rm -rf build

-include $(OBJ:.o=.d)
.PHONY: all run dis clean
