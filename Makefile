# FUSE build
#   make            build server and client (optimised, -O2)
#   make baseline   build the original lecturer programs, for "before" measurements
#   make baseline-O0  baseline at -O0 (or -O2, ...) with -g and frame pointers, for perf
#   make profile    -O2 with debug symbols and frame pointers, for perf
#   make debug      -O0 with ThreadSanitizer, for data-race checks
#   make LOG=debug  also print one line per network message (common/log.h, #17)
#   make PADDING=0 compact shared fields; PADDING=1 (default) adds cache-line separation (#11)
#   make bench      build every bench/bench_*.c (bench_particles in three builds, see below)
#   make clean      remove build/ and generated DDS code

CC      := gcc
CFLAGS  := -std=gnu11 -Wall -Wextra -O2 -Icommon -Igenerated -Ithird_party \
           $(shell pkg-config --cflags CycloneDDS glfw3 stb libcrypto)
# Log level (#17, common/log.h): per-message debug lines are compiled out
# unless LOG=debug
LOG     ?= info
ifeq ($(LOG),debug)
CFLAGS  += -DLOG_LEVEL=LOG_LEVEL_DEBUG
else
CFLAGS  += -DLOG_LEVEL=LOG_LEVEL_INFO
endif
# Layout is a build-time choice shared by every translation unit. A content
# stamp below makes switching it rebuild existing targets, including particles.o.
PADDING ?= 1
ifneq ($(PADDING),0)
ifneq ($(PADDING),1)
$(error PADDING must be 0 or 1)
endif
endif
override CFLAGS += -DFUSE_CACHE_PADDING=$(PADDING)
PADDING_STAMP := build/.cache-padding
LDDDS   := $(shell pkg-config --libs CycloneDDS) -pthread
# Packaged Ubuntu plugins may live one directory below libddsc; source builds
# install them alongside it. Bake in the detected path, with a runtime override.
DDS_LIBDIR := $(shell pkg-config --variable=libdir CycloneDDS)
DDS_PLUGIN_DIR := $(shell if test -f "$(DDS_LIBDIR)/libddsc0debian/libdds_security_auth.so"; then printf '%s' "$(DDS_LIBDIR)/libddsc0debian"; else printf '%s' "$(DDS_LIBDIR)"; fi)
DDS_STAMP := build/.dds-config
# CFLAGS already has override origin from padding; retain it for this default too.
override CFLAGS += -DFUSE_DDS_PLUGIN_DIR_DEFAULT='"$(DDS_PLUGIN_DIR)"'
LDDDS += -Wl,-rpath,$(DDS_LIBDIR)
LDGL    := $(shell pkg-config --libs glfw3 stb) -lGL -lm
# Security preflight validates the identity key/certificate before discovery.
LDDDS += $(shell pkg-config --libs libcrypto)

# DDS message types (idl/fuse.idl, v1 message set; see docs/PLAN.md section 2)
GEN     := generated/fuse.c
COMMON  := $(wildcard common/*.c) $(GEN)
GAME_HEADERS := $(wildcard common/*.h client/*.h server/*.h) generated/fuse.h
BENCHES := $(patsubst bench/%.c,build/%,$(wildcard bench/bench_*.c))
# Particle update (#7, SIMD): optimised and vectorised for this machine's
# widest vector unit (AVX2 on the test laptop). -fno-trapping-math: the game
# never reads floating-point exception flags, and without it GCC will not turn
# the position clamps (x < 0 ? 0 : x) into vector selects, so the loop stays
# scalar. It does not change any result. Override with SIMD_FLAGS=...
SIMD_FLAGS ?= -O3 -march=native -fno-trapping-math

all: build/server build/client build/pairing

# Enrollment is a separate process: a failed pairing never stops an active match.
build/pairing: common/pairing_tool.c
	mkdir -p build && $(CC) $(CFLAGS) -DFUSE_PAIRING_TOOL $< -o $@ -pthread $(shell pkg-config --libs openssl)

build/test_pairing: tests/test_pairing.c common/pairing_tool.c
	mkdir -p build && $(CC) $(CFLAGS) $< -o $@ -pthread $(shell pkg-config --libs openssl)

build/test_pairing_ui: tests/test_pairing_ui.c client/pairing_ui.c client/pairing_ui.h client/ui.c
	mkdir -p build && $(CC) $(CFLAGS) -Iclient tests/test_pairing_ui.c client/ui.c -o $@ -pthread $(LDGL) $(shell pkg-config --libs libcrypto)

test-pairing: build/pairing build/test_pairing build/test_pairing_ui
	./build/test_pairing
	python3 security/scripts/test_client_session.py -v
	python3 security/scripts/test_pairing.py -v
.PHONY: test-pairing

# Deterministic game/lobby regressions; no window or running DDS server needed.
# Run in both thread handoff modes (sheeyang's #3/#4).
test: build/test_server build/test_input
	FUSE_HANDOFF=spsc ./build/test_server
	FUSE_HANDOFF=mutex ./build/test_server
	./build/test_input

build/test_input: tests/test_input.c client/input.c $(wildcard client/*.h common/*.h) generated/fuse.h
	mkdir -p build && $(CC) $(CFLAGS) tests/test_input.c -o $@ -pthread

test-network: build/server build/test_dds
	python3 tests/run_network.py

build/test_dds: tests/test_dds.c common/net.c common/dds_security.c $(GEN) $(wildcard common/*.h) generated/fuse.h
	mkdir -p build && $(CC) $(CFLAGS) $(filter %.c,$^) -o $@ $(LDDDS)

# test_server #includes server/game.c and server/lobby.c (to reach their statics)
TEST_SERVER_SRC := server/bot.c server/collision.c server/auth.c common/crypto.c \
	common/handoff.c common/net.c common/dds_security.c common/timing.c $(GEN)
build/test_server: tests/test_server.c server/game.c server/lobby.c $(TEST_SERVER_SRC) \
		$(wildcard server/*.h common/*.h) generated/fuse.h
	mkdir -p build && $(CC) $(CFLAGS) -Iserver tests/test_server.c $(TEST_SERVER_SRC) \
		-o $@ $(LDDDS) -lm

# Regenerate DDS type code whenever the IDL changes. -x final: every struct is
# "final" (fixed layout, no XTypes extension header), which is what idlc picks
# by default today; saying so silences its warning that the default may change.
$(GEN) generated/fuse.h &: idl/fuse.idl $(DDS_STAMP)
	mkdir -p generated && cd generated && idlc -x final -l c ../idl/fuse.idl

build/server: $(wildcard server/*.c) $(COMMON) $(GAME_HEADERS)
	mkdir -p build && $(CC) $(CFLAGS) $(filter %.c,$^) -o $@ $(LDDDS) -lm

build/client: $(filter-out client/particles.c,$(wildcard client/*.c)) build/particles.o $(COMMON) $(GAME_HEADERS)
	mkdir -p build && $(CC) $(CFLAGS) $(filter %.c %.o,$^) -o $@ $(LDDDS) $(LDGL)

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

bench: $(BENCHES) build/bench_particles_scalar build/bench_particles_sse2 \
	build/dds_joiner build/dds_players build/bench_collision build/bench_reduce \
	build/bench_collision_novec build/bench_collision_avx2

# Collision benchmarks align every loop to 64 bytes: without it, where GCC
# happens to place the hot query loop moved the same array-layout code
# between 19 and 28 ns per query on the P-cores (more branch misses when the
# loop starts mid-line), as large as the #8b effect being measured.
COLLISION_BENCH_FLAGS := -falign-loops=64

build/bench_collision: bench/bench_collision.c server/collision.c server/collision.h \
		common/timing.c common/timing.h
	mkdir -p build && $(CC) $(CFLAGS) $(COLLISION_BENCH_FLAGS) -Iserver bench/bench_collision.c \
		server/collision.c common/timing.c -o $@ -pthread -lm

# #8b: the same benchmark with vectorisation off, and for this machine's
# widest vectors (AVX2), to separate the ember layout gain from the SIMD gain
# (the server itself uses the plain -O2 build above: SSE2)
build/bench_collision_novec: bench/bench_collision.c server/collision.c server/collision.h \
		common/timing.c common/timing.h
	mkdir -p build && $(CC) $(CFLAGS) $(COLLISION_BENCH_FLAGS) -fno-tree-vectorize -Iserver bench/bench_collision.c \
		server/collision.c common/timing.c -o $@ -pthread -lm

build/bench_collision_avx2: bench/bench_collision.c server/collision.c server/collision.h \
		common/timing.c common/timing.h
	mkdir -p build && $(CC) $(CFLAGS) $(COLLISION_BENCH_FLAGS) -march=native -Iserver bench/bench_collision.c \
		server/collision.c common/timing.c -o $@ -pthread -lm

build/bench_reduce: bench/bench_reduce.c common/timing.c common/timing.h
	mkdir -p build && $(CC) $(CFLAGS) -g -fno-tree-vectorize bench/bench_reduce.c \
		common/timing.c -o $@ -pthread -lm

build/bench_branches: bench/bench_branches.c server/movement.h server/collision.h \
		common/timing.c common/timing.h
	mkdir -p build && $(CC) $(CFLAGS) -g -Iserver bench/bench_branches.c \
		common/timing.c -o $@ -pthread -lm

# Headless client for the join tests of #14 (bench/join_loss.sh): uses the
# game's DDS code (common/net.c) so it joins with exactly the client's QoS
APP_CLIENT_SRC := common/app_client.c common/crypto.c
build/dds_joiner: bench/dds_joiner.c common/net.c common/net.h common/dds_security.c common/dds_security.h $(APP_CLIENT_SRC) $(GEN)
	mkdir -p build && $(CC) $(CFLAGS) $(filter %.c,$^) -o $@ $(LDDDS)

# DDS security acceptance probes, each linked only to its own message schema.
build/dds_security_probe: bench/dds_security_probe.c common/net.c common/dds_security.c $(APP_CLIENT_SRC) $(GEN) common/dds_security.h
	mkdir -p build && $(CC) $(CFLAGS) $(filter %.c,$^) -o $@ $(LDDDS)

build/dds_security_probe_baseline: bench/dds_security_probe.c common/dds_security.c build/baseline/messages.c common/dds_security.h
	mkdir -p build && $(CC) $(CFLAGS) -Ibuild/baseline -DFUSE_PROBE_BASELINE $(filter %.c,$^) -o $@ $(LDDDS)

AUTH_TEST_SRC := bench/test_auth.c server/auth.c server/lobby.c server/game.c server/collision.c server/bot.c common/crypto.c common/app_client.c common/net.c common/dds_security.c common/timing.c common/handoff.c $(GEN)
build/test_auth: $(AUTH_TEST_SRC) $(GAME_HEADERS)
	mkdir -p build && $(CC) $(CFLAGS) -Iserver $(AUTH_TEST_SRC) -o $@ $(LDDDS) -lm

test-auth: build/test_auth
	./build/test_auth

build/auth_probe: bench/auth_probe.c common/net.c common/dds_security.c $(APP_CLIENT_SRC) $(GEN)
	mkdir -p build && $(CC) $(CFLAGS) $(filter %.c,$^) -o $@ $(LDDDS)

AUTH_WORKER_SRC := bench/auth_worker.c client/net_client.c common/net.c common/dds_security.c common/handoff.c common/timing.c $(APP_CLIENT_SRC) $(GEN)
build/auth_worker: $(AUTH_WORKER_SRC) $(GAME_HEADERS)
	mkdir -p build && $(CC) $(CFLAGS) -Iclient $(AUTH_WORKER_SRC) -o $@ $(LDDDS)

# Header-only queue/buffer stress tests, independent of DDS and graphics.
build/test_concurrency: bench/test_concurrency.c common/spsc_ring.h common/double_buffer.h common/worker_pool.h
	mkdir -p build && $(CC) $(CFLAGS) $< -o $@ -pthread

test-concurrency: build/test_concurrency build/test_auth
	./build/test_concurrency
	FUSE_HANDOFF=spsc ./build/test_auth
	FUSE_HANDOFF=mutex ./build/test_auth

TSAN_FLAGS := $(filter-out -O2,$(CFLAGS)) -O1 -g -fno-omit-frame-pointer -fsanitize=thread -fno-pie -no-pie
build/test_concurrency_tsan: bench/test_concurrency.c common/spsc_ring.h common/double_buffer.h common/worker_pool.h
	mkdir -p build && $(CC) $(TSAN_FLAGS) $< -o $@ -pthread

ASAN_FLAGS := $(filter-out -O2,$(CFLAGS)) -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-pie -no-pie
build/test_concurrency_asan: bench/test_concurrency.c common/spsc_ring.h common/double_buffer.h common/worker_pool.h
	mkdir -p build && $(CC) $(ASAN_FLAGS) $< -o $@ -pthread

build/test_auth_asan: $(AUTH_TEST_SRC) $(GAME_HEADERS)
	mkdir -p build && $(CC) $(ASAN_FLAGS) -Iserver $(AUTH_TEST_SRC) -o $@ $(LDDDS) -lm

build/concurrency-tsan/server: $(wildcard server/*.c) $(COMMON) $(GAME_HEADERS)
	mkdir -p $(@D) && $(CC) $(TSAN_FLAGS) $(filter %.c,$^) -o $@ $(LDDDS) -lm

build/concurrency-tsan/auth_worker: $(AUTH_WORKER_SRC) $(GAME_HEADERS)
	mkdir -p $(@D) && $(CC) $(TSAN_FLAGS) -Iclient $(AUTH_WORKER_SRC) -o $@ $(LDDDS)

# Instrument DDS as well: its custom atomics are invisible to TSan otherwise.
# Uses the pinned source already cached by security/scripts/setup_wsl.sh.
DDS_TSAN_SOURCE ?= $(HOME)/.cache/fuse/cyclonedds-0.10.5
concurrency-tsan-dds:
	cmake -S "$(DDS_TSAN_SOURCE)" -B build/tsan-dds -DCMAKE_BUILD_TYPE=Debug \
	    -DCMAKE_C_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" \
	    -DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=thread -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread -no-pie" \
	    -DBUILD_SHARED_LIBS=ON -DENABLE_SECURITY=ON -DENABLE_SSL=ON -DENABLE_SHM=OFF \
	    -DBUILD_IDLC=OFF -DBUILD_TESTING=OFF -DBUILD_EXAMPLES=OFF
	cmake --build build/tsan-dds --target ddsc dds_security_auth dds_security_ac dds_security_crypto --parallel 4

test-concurrency-integration: all build/auth_worker build/auth_probe build/dds_joiner build/dds_security_probe build/test_auth
	FUSE_HANDOFF=spsc python3 security/scripts/test_auth.py --output bench/results/concurrency-tests/spsc
	FUSE_HANDOFF=mutex python3 security/scripts/test_auth.py --output bench/results/concurrency-tests/mutex

test-auth-integration: all build/test_auth build/auth_probe build/auth_worker build/dds_joiner build/dds_security_probe
	python3 security/scripts/test_auth.py

# Headless test players for #2 and the game rules: snapshot gaps, phases
build/dds_players: bench/dds_players.c common/net.c common/net.h common/dds_security.c $(APP_CLIENT_SRC) $(GEN) $(GAME_HEADERS)
	mkdir -p build && $(CC) $(CFLAGS) $(filter %.c,$^) -o $@ $(LDDDS) -lm

build/bench_%: bench/bench_%.c
	mkdir -p build && $(CC) $(CFLAGS) $< -o $@ -pthread -lm

# Keep both false-sharing binaries side by side, with identical flags except #11.
# Filter out the global setting so these targets never depend on PADDING's value.
build/bench_false_share: bench/bench_false_share.c common/cache_padding.h common/spsc_ring.h
	mkdir -p build && $(CC) $(CFLAGS) -g -fno-omit-frame-pointer $< -o $@ -pthread

build/bench_false_share_0 build/bench_false_share_1: build/bench_false_share_%: bench/bench_false_share.c common/cache_padding.h common/spsc_ring.h
	mkdir -p build && $(CC) $(filter-out -DFUSE_CACHE_PADDING=%,$(CFLAGS)) -DFUSE_CACHE_PADDING=$* -g -fno-omit-frame-pointer $< -o $@ -pthread

bench-padding: build/bench_false_share_0 build/bench_false_share_1

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

# Only touch this stamp when the selected layout changes. A no-op rebuild stays
# a no-op; switching 1 -> 0 -> 1 invalidates every object containing shared types.
$(PADDING_STAMP): FORCE_PADDING
	@mkdir -p build
	@printf '%s\n' '$(PADDING)' > $@.tmp
	@cmp -s $@.tmp $@ || cp $@.tmp $@
	@rm -f $@.tmp

PADDING_TARGETS := build/server build/client build/particles.o $(BENCHES) \
    build/bench_particles_scalar build/bench_particles_sse2 build/dds_joiner \
    build/dds_security_probe build/dds_security_probe_baseline build/auth_probe \
    build/auth_worker build/dds_players build/test_auth build/test_auth_asan build/test_concurrency \
    build/test_concurrency_tsan build/test_concurrency_asan \
    build/concurrency-tsan/server build/concurrency-tsan/auth_worker \
    build/test_server build/test_input build/test_dds
$(PADDING_TARGETS): $(PADDING_STAMP) common/cache_padding.h

# Switching between the Ubuntu DDS package and the pinned local SDK changes
# headers, generated types, plugin paths and the linked SONAME. Source mtimes
# alone cannot detect that change. Refresh this signature on every make, but
# only touch the stamp when the selected SDK actually changes (as for padding).
$(DDS_STAMP): FORCE_DDS
	@mkdir -p build
	@printf '%s\n' '$(shell pkg-config --modversion CycloneDDS)' \
	    '$(DDS_LIBDIR)' '$(DDS_PLUGIN_DIR)' \
	    '$(shell pkg-config --cflags --libs CycloneDDS)' \
	    '$(shell command -v idlc)' > $@.tmp
	@cmp -s $@.tmp $@ || cp $@.tmp $@
	@rm -f $@.tmp

build/server build/client build/particles.o: $(DDS_STAMP)

.PHONY: all baseline bench bench-padding profile debug clean test test-network test-auth test-auth-integration test-concurrency test-concurrency-integration concurrency-tsan-dds FORCE_PADDING FORCE_DDS
