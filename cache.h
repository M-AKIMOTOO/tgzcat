#ifndef TGZCAT_CACHE_H
#define TGZCAT_CACHE_H

#include <dirent.h>
#include <limits.h>
#include <fcntl.h>

/* The metadata is uncompressed; the filename list is a zlib stream. */
struct list_cache_key {
    char magic[8];
    uint64_t dev, ino, size;
    int64_t mtime_sec, mtime_nsec, ctime_sec, ctime_nsec;
};
struct list_cache {
    struct list_cache_key key;
    FILE *out;
    char *path, *tmp;
    z_stream encoder;
    int encoding, failed;
};

static int cache_key(const char *archive, struct list_cache_key *key) {
    struct stat st;
    if (stat(archive, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
    memset(key, 0, sizeof *key);
    memcpy(key->magic, "TGZLST2", 8);
    key->dev = (uint64_t)st.st_dev;
    key->ino = (uint64_t)st.st_ino;
    key->size = (uint64_t)st.st_size;
    key->mtime_sec = st.st_mtim.tv_sec;
    key->mtime_nsec = st.st_mtim.tv_nsec;
    key->ctime_sec = st.st_ctim.tv_sec;
    key->ctime_nsec = st.st_ctim.tv_nsec;
    return 0;
}

static char *cache_join(const char *parent, const char *name) {
    size_t n = strlen(parent) + strlen(name) + 2;
    char *path = malloc(n);
    if (path) snprintf(path, n, "%s/%s", parent, name);
    return path;
}

static char *cache_directory(void) {
    const char *override = getenv("TGZCAT_CACHE_DIR");
    if (override && *override) return strdup(override);
    const char *xdg = getenv("XDG_CACHE_HOME");
    if (xdg && xdg[0] == '/') return cache_join(xdg, "tgzcat");
    const char *home = getenv("HOME");
    if (!home || home[0] != '/') { errno = ENOENT; return NULL; }
    return cache_join(home, ".cache/tgzcat");
}

/* Resolve the parent even if the archive was deleted, so release still works. */
static char *cache_archive_path(const char *archive) {
    char *path = realpath(archive, NULL);
    if (path) return path;
    if (errno != ENOENT) return NULL;
    char *copy = strdup(archive);
    if (!copy) return NULL;
    char *slash = strrchr(copy, '/');
    const char *name = slash ? slash + 1 : copy;
    if (slash) *slash = '\0';
    char *parent = realpath(slash ? (*copy ? copy : "/") : ".", NULL);
    if (parent) {
        path = cache_join(parent, name);
        free(parent);
    }
    free(copy);
    return path;
}

static char *cache_path_for(const char *archive) {
    char *canonical = cache_archive_path(archive);
    char *dir = cache_directory();
    if (!canonical || !dir) { free(canonical); free(dir); return NULL; }
    /* Two hashes identify the canonical path. Metadata independently validates hits. */
    uint64_t a = UINT64_C(14695981039346656037);
    uint64_t b = UINT64_C(7809847782465536322);
    for (const unsigned char *p = (const unsigned char *)canonical; *p; p++) {
        a = (a ^ *p) * UINT64_C(1099511628211);
        b = (b ^ *p) * UINT64_C(14029467366897019727);
    }
    char name[48];
    snprintf(name, sizeof name, "%016" PRIx64 "%016" PRIx64 ".list.z", a, b);
    char *path = cache_join(dir, name);
    free(canonical);
    free(dir);
    return path;
}

static int cache_mkdir_parents(const char *file) {
    char *path = strdup(file);
    if (!path) return -1;
    for (char *p = path + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(path, 0700) != 0 && errno != EEXIST) {
            free(path);
            return -1;
        }
        *p = '/';
    }
    free(path);
    return 0;
}

struct cache_search {
    const char *filter;
    int first, found;
    char *line;
    size_t length, capacity;
};

/* Return 2 after the first match, 1 to continue, -1 on output/allocation failure. */
static int cache_emit(struct cache_search *search, const unsigned char *data, size_t size) {
    if (!search->filter) {
        if (size && fwrite(data, 1, size, stdout) != size) return -1;
        if (size) search->found = 1;
        return 1;
    }
    while (size) {
        const unsigned char *end = memchr(data, '\n', size);
        size_t n = end ? (size_t)(end - data) + 1 : size;
        if (n > SIZE_MAX - search->length - 1) { errno = ENOMEM; return -1; }
        size_t needed = search->length + n + 1;
        if (needed > search->capacity) {
            size_t capacity = needed > SIZE_MAX / 2 ? needed : needed * 2;
            char *line = realloc(search->line, capacity);
            if (!line) return -1;
            search->line = line;
            search->capacity = capacity;
        }
        memcpy(search->line + search->length, data, n);
        search->length += n;
        search->line[search->length] = '\0';
        data += n;
        size -= n;
        if (end) {
            /* Exclude the separator from literal matching (TEXT may itself contain '\n'). */
            search->line[search->length - 1] = '\0';
            if (strstr(search->line, search->filter)) {
                search->line[search->length - 1] = '\n';
                if (fwrite(search->line, 1, search->length, stdout) != search->length) return -1;
                search->found = 1;
                if (search->first) return 2;
            }
            search->length = 0;
        }
    }
    return 1;
}
/* Return 1 for a valid complete stream, 0 for corrupt input, -1 for output failure. */
static int cache_decode(FILE *file, struct cache_search *search) {
    z_stream stream = {0};
    if (inflateInit(&stream) != Z_OK) return 0;
    unsigned char in[8192], out[8192];
    int result = 0;
    int input_eof = 0;
    for (;;) {
        if (!stream.avail_in && !input_eof) {
            size_t n = fread(in, 1, sizeof in, file);
            if (!n) {
                if (ferror(file)) break;
                input_eof = 1;
            }
            stream.next_in = in;
            stream.avail_in = (uInt)n;
        }
        stream.next_out = out;
        stream.avail_out = sizeof out;
        int ret = inflate(&stream, Z_NO_FLUSH);
        size_t n = sizeof out - stream.avail_out;
        if (search) {
            int emitted = cache_emit(search, out, n);
            if (emitted < 0) { result = -1; break; }
            if (emitted == 2) { result = 1; break; }
        }
        if (ret == Z_STREAM_END) {
            result = stream.avail_in == 0 && fgetc(file) == EOF && !ferror(file);
            break;
        }
        if (ret != Z_OK || (input_eof && !n)) break;
    }
    inflateEnd(&stream);
    return result;
}

static int cache_read(struct list_cache *cache, const char *filter, int first, int *found) {
    FILE *file = fopen(cache->path, "rb");
    if (!file) return 0;
    struct list_cache_key key;
    int result = 0;
    if (fread(&key, sizeof key, 1, file) == 1 &&
        memcmp(&key, &cache->key, sizeof key) == 0 &&
        cache_decode(file, NULL) == 1 &&
        fseeko(file, sizeof key, SEEK_SET) == 0) {
        /* Verify first, then emit: a corrupt cache must not precede a fresh listing. */
        struct cache_search search = {.filter = filter, .first = first};
        result = cache_decode(file, &search);
        *found = search.found;
        free(search.line);
        if (!result) { errno = EIO; result = -1; }
        if (fflush(stdout) != 0) result = -1;
    }
    int saved_errno = errno;
    fclose(file);
    errno = saved_errno;
    return result;
}

static void cache_encode(struct list_cache *cache, const void *data, size_t size, int flush) {
    if (!cache->out || !cache->encoding || cache->failed) return;
    const unsigned char *p = data;
    do {
        uInt chunk = (uInt)(size > UINT_MAX ? UINT_MAX : size);
        cache->encoder.next_in = (Bytef *)p;
        cache->encoder.avail_in = chunk;
        int ret;
        do {
            unsigned char out[8192];
            cache->encoder.next_out = out;
            cache->encoder.avail_out = sizeof out;
            ret = deflate(&cache->encoder, flush);
            size_t n = sizeof out - cache->encoder.avail_out;
            if ((ret != Z_OK && ret != Z_STREAM_END) ||
                fwrite(out, 1, n, cache->out) != n) {
                cache->failed = 1;
                return;
            }
        } while (ret != Z_STREAM_END && (cache->encoder.avail_in ||
                 cache->encoder.avail_out == 0 ||
                 flush == Z_FINISH));
        if (chunk) p += chunk;
        size -= chunk;
    } while (size);
}

static void cache_add(struct list_cache *cache, const char *name) {
    cache_encode(cache, name, strlen(name), Z_NO_FLUSH);
    cache_encode(cache, "\n", 1, Z_NO_FLUSH);
}

static void cache_begin(struct list_cache *cache) {
    if (cache_mkdir_parents(cache->path) != 0) return;
    size_t n = strlen(cache->path) + sizeof ".tmp.XXXXXX";
    cache->tmp = malloc(n);
    if (!cache->tmp) return;
    snprintf(cache->tmp, n, "%s.tmp.XXXXXX", cache->path);
    int fd = mkstemp(cache->tmp);
    if (fd < 0) { free(cache->tmp); cache->tmp = NULL; return; }
    cache->out = fdopen(fd, "wb");
    if (!cache->out) { close(fd); return; }
    cache->encoding = deflateInit(&cache->encoder, Z_BEST_COMPRESSION) == Z_OK;
    if (!cache->encoding ||
        fwrite(&cache->key, sizeof cache->key, 1, cache->out) != 1) cache->failed = 1;
}

static void cache_end(struct list_cache *cache, const char *archive, int complete) {
    if (cache->out) {
        struct list_cache_key current;
        int ok = complete && !cache->failed && cache->encoding &&
            cache_key(archive, &current) == 0 &&
            memcmp(&current, &cache->key, sizeof current) == 0;
        if (ok) cache_encode(cache, NULL, 0, Z_FINISH);
        if (cache->failed) ok = 0;
        if (cache->encoding) deflateEnd(&cache->encoder);
        if (fclose(cache->out) != 0) ok = 0;
        if (ok) (void)rename(cache->tmp, cache->path);
    }
    if (cache->tmp) unlink(cache->tmp);
    free(cache->tmp);
    free(cache->path);
    memset(cache, 0, sizeof *cache);
}

static int cache_release(const char *archive, const char *prog) {
    char *path = cache_path_for(archive);
    if (!path) {
        fprintf(stderr, "%s: cannot locate cache for %s: %s\n", prog, archive, strerror(errno));
        return 1;
    }
    char *slash = strrchr(path, '/');
    *slash = '\0';
    const char *name = slash + 1;
    size_t len = strlen(name);
    size_t removed = 0;
    int result = 0;
    DIR *dir = opendir(path);
    if (dir) {
        struct dirent *entry;
        errno = 0;
        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, name) != 0 &&
                !(strncmp(entry->d_name, name, len) == 0 &&
                  strncmp(entry->d_name + len, ".tmp.", 5) == 0)) continue;
            if (unlinkat(dirfd(dir), entry->d_name, 0) == 0) removed++;
            else if (errno != ENOENT) { result = 1; break; }
            errno = 0;
        }
        if (errno) result = 1;
        closedir(dir);
    } else if (errno != ENOENT) result = 1;
    free(path);
    /* Remove a recognized cache from versions that wrote beside this archive. */
    size_t n = strlen(archive) + sizeof ".tgzcat-list";
    char *legacy = malloc(n);
    if (legacy) {
        snprintf(legacy, n, "%s.tgzcat-list", archive);
        FILE *file = fopen(legacy, "rb");
        if (file) {
            char magic[8];
            int recognized = fread(magic, 1, sizeof magic, file) == sizeof magic &&
                memcmp(magic, "TGZLST1", sizeof magic) == 0;
            fclose(file);
            if (recognized) {
                if (unlink(legacy) == 0) removed++;
                else if (errno != ENOENT) result = 1;
            }
        } else if (errno != ENOENT) result = 1;
        free(legacy);
    }
    if (result) fprintf(stderr, "%s: could not release all caches for %s\n", prog, archive);
    else fprintf(stderr, "%s: released %zu cache file(s) for %s\n", prog, removed, archive);
    return result;
}
#endif
