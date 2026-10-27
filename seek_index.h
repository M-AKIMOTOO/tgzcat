#ifndef TGZCAT_SEEK_INDEX_H
#define TGZCAT_SEEK_INDEX_H

/*
 * DEFLATE checkpoints follow Mark Adler's zran example:
 * https://github.com/madler/zlib/blob/master/examples/zran.c
 * This implementation streams checkpoints to disk instead of retaining an array.
 */
#define INDEX_SPAN (16u * 1024u * 1024u)
#define INDEX_WINDOW 32768u
#define INDEX_INPUT (256u * 1024u)
#define INDEX_NAME_MAX (1024u * 1024u)

struct index_point_disk { uint64_t in, out; uint32_t bits, length; };
struct index_member_disk { uint64_t offset, size; uint32_t length, type; };
struct index_footer { uint64_t total, points, members; };
_Static_assert(sizeof(struct index_point_disk) == 24, "index point layout");
_Static_assert(sizeof(struct index_member_disk) == 24, "index member layout");

struct index_point {
    struct index_point_disk disk;
    unsigned char window[INDEX_WINDOW];
};
struct index_builder {
    struct list_cache cache;
    uint64_t points, members, last;
};
struct index_match {
    struct list_cache_key key;
    uint64_t offset, size;
    int gzip;
    struct index_point point;
};

static void index_write(struct index_builder *builder, const void *data, size_t size) {
    if (!size) return;
    cache_encode(&builder->cache, data, size, Z_NO_FLUSH);
}

static void index_begin(struct index_builder *builder, const char *archive) {
    memset(builder, 0, sizeof *builder);
    struct list_cache *cache = &builder->cache;
    if (cache_key(archive, &cache->key) != 0) return;
    cache->path = cache_index_path_for(archive);
    if (!cache->path || cache_mkdir_parents(cache->path) != 0) return;
    size_t n = strlen(cache->path) + sizeof ".tmp.XXXXXX";
    cache->tmp = malloc(n);
    if (!cache->tmp) return;
    snprintf(cache->tmp, n, "%s.tmp.XXXXXX", cache->path);
    int fd = mkstemp(cache->tmp);
    if (fd < 0) { free(cache->tmp); cache->tmp = NULL; return; }
    cache->out = fdopen(fd, "wb");
    if (!cache->out) { close(fd); return; }
    cache->encoding = deflateInit2(&cache->encoder, Z_BEST_COMPRESSION, Z_DEFLATED,
                                  31, 8, Z_DEFAULT_STRATEGY) == Z_OK;
    if (!cache->encoding) { cache->failed = 1; return; }
    index_write(builder, &cache->key, sizeof cache->key);
    index_write(builder, "TGZIDX1", 8);
}

static void index_add_member(struct index_builder *builder, const char *name,
                             uint64_t offset, uint64_t size, char type) {
    if (!builder->cache.out) return;
    size_t n = strlen(name);
    if (n > INDEX_NAME_MAX) { builder->cache.failed = 1; return; }
    unsigned char tag = 2;
    struct index_member_disk member = {offset, size, (uint32_t)n, (unsigned char)type};
    index_write(builder, &tag, 1);
    index_write(builder, &member, sizeof member);
    index_write(builder, name, n);
    builder->members++;
}

static void index_end(struct index_builder *builder, const char *archive,
                      int complete, uint64_t total) {
    if (builder->cache.out && complete) {
        unsigned char tag = 0;
        struct index_footer footer = {total, builder->points, builder->members};
        index_write(builder, &tag, 1);
        index_write(builder, &footer, sizeof footer);
    }
    cache_end(&builder->cache, archive, complete);
}

/* Buffered sequential reader that also records restartable DEFLATE boundaries. */
struct seek_reader {
    FILE *file;
    z_stream stream;
    struct index_builder *builder;
    unsigned char input[INDEX_INPUT], output[INDEX_WINDOW];
    size_t position, length;
    uint64_t input_total, output_total, member_start, file_size;
    int gzip, raw, initialized, done, error;
    const char *message;
};

static int seek_fail(struct seek_reader *reader, const char *message) {
    reader->error = 1;
    reader->message = message;
    return -1;
}

static int seek_input(struct seek_reader *reader) {
    if (reader->stream.avail_in) return 1;
    size_t n = fread(reader->input, 1, sizeof reader->input, reader->file);
    if (!n) return ferror(reader->file) ? seek_fail(reader, "archive read error") : 0;
    reader->input_total += n;
    reader->stream.next_in = reader->input;
    reader->stream.avail_in = (uInt)n;
    return 1;
}

static void seek_checkpoint(struct seek_reader *reader) {
    struct index_builder *builder = reader->builder;
    if (!builder || reader->raw ||
        (reader->stream.data_type & 0xc0) != 0x80 ||
        (builder->points && reader->output_total - builder->last < INDEX_SPAN)) return;
    unsigned recent = INDEX_WINDOW - reader->stream.avail_out;
    uint64_t history = reader->output_total - reader->member_start;
    unsigned length = history < INDEX_WINDOW ? (unsigned)history : INDEX_WINDOW;
    struct index_point point = {0};
    point.disk.in = reader->input_total - reader->stream.avail_in;
    point.disk.out = reader->output_total;
    point.disk.bits = reader->stream.data_type & 7;
    point.disk.length = length;
    /* The output buffer is a circular 32 KiB history, including the previous fill. */
    unsigned copy = recent < length ? recent : length;
    memcpy(point.window + length - copy, reader->output + recent - copy, copy);
    copy = length - copy;
    if (copy) memcpy(point.window, reader->output + INDEX_WINDOW - copy, copy);
    unsigned char tag = 1;
    index_write(builder, &tag, 1);
    index_write(builder, &point.disk, sizeof point.disk);
    index_write(builder, point.window, length);
    builder->last = reader->output_total;
    builder->points++;
}

static int seek_fill(struct seek_reader *reader) {
    if (reader->error) return -1;
    if (reader->done) return 0;
    reader->position = reader->length = 0;
    if (!reader->gzip) {
        reader->length = fread(reader->output, 1, sizeof reader->output, reader->file);
        reader->output_total += reader->length;
        if (ferror(reader->file)) return seek_fail(reader, "archive read error");
        if (!reader->length) reader->done = 1;
        return reader->length ? 1 : 0;
    }
    reader->stream.next_out = reader->output;
    reader->stream.avail_out = sizeof reader->output;
    while (reader->stream.avail_out && !reader->done) {
        int input = seek_input(reader);
        if (input < 0) return -1;
        if (!input) return seek_fail(reader, "truncated gzip stream");
        unsigned before = reader->stream.avail_out;
        int ret = inflate(&reader->stream, reader->builder ? Z_BLOCK : Z_NO_FLUSH);
        reader->output_total += before - reader->stream.avail_out;
        if (ret != Z_OK && ret != Z_STREAM_END)
            return seek_fail(reader, "invalid gzip/DEFLATE stream");
        seek_checkpoint(reader);
        if (ret == Z_STREAM_END) {
            if (reader->raw) {
                /* The indexed prefix was already verified at build time. */
                unsigned drop = 8;
                while (drop) {
                    input = seek_input(reader);
                    if (input <= 0) return seek_fail(reader, "truncated gzip trailer");
                    unsigned n = drop < reader->stream.avail_in ? drop : reader->stream.avail_in;
                    reader->stream.next_in += n;
                    reader->stream.avail_in -= n;
                    drop -= n;
                }
            }
            input = seek_input(reader);
            if (input < 0) return -1;
            if (!input) reader->done = 1;
            else {
                if (inflateReset2(&reader->stream, 31) != Z_OK)
                    return seek_fail(reader, "cannot reset gzip decoder");
                reader->raw = 0;
                reader->member_start = reader->output_total;
            }
        }
    }
    reader->length = sizeof reader->output - reader->stream.avail_out;
    return reader->length ? 1 : 0;
}

static struct seek_reader *seek_open(const char *archive, struct index_builder *builder,
                                     const struct index_match *match) {
    struct seek_reader *reader = calloc(1, sizeof *reader);
    if (!reader) return NULL;
    reader->file = fopen(archive, "rb");
    if (!reader->file) { free(reader); return NULL; }
    struct stat st;
    if (fstat(fileno(reader->file), &st) != 0) goto fail;
    reader->file_size = (uint64_t)st.st_size;
    if (match && ((uint64_t)st.st_dev != match->key.dev ||
        (uint64_t)st.st_ino != match->key.ino || (uint64_t)st.st_size != match->key.size ||
        st.st_mtim.tv_sec != match->key.mtime_sec || st.st_mtim.tv_nsec != match->key.mtime_nsec ||
        st.st_ctim.tv_sec != match->key.ctime_sec || st.st_ctim.tv_nsec != match->key.ctime_nsec)) goto fail;
    if (match) reader->gzip = match->gzip;
    else {
        unsigned char magic[2];
        reader->gzip = fread(magic, 1, 2, reader->file) == 2 &&
            magic[0] == 0x1f && magic[1] == 0x8b;
        if (fseeko(reader->file, 0, SEEK_SET) != 0) goto fail;
    }
    reader->builder = builder;
    if (builder) {
        unsigned char kind = reader->gzip;
        index_write(builder, &kind, 1);
    }
    if (reader->gzip) {
        if (inflateInit2(&reader->stream, match ? -15 : 31) != Z_OK) goto fail;
        reader->initialized = 1;
        if (match) {
            const struct index_point *point = &match->point;
            if (fseeko(reader->file, (off_t)(point->disk.in - !!point->disk.bits),
                       SEEK_SET) != 0) goto fail;
            if (point->disk.bits) {
                int ch = fgetc(reader->file);
                if (ch == EOF || inflatePrime(&reader->stream, point->disk.bits,
                    ch >> (8 - point->disk.bits)) != Z_OK) goto fail;
            }
            if (point->disk.length && inflateSetDictionary(&reader->stream,
                point->window, point->disk.length) != Z_OK) goto fail;
            reader->input_total = point->disk.in;
            reader->output_total = point->disk.out;
            reader->raw = 1;
        }
    } else if (match) {
        if (fseeko(reader->file, (off_t)match->offset, SEEK_SET) != 0) goto fail;
        reader->output_total = match->offset;
    }
    return reader;
fail:
    if (reader->initialized) inflateEnd(&reader->stream);
    fclose(reader->file);
    free(reader);
    return NULL;
}

static int seek_read(struct seek_reader *reader, void *data, unsigned size) {
    unsigned done = 0;
    while (done < size) {
        if (reader->position == reader->length) {
            int ret = seek_fill(reader);
            if (ret < 0) return -1;
            if (!ret) break;
        }
        size_t n = reader->length - reader->position;
        if (n > size - done) n = size - done;
        memcpy((unsigned char *)data + done, reader->output + reader->position, n);
        reader->position += n;
        done += (unsigned)n;
    }
    return (int)done;
}

static int seek_skip(struct seek_reader *reader, uint64_t size) {
    if (!reader->gzip && size > reader->length - reader->position) {
        uint64_t available = reader->length - reader->position;
        uint64_t skip = size - available;
        if (reader->output_total > reader->file_size ||
            skip > reader->file_size - reader->output_total ||
            fseeko(reader->file, (off_t)skip, SEEK_CUR) != 0)
            return seek_fail(reader, "truncated tar payload");
        reader->output_total += skip;
        reader->position = reader->length = 0;
        return 0;
    }
    while (size) {
        if (reader->position == reader->length) {
            int ret = seek_fill(reader);
            if (ret <= 0) return seek_fail(reader, "truncated archive payload");
        }
        size_t n = reader->length - reader->position;
        if (n > size) n = (size_t)size;
        reader->position += n;
        size -= n;
    }
    return 0;
}

struct archive_reader {
    gzFile ordinary;
    struct seek_reader *indexed;
    uint64_t position;
};

static struct archive_reader *archive_open(const char *path, struct index_builder *builder,
                                           const struct index_match *match) {
    struct archive_reader *reader = calloc(1, sizeof *reader);
    if (!reader) return NULL;
    if (builder || match) {
        reader->indexed = seek_open(path, builder, match);
        if (!reader->indexed) { free(reader); return NULL; }
        if (match) {
            reader->position = match->gzip ? match->point.disk.out : match->offset;
            uint64_t skip = match->offset - reader->position;
            if (seek_skip(reader->indexed, skip) != 0) {
                if (reader->indexed->initialized) inflateEnd(&reader->indexed->stream);
                fclose(reader->indexed->file);
                free(reader->indexed);
                free(reader);
                return NULL;
            }
            reader->position = match->offset;
        }
    } else {
        reader->ordinary = gzopen(path, "rb");
        if (!reader->ordinary) { free(reader); return NULL; }
        gzbuffer(reader->ordinary, IO_BUFSZ);
    }
    return reader;
}

static int archive_read(struct archive_reader *reader, void *data, unsigned size) {
    int ret = reader->indexed ? seek_read(reader->indexed, data, size) :
        gzread(reader->ordinary, data, size);
    if (ret > 0) reader->position += (unsigned)ret;
    return ret;
}
static int archive_skip(struct archive_reader *reader, uint64_t size, unsigned char *buf) {
    if (reader->indexed) {
        if (seek_skip(reader->indexed, size) != 0) return -1;
    } else {
#ifdef TGZCAT_FAST_READER
        (void)buf;
        if (fast_gzskip(reader->ordinary, size) != 0) return -1;
#else
        uint64_t left = size;
        while (left) {
            unsigned n = (unsigned)(left > IO_BUFSZ ? IO_BUFSZ : left);
            int ret = gzread(reader->ordinary, buf, n);
            if (ret <= 0) return -1;
            left -= (unsigned)ret;
        }
#endif
    }
    reader->position += size;
    return 0;
}
static const char *archive_error(struct archive_reader *reader, int *code) {
    if (!reader->indexed) return gzerror(reader->ordinary, code);
    *code = reader->indexed->error ? Z_DATA_ERROR : Z_OK;
    return reader->indexed->message ? reader->indexed->message : "";
}
static void archive_close(struct archive_reader *reader) {
    if (!reader) return;
    if (reader->indexed) {
        if (reader->indexed->initialized) inflateEnd(&reader->indexed->stream);
        fclose(reader->indexed->file);
        free(reader->indexed);
    } else gzclose(reader->ordinary);
    free(reader);
}

static int index_exact(gzFile file, void *data, size_t size) {
    unsigned char *p = data;
    while (size) {
        unsigned n = (unsigned)(size > IO_BUFSZ ? IO_BUFSZ : size);
        int ret = gzread(file, p, n);
        if (ret <= 0) return 0;
        p += ret;
        size -= (unsigned)ret;
    }
    return 1;
}

struct index_listing {
    struct list_cache *cache;
    const char *filter;
    int first, found;
};
/* 0: miss/invalid cache; 1: valid index (and member found); 2: member absent. */
static int index_scan(const char *archive, const char *wanted, struct index_match *match,
                      struct index_listing *listing) {
    struct list_cache_key expected, key;
    if (cache_key(archive, &expected) != 0) return 0;
    char *path = cache_index_path_for(archive);
    if (!path) return 0;
    gzFile file = gzopen(path, "rb");
    free(path);
    if (!file) return 0;
    char magic[8];
    unsigned char kind;
    int result = 0, found = 0, have = 0, previous_valid = 0;
    uint64_t points = 0, members = 0, maximum = 0;
    struct index_point latest = {0}, previous = {0};
    if (!index_exact(file, &key, sizeof key) || memcmp(&key, &expected, sizeof key) ||
        !index_exact(file, magic, sizeof magic) || memcmp(magic, "TGZIDX1", 8) ||
        !index_exact(file, &kind, 1) || kind > 1) goto done;
    if (match) { memset(match, 0, sizeof *match); match->key = key; }
    for (;;) {
        unsigned char tag;
        if (!index_exact(file, &tag, 1)) break;
        if (tag == 1) {
            struct index_point next = {0};
            if (!kind || !index_exact(file, &next.disk, sizeof next.disk) ||
                next.disk.bits > 7 || next.disk.length > INDEX_WINDOW ||
                next.disk.length > next.disk.out || !next.disk.in ||
                next.disk.in > expected.size ||
                (!have && next.disk.out != 0) ||
                (have && (next.disk.out < latest.disk.out ||
                          next.disk.out - latest.disk.out < INDEX_SPAN)) ||
                !index_exact(file, next.window, next.disk.length)) break;
            previous = latest;
            previous_valid = have;
            latest = next;
            have = 1;
            points++;
        } else if (tag == 2) {
            struct index_member_disk member;
            if (!index_exact(file, &member, sizeof member) || member.length > INDEX_NAME_MAX ||
                member.offset > UINT64_MAX - member.size) break;
            char *name = malloc((size_t)member.length + 1);
            if (!name) break;
            if (!index_exact(file, name, member.length)) { free(name); break; }
            name[member.length] = '\0';
            const char *base = strrchr(name, '/');
            int regular = member.type == '0' || member.type == '7';
            int selected = wanted && !found && regular &&
                strcmp(strchr(wanted, '/') ? name : (base ? base + 1 : name), wanted) == 0;
            if (listing) {
                if (listing->cache && listing->cache->out) cache_add(listing->cache, name);
                if ((!listing->first || !listing->found) &&
                    (!listing->filter || strstr(name, listing->filter))) {
                    if (puts(name) == EOF) { free(name); result = -1; goto done; }
                    listing->found = 1;
                }
            }
            free(name);
            if (selected) {
                match->gzip = kind;
                match->offset = member.offset;
                match->size = member.size;
                if (kind) {
                    const struct index_point *point = have && latest.disk.out <= member.offset ?
                        &latest : (previous_valid && previous.disk.out <= member.offset ? &previous : NULL);
                    if (!point) break;
                    match->point = *point;
                }
                found = 1;
            }
            if (member.offset + member.size > maximum) maximum = member.offset + member.size;
            members++;
        } else if (tag == 0) {
            struct index_footer footer;
            unsigned char extra;
            int code;
            if (!index_exact(file, &footer, sizeof footer) ||
                footer.points != points || footer.members != members ||
                maximum > footer.total || (kind && (!have || latest.disk.out > footer.total)) ||
                (!kind && footer.total != expected.size)) break;
            int ret = gzread(file, &extra, 1);
            (void)gzerror(file, &code);
            if (ret != 0 || (code != Z_OK && code != Z_STREAM_END)) break;
            result = !wanted || found ? 1 : 2;
            break;
        } else break;
    }
done:
    gzclose(file);
    return result;
}
static int index_lookup(const char *archive, const char *wanted, struct index_match *match) {
    return index_scan(archive, wanted, match, NULL);
}
#endif
