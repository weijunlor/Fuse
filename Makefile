# FUSE build
#   make            build server and client (optimised, -O2)
#   make baseline   build the original lecturer programs, for "before" measurements
#   make baseline-O0  baseline at -O0 (or -O2, ...) with -g and frame pointers, for perf
#   make profile    -O2 with debug symbols and frame pointers, for perf
#   make debug      -O0 with ThreadSanitizer, for data-race checks
#   make LOG=debug  also print one line per network message (common/log.h, #17)
#   make bench      build every bench/bench_*.c (bench_particles in three builds, see below)
#   make clean      remove build/ and generated DDS code

CC      := gcc
CFLAGS  := -std=gnu11 -Wall -Wextra -O2 -Icommon -Igenerated -Ithird_party \
           $(shell pkg-config --cflags CycloneDDS glfw3 stb)
# Log level (#17, common/log.h): per-message debug lines are compiled out
# unless LOG=debug
LOG     ?= info
ifeq ($(LOG),debug)
CFLAGS  += -DLOG_LEVEL=LOG_LEVEL_DEBUG
else
CFLAGS  += -DLOG_LEVEL=LOG_LEVEL_INFO
endif
LDDDS   := $(shell pkg-config --libs CycloneDDS) -pthread
LDGL    := $(shell pkg-config --libs glfw3 stb) -lGL -lm
# Add -lcrypto (OpenSSL) or -lsodium here once common/crypto.c uses it

# DDS message types (idl/fuse.idl, v1 message set; see docs/PLAN.md section 2)
GEN     := generated/fuse.c
COMMON  := $(wildcard common/*.c) $(GEN)
BENCHES := $(patsubst bench/%.c,build/%,$(wildcard bench/bench_*.c))
# Particle update (#7, SIMD): optimised and vectorised for this machine's
# widest vector unit (AVX2 on the test laptop). -fno-trapping-math: the game
# never reads floating-point exception flags, and without it GCC will not turn
# the position clamps (x < 0 ? 0 : x) into vector selects, so the loop stays
# scalar. It does not change any result. Override with SIMD_FLAGS=...
SIMD_FLAGS ?= -O3 -march=native -fno-trapping-math

all: build/server build/client

# Regenerate DDS type code whenever the IDL changes. -x final: every struct is
# "final" (fixed layout, no XTypes extension header), which is what idlc picks
# by default today; saying so silences its warning that the default may change.
$(GEN): idl/fuse.idl
	mkdir -p generated && cd generated && idlc -x final -l c ../idl/fuse.idl

build/server: $(wildcard server/*.c) $(COMMON)
	mkdir -p build && $(CC) $(CFLAGS) $^ -o $@ $(LDDDS)

build/client: $(filter-out client/particles.c,$(wildcard client/*.c)) build/particles.o $(COMMON)
	mkdir -p build && $(CC) $(CFLAGS) $^ -o $@ $(LDDDS) $(LDGL)

# The particle system is the one file built with SIMD_FLAGS
build/particles.o: client/particles.c client/particles.h common/worker_pool.h
	mkdir -p build && $(CC) $(filter-out -O2,$(CFLAGS)) $(SIMD_FLAGS) -c $< -o $@

# Baseline uses its own copy of the IDL so it never changes when ours does
baseline: build/baseline_server build/baseline_client

build/baseline/messages.c: baseline/messages.idl
	mkdir -p build/baseline && cd build/baseline && idlc -l c ../../baseline/messages.idl

build/baseline_server: baseline/server.c build/baseline/messages.c
	$(CC) -O2 -Ibuild/baseline $^ -o $@ $(shell pkg-config --cflags CycloneDDS) $(LDDDS)

build/baseline_client: baseline/client.c build/baseline/messages.c
	$(CC) -O2 -Ibuild/baseline -Ithird_party $^ -o $@ \
		$(shell pkg-config --cflags CycloneDDS glfw3) $(LDDDS) $(LDGL)

# Baseline built for perf at a chosen optimisation level (make baseline-O0,
# make baseline-O2, ...): debug info and frame pointers so perf can unwind call stacks
baseline-O%: build/baseline-O%/server build/baseline-O%/client ;

build/baseline-O%/server: baseline/server.c build/baseline/messages.c
	mkdir -p $(@D) && $(CC) -O$* -g -fno-omit-frame-pointer -Ibuild/baseline $^ -o $@ \
		$(shell pkg-config --cflags CycloneDDS) $(LDDDS)

build/baseline-O%/client: baseline/client.c build/baseline/messages.c
	mkdir -p $(@D) && $(CC) -O$* -g -fno-omit-frame-pointer -Ibuild/baseline -Ithird_party $^ -o $@ \
		$(shell pkg-config --cflags CycloneDDS glfw3) $(LDDDS) $(LDGL)

bench: $(BENCHES) build/bench_particles_scalar build/bench_particles_sse2 build/dds_joiner

# Headless client for the join tests of #14 (bench/join_loss.sh): uses the
# game's DDS code (common/net.c) so it joins with exactly the client's QoS
build/dds_joiner: bench/dds_joiner.c common/net.c common/net.h $(GEN)
	mkdir -p build && $(CC) $(CFLAGS) bench/dds_joiner.c common/net.c $(GEN) -o $@ $(LDDDS)

build/bench_%: bench/bench_%.c
	mkdir -p build && $(CC) $(CFLAGS) $< -o $@ -pthread -lm

# bench_particles runs the client's particle code (client/particles.c), built
# three ways so each step of #7 is measured from the same source:
#   _scalar  -O2 with the vectoriser off (no SIMD; #6 aos vs soa is measured here)
#   _sse2    -O3 -fno-trapping-math for any x86-64: 128-bit SSE2, 4 floats at a time
#   (none)   SIMD_FLAGS, as the client uses (-march=native: 256-bit AVX2, 8 floats)
# -g and frame pointers so perf annotate/record can show the hot loop.
# The vectoriser report (which loops became SIMD) goes to build/*.vec.txt
# (GCC appends to it, so it is deleted first).
PART_SRC   := bench/bench_particles.c client/particles.c common/worker_pool.c common/timing.c
PART_DEPS  := $(PART_SRC) client/particles.h common/worker_pool.h
PART_FLAGS := $(filter-out -O2,$(CFLAGS)) -g -fno-omit-frame-pointer -Iclient

build/bench_particles_scalar: $(PART_DEPS)
	mkdir -p build && $(CC) $(PART_FLAGS) -O2 -fno-tree-vectorize $(PART_SRC) -o $@ -pthread -lm

build/bench_particles_sse2: $(PART_DEPS)
	mkdir -p build && rm -f $@.vec.txt && $(CC) $(PART_FLAGS) -O3 -fno-trapping-math -fopt-info-vec-all=$@.vec.txt $(PART_SRC) -o $@ -pthread -lm

build/bench_particles: $(PART_DEPS)
	mkdir -p build && rm -f $@.vec.txt && $(CC) $(PART_FLAGS) $(SIMD_FLAGS) -fopt-info-vec-all=$@.vec.txt $(PART_SRC) -o $@ -pthread -lm

profile: CFLAGS += -g -fno-omit-frame-pointer
profile: clean all

debug: CFLAGS := $(filter-out -O2,$(CFLAGS)) -O0 -g -fsanitize=thread
debug: LDDDS += -fsanitize=thread
debug: clean all

clean:
	rm -rf build generated/fuse.c generated/fuse.h generated/messages.c generated/messages.h

# Keep pattern-built baseline binaries (make would delete them as intermediates)
.PRECIOUS: build/baseline-O%/server build/baseline-O%/client

.PHONY: all baseline bench profile debug clean
