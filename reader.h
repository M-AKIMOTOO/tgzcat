#ifndef TGZCAT_READER_H
#define TGZCAT_READER_H

/* C entry points for the bounded, streaming zlib-rs reader. */
#ifdef TGZCAT_FAST_READER
typedef struct fast_reader *fast_gzFile;
fast_gzFile fast_gzopen(const char *path, const char *mode);
int fast_gzread(fast_gzFile reader, void *data, unsigned size);
int fast_gzskip(fast_gzFile reader, uint64_t size);
int fast_gzfinish(fast_gzFile reader);
const char *fast_gzerror(fast_gzFile reader, int *code);
int fast_gzclose(fast_gzFile reader);
static inline int fast_gzbuffer(fast_gzFile reader, unsigned size) {
    (void)reader;
    (void)size;
    return 0;
}
#ifdef gzopen
#undef gzopen
#endif
#define gzFile fast_gzFile
#define gzopen fast_gzopen
#define gzread fast_gzread
#define gzbuffer fast_gzbuffer
#define gzerror fast_gzerror
#define gzclose fast_gzclose
#endif
#endif
