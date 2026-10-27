#define _FILE_OFFSET_BITS 64
#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <zlib.h>
#include <sys/stat.h>
#include "reader.h"

#define TAR_BLOCK 512
#define IO_BUFSZ (1u << 20)
#include "cache.h"
#include "seek_index.h"

struct tar_header {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
};


static int archive_read_exact(struct archive_reader *gz, void *buf, size_t n) {
    unsigned char *p = buf;
    size_t off = 0;
    while (off < n) {
        unsigned want = (unsigned)((n - off) > (size_t)0x7fffffff ? 0x7fffffff : (n - off));
        int r = archive_read(gz, p + off, want);
        if (r < 0) return -1;
        if (r == 0) {
            int zerr = Z_OK;
            (void)archive_error(gz, &zerr);
            if (zerr != Z_OK && zerr != Z_STREAM_END) return -1;
            return off == 0 ? 0 : -2;
        }
        off += (size_t)r;
    }
    return 1;
}

static int all_zero(const unsigned char *p, size_t n) {
    for (size_t i = 0; i < n; i++) if (p[i] != 0) return 0;
    return 1;
}

static uint64_t parse_octal(const char *s, size_t n) {
    uint64_t v = 0;
    size_t i = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\0')) i++;
    for (; i < n; i++) {
        if (s[i] < '0' || s[i] > '7') break;
        v = (v << 3) + (uint64_t)(s[i] - '0');
    }
    return v;
}

static char *header_path(const struct tar_header *h) {
    size_t nl = strnlen(h->name, sizeof h->name);
    size_t pl = strnlen(h->prefix, sizeof h->prefix);
    size_t len = nl + (pl ? pl + 1 : 0);
    char *out = malloc(len + 1);
    if (!out) return NULL;
    if (pl) {
        memcpy(out, h->prefix, pl);
        out[pl] = '/';
        memcpy(out + pl + 1, h->name, nl);
    } else {
        memcpy(out, h->name, nl);
    }
    out[len] = '\0';
    return out;
}

static const char *base_name(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static int skip_bytes(struct archive_reader *gz, uint64_t n, unsigned char *buf) {
    return archive_skip(gz, n, buf);
}

static int finish_stream(struct archive_reader *gz, unsigned char *buf) {
    int ret;
    do { ret = archive_read(gz, buf, IO_BUFSZ); } while (ret > 0);
    if (ret < 0) return -1;
    int code = Z_OK;
    (void)archive_error(gz, &code);
    return code == Z_OK || code == Z_STREAM_END ? 0 : -1;
}
static char *read_payload_string(struct archive_reader *gz, uint64_t size, unsigned char *buf) {
    if (size > SIZE_MAX - 1) return NULL;
    char *s = malloc((size_t)size + 1);
    if (!s) return NULL;
    uint64_t left = size;
    size_t off = 0;
    while (left) {
        unsigned chunk = (unsigned)(left > IO_BUFSZ ? IO_BUFSZ : left);
        int r = archive_read(gz, s + off, chunk);
        if (r <= 0) { free(s); return NULL; }
        off += (size_t)r;
        left -= (uint64_t)r;
    }
    s[off] = '\0';
    if (off && s[off - 1] == '\0') {
        /* GNU longname usually includes trailing NUL. */
    } else {
        while (off && (s[off - 1] == '\n' || s[off - 1] == '\r')) s[--off] = '\0';
    }
    uint64_t pad = (TAR_BLOCK - (size % TAR_BLOCK)) % TAR_BLOCK;
    if (pad && skip_bytes(gz, pad, buf) != 0) { free(s); return NULL; }
    return s;
}

static char *pax_path_value(const char *data, size_t len) {
    size_t pos = 0;
    char *found = NULL;
    while (pos < len) {
        char *endp;
        errno = 0;
        long rec_len = strtol(data + pos, &endp, 10);
        if (errno || endp == data + pos || rec_len <= 0) break;
        size_t rl = (size_t)rec_len;
        if (rl > len - pos) break;
        const char *rec = data + pos;
        const char *space = memchr(rec, ' ', rl);
        if (!space) break;
        const char *kv = space + 1;
        size_t kvlen = rl - (size_t)(kv - rec);
        if (kvlen && kv[kvlen - 1] == '\n') kvlen--;
        const char key[] = "path=";
        if (kvlen >= sizeof(key) - 1 && memcmp(kv, key, sizeof(key) - 1) == 0) {
            free(found);
            found = malloc(kvlen - (sizeof(key) - 1) + 1);
            if (!found) return NULL;
            size_t vl = kvlen - (sizeof(key) - 1);
            memcpy(found, kv + sizeof(key) - 1, vl);
            found[vl] = '\0';
        }
        pos += rl;
    }
    return found;
}

static int write_payload(struct archive_reader *gz, uint64_t size, unsigned char *buf) {
    while (size) {
        unsigned chunk = (unsigned)(size > IO_BUFSZ ? IO_BUFSZ : size);
        int r = archive_read(gz, buf, chunk);
        if (r <= 0) return -1;
        size_t off = 0;
        while (off < (size_t)r) {
            ssize_t w = write(STDOUT_FILENO, buf + off, (size_t)r - off);
            if (w < 0) {
                if (errno == EINTR) continue;
                if (errno == EPIPE) return 1;
                return -1;
            }
            off += (size_t)w;
        }
        size -= (uint64_t)r;
    }
    return 0;
}

#include "cli.h"

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    struct options options;
    int parsed = parse_options(argc, argv, &options);
    if (parsed != 0) return parsed > 0 ? 0 : 2;
    if (options.release) return cache_release(options.archive, argv[0]);

    const int list = options.list;
    const int use_cache = options.cache;
    const int first_only = options.first;
    const char *archive = options.archive;
    const char *wanted = options.member;
    const char *filter = options.filter;
    const int basename_mode = wanted && strchr(wanted, '/') == NULL;
    int found = 0;

    struct list_cache cache = {0};
    struct index_builder index = {0};
    struct index_match selected;
    int index_status = use_cache ?
        index_lookup(archive, wanted, wanted ? &selected : NULL) : 0;
    if (!list && index_status == 2) {
        if (options.verbose) fprintf(stderr, "%s: index hit; member is absent\n", argv[0]);
        fprintf(stderr, "%s: \x27%s\x27 not found in \x27%s\x27\n", argv[0], wanted, archive);
        return 1;
    }
    if (!list && index_status == 1) {
        struct archive_reader *input = archive_open(archive, NULL, &selected);
        if (input) {
            if (options.verbose) fprintf(stderr,
                "%s: index hit; compressed offset=%" PRIu64 ", uncompressed offset=%" PRIu64
                ", skip=%" PRIu64 ", member bytes=%" PRIu64 "\n", argv[0],
                selected.gzip ? selected.point.disk.in : selected.offset,
                selected.gzip ? selected.point.disk.out : selected.offset,
                selected.gzip ? selected.offset - selected.point.disk.out : 0, selected.size);
            unsigned char *data = malloc(IO_BUFSZ);
            if (!data) { archive_close(input); return 1; }
            int ret = write_payload(input, selected.size, data);
            free(data);
            archive_close(input);
            if (ret < 0) fprintf(stderr, "%s: indexed extraction failed for %s\n", argv[0], wanted);
            return ret < 0 ? 1 : 0;
        }
        index_status = 0;
    }
    if (use_cache && list && cache_key(archive, &cache.key) == 0) {
        cache.path = cache_path_for(archive);
        if (cache.path) {
            int hit = first_only || index_status == 1 ?
                cache_read(&cache, filter, first_only, &found) : 0;
            if (hit) {
                if (options.verbose && hit > 0) fprintf(stderr, "%s: list cache hit\n", argv[0]);
                free(cache.path);
                if (hit < 0 && errno == EPIPE) return 0;
                if (hit < 0 && errno != EPIPE) {
                    fprintf(stderr, "%s: failed to read/write cached list\n", argv[0]);
                    return 1;
                }
                return filter && !found ? 1 : 0;
            }
        }
    }
    if (use_cache && list && index_status == 1) {
        if (cache.path && !first_only) cache_begin(&cache);
        struct index_listing listing = {.cache = &cache, .filter = filter, .first = first_only};
        int ret = index_scan(archive, NULL, NULL, &listing);
        if (fflush(stdout) != 0) ret = -1;
        int broken_pipe = errno == EPIPE;
        cache_end(&cache, archive, ret == 1);
        if (ret != 1) return broken_pipe ? 0 : 1;
        if (options.verbose) fprintf(stderr, "%s: listing from extraction index\n", argv[0]);
        return filter && !listing.found ? 1 : 0;
    }
    if (use_cache && !first_only && index_status == 0) index_begin(&index, archive);
    const int building = index.cache.out && index.cache.encoding && !index.cache.failed;
    if (options.verbose && building) fprintf(stderr, "%s: building extraction index (full archive scan)\n", argv[0]);
    struct archive_reader *gz = archive_open(archive, building ? &index : NULL, NULL);
    if (!gz) {
        cache_end(&cache, archive, 0);
        index_end(&index, archive, 0, 0);
        fprintf(stderr, "%s: cannot open '%s'\n", argv[0], archive);
        return 1;
    }

    unsigned char *buf = malloc(IO_BUFSZ);
    if (!buf) {
        fprintf(stderr, "%s: out of memory\n", argv[0]);
        cache_end(&cache, archive, 0);
        index_end(&index, archive, 0, 0);
        archive_close(gz);
        return 1;
    }

    if (cache.path && !first_only) cache_begin(&cache);
    unsigned char block[TAR_BLOCK];
    char *gnu_longname = NULL;
    char *pax_path = NULL;

    for (;;) {
        int rr = archive_read_exact(gz, block, TAR_BLOCK);
        if (rr == 0) break;
        if (rr < 0) {
            int zerr = 0;
            const char *msg = archive_error(gz, &zerr);
            fprintf(stderr, "%s: truncated/corrupt archive%s%s\n",
                    argv[0], msg && *msg ? ": " : "", msg && *msg ? msg : "");
            goto fail;
        }
        if (all_zero(block, TAR_BLOCK)) break;

        struct tar_header h;
        memcpy(&h, block, TAR_BLOCK);
        uint64_t size = parse_octal(h.size, sizeof h.size);
        char type = h.typeflag ? h.typeflag : '0';

        if (type == 'L') {
            free(gnu_longname);
            gnu_longname = read_payload_string(gz, size, buf);
            if (!gnu_longname) {
                fprintf(stderr, "%s: failed to read GNU longname\n", argv[0]);
                goto fail;
            }
            continue;
        }

        if (type == 'x') {
            char *pax = read_payload_string(gz, size, buf);
            if (!pax) {
                fprintf(stderr, "%s: failed to read PAX header\n", argv[0]);
                goto fail;
            }
            free(pax_path);
            pax_path = pax_path_value(pax, (size_t)size);
            free(pax);
            continue;
        }

        char *path = NULL;
        if (pax_path) {
            path = pax_path;
            pax_path = NULL;
        } else if (gnu_longname) {
            path = gnu_longname;
            gnu_longname = NULL;
        } else {
            path = header_path(&h);
            if (!path) {
                fprintf(stderr, "%s: out of memory\n", argv[0]);
                goto fail;
            }
        }

        if (building) index_add_member(&index, path, gz->position, size, type);
        if (cache.out) cache_add(&cache, path);
        if (list && (!filter || strstr(path, filter))) {
            if (puts(path) == EOF) {
                int broken_pipe = errno == EPIPE;
                free(path);
                cache_end(&cache, archive, 0);
                index_end(&index, archive, 0, 0);
                free(gnu_longname);
                free(pax_path);
                free(buf);
                archive_close(gz);
                return broken_pipe ? 0 : 1;
            }
            found = 1;
            if (first_only) {
                free(path);
                break;
            }
        }

        int is_regular = (type == '0' || type == '7');
        int match = 0;
        if (!list && !found && is_regular) {
            match = basename_mode ? strcmp(base_name(path), wanted) == 0
                                  : strcmp(path, wanted) == 0;
        }

        if (match) {
            found = 1;
            int wr = write_payload(gz, size, buf);
            if (wr < 0) {
                fprintf(stderr, "%s: read/write error while extracting %s\n", argv[0], path);
                free(path);
                goto fail;
            }
            if (!building || wr > 0) {
                free(path);
                cache_end(&cache, archive, 0);
                index_end(&index, archive, 0, 0);
                free(gnu_longname);
                free(pax_path);
                free(buf);
                archive_close(gz);
                return 0;
            }
        }

        if (!match && skip_bytes(gz, size, buf) != 0) {
            fprintf(stderr, "%s: truncated/corrupt payload for '%s'\n", argv[0], path);
            free(path);
            goto fail;
        }
        uint64_t pad = (TAR_BLOCK - (size % TAR_BLOCK)) % TAR_BLOCK;
        if (pad && skip_bytes(gz, pad, buf) != 0) {
            fprintf(stderr, "%s: truncated/corrupt padding after '%s'\n", argv[0], path);
            free(path);
            goto fail;
        }

        free(path);
    }

    if ((list || building) && !(first_only && found) && finish_stream(gz, buf) != 0) {
        int zerr;
        fprintf(stderr, "%s: corrupt/truncated gzip stream: %s\n",
                argv[0], archive_error(gz, &zerr));
        goto fail;
    }
    if (list && fflush(stdout) != 0) {
        int broken_pipe = errno == EPIPE;
        cache_end(&cache, archive, 0);
        index_end(&index, archive, 0, 0);
        free(gnu_longname);
        free(pax_path);
        free(buf);
        archive_close(gz);
        return broken_pipe ? 0 : 1;
    }
    cache_end(&cache, archive, 1);
    index_end(&index, archive, building, gz->position);
    if (!list && !found) {
        fprintf(stderr, "%s: '%s' not found in '%s'\n", argv[0], wanted, archive);
        goto fail;
    }

    free(gnu_longname);
    free(pax_path);
    free(buf);
    archive_close(gz);
    return filter && !found ? 1 : 0;

fail:
    cache_end(&cache, archive, 0);
    index_end(&index, archive, 0, 0);
    free(gnu_longname);
    free(pax_path);
    free(buf);
    archive_close(gz);
    return 1;
}

