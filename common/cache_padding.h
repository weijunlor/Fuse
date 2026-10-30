/* common/cache_padding.h
Compile-time cache-line separation (#11), independent of handoff algorithms. */
#ifndef COMMON_CACHE_PADDING_H
#define COMMON_CACHE_PADDING_H

// make PADDING=0 keeps compact fields; PADDING=1 (default) separates writers.
#ifndef FUSE_CACHE_PADDING
#define FUSE_CACHE_PADDING 1
#endif
#if FUSE_CACHE_PADDING != 0 && FUSE_CACHE_PADDING != 1
#error "FUSE_CACHE_PADDING must be 0 or 1"
#endif

// Target cache-line size for the team's x86-64 machines, not runtime detection.
#define FUSE_CACHE_LINE_SIZE 64
#if FUSE_CACHE_PADDING
#define FUSE_CACHE_ALIGN _Alignas(FUSE_CACHE_LINE_SIZE)
#define FUSE_CACHE_PADDING_LABEL "FUSE_CACHE_PADDING=1"
#else
#define FUSE_CACHE_ALIGN
#define FUSE_CACHE_PADDING_LABEL "FUSE_CACHE_PADDING=0"
#endif

/* Preserve the actual compiled setting in binaries, including non-debug builds.
bench/setup_info.sh reads this marker rather than guessing from the environment.
The project uses GCC/Clang; used keeps an otherwise unreferenced string. */
static const char str_fuse_cache_padding_build[] __attribute__((used)) =
    FUSE_CACHE_PADDING_LABEL;

/* _Alignas increases a containing type's alignment and rounds its size, so
arrays of aligned types also have a whole-line stride. Static/automatic objects
honour it; heap users must use aligned_alloc, not ordinary malloc/calloc. */
#endif /* COMMON_CACHE_PADDING_H */
