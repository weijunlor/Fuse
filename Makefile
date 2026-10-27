# FUSE build
#   make            build server and client (optimised, -O2)
#   make baseline   build the original lecturer programs, for "before" measurements
#   make profile    -O2 with debug symbols and frame pointers, for perf
#   make debug      -O0 with ThreadSanitizer, for data-race checks
#   make bench      build every bench/bench_*.c
#   make clean      remove build/ and generated DDS code

CC      := gcc
CFLAGS  := -std=gnu11 -Wall -Wextra -O2 -Icommon -Igenerated -Ithird_party \
           $(shell pkg-config --cflags CycloneDDS glfw3)
LDDDS   := $(shell pkg-config --libs CycloneDDS) -pthread
LDGL    := $(shell pkg-config --libs glfw3) -lGL -lm
# Add -lcrypto (OpenSSL) or -lsodium here once common/crypto.c uses it

GEN     := generated/messages.c
COMMON  := $(wildcard common/*.c) $(GEN)
BENCHES := $(patsubst bench/%.c,build/%,$(wildcard bench/bench_*.c))

all: build/server build/client

# Regenerate DDS type code whenever the IDL changes
$(GEN): idl/messages.idl
	mkdir -p generated && cd generated && idlc -l c ../idl/messages.idl

build/server: $(wildcard server/*.c) $(COMMON)
	mkdir -p build && $(CC) $(CFLAGS) $^ -o $@ $(LDDDS)

build/client: $(wildcard client/*.c) $(COMMON)
	mkdir -p build && $(CC) $(CFLAGS) $^ -o $@ $(LDDDS) $(LDGL)

# Baseline uses its own copy of the IDL so it never changes when ours does
baseline: build/baseline_server build/baseline_client

build/baseline/messages.c: baseline/messages.idl
	mkdir -p build/baseline && cd build/baseline && idlc -l c ../../baseline/messages.idl

build/baseline_server: baseline/server.c build/baseline/messages.c
	$(CC) -O2 -Ibuild/baseline $^ -o $@ $(shell pkg-config --cflags CycloneDDS) $(LDDDS)

build/baseline_client: baseline/client.c build/baseline/messages.c
	$(CC) -O2 -Ibuild/baseline -Ithird_party $^ -o $@ \
		$(shell pkg-config --cflags CycloneDDS glfw3) $(LDDDS) $(LDGL)

bench: $(BENCHES)

build/bench_%: bench/bench_%.c
	mkdir -p build && $(CC) $(CFLAGS) $< -o $@ -pthread -lm

profile: CFLAGS += -g -fno-omit-frame-pointer
profile: clean all

debug: CFLAGS := $(filter-out -O2,$(CFLAGS)) -O0 -g -fsanitize=thread
debug: LDDDS += -fsanitize=thread
debug: clean all

clean:
	rm -rf build generated/messages.c generated/messages.h

.PHONY: all baseline bench profile debug clean
