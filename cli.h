#ifndef TGZCAT_CLI_H
#define TGZCAT_CLI_H

struct options {
    const char *archive, *member, *filter;
    int list, cache, release, first, verbose;
};

static void usage(FILE *out, const char *prog) {
    fprintf(out,
        "Usage:\n"
        "  %s ARCHIVE [MEMBER] [OPTIONS]\n"
        "\n"
        "  ARCHIVE          List archive member names.\n"
        "  ARCHIVE MEMBER   Write one member to standard output.\n"
        "                   Match the full path, or the basename if MEMBER has no '/'.\n"
        "                   If names repeat, extract the first match.\n"
        "\n"
        "Options (may appear before or after ARCHIVE):\n"
        "  -l, --list           List names explicitly (the default without MEMBER).\n"
        "  -f, --find TEXT      List names containing TEXT (literal, case-sensitive).\n"
        "  -1, --first TEXT     List only the first name containing TEXT.\n"
        "  -c, --cache          Cache names and seek positions for listing/search/extraction.\n"
        "      --cache release  Remove only the specified archive's cache.\n"
        "  -h, --help           Show this help and examples.\n"
        "  -v, --verbose        Report cache hits and extraction restart positions.\n"
        "      --               Treat remaining arguments as paths, even if they start '-'.\n"
        "\n"
        "Examples:\n"
        "  %s data.tar.gz\n"
        "  %s data.tgz experiment.DRG\n"
        "  %s data.tar.gz dir/scan.cor > scan.cor\n"
        "  %s data.tar.gz --find YAMAGU32_YAMAGU34\n"
        "  %s data.tar.gz --first .DRG\n"
        "  %s data.tar.gz --cache --find YAMAGU32_YAMAGU34\n"
        "  %s data.tar.gz --cache release\n"
        "\n"
        "Notes:\n"
        "  ARCHIVE may be .tar.gz, .tgz, or uncompressed .tar.\n"
        "  MEMBER writes file contents; --find/--first write filenames.\n"
        "  --cache builds an index on the first full scan; later extractions seek near MEMBER.\n"
        "  Cache is opt-in; compressed lists and restart dictionaries live in ~/.cache/tgzcat/.\n"
        "  --first uses an existing cache; on a miss it stops early without building one.\n"
        "  Exit status: 0 success, 1 no match/I/O error, 2 invalid arguments.\n",
        prog, prog, prog, prog, prog, prog, prog, prog);
}

static int cli_error(const char *prog, const char *message) {
    fprintf(stderr, "%s: %s\nTry '%s --help' for usage and examples.\n",
            prog, message, prog);
    return -1;
}

/* Return 0 for parsed arguments, 1 for help, -1 for an argument error. */
static int parse_options(int argc, char **argv, struct options *options) {
    int positional_only = 0, explicit_list = 0;
    memset(options, 0, sizeof *options);
    if (argc == 1) {
        usage(stdout, argv[0]);
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (!positional_only && strcmp(arg, "--") == 0) {
            positional_only = 1;
            continue;
        }
        if (!positional_only && (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0)) {
            usage(stdout, argv[0]);
            return 1;
        }
        if (!positional_only && (strcmp(arg, "-v") == 0 || strcmp(arg, "--verbose") == 0)) {
            options->verbose = 1;
            continue;
        }
        if (!positional_only && (strcmp(arg, "-l") == 0 || strcmp(arg, "--list") == 0)) {
            explicit_list = 1;
            continue;
        }
        if (!positional_only && (strcmp(arg, "-c") == 0 || strcmp(arg, "--cache") == 0 ||
                                strcmp(arg, "--cache=release") == 0)) {
            options->cache = 1;
            if (strcmp(arg, "--cache=release") == 0) options->release = 1;
            else if (i + 1 < argc && strcmp(argv[i + 1], "release") == 0) {
                options->release = 1;
                i++;
            }
            continue;
        }
        int find = !positional_only &&
            (strcmp(arg, "-f") == 0 || strcmp(arg, "--find") == 0 ||
             strncmp(arg, "--find=", 7) == 0);
        int first = !positional_only &&
            (strcmp(arg, "-1") == 0 || strcmp(arg, "--first") == 0 ||
             strncmp(arg, "--first=", 8) == 0);
        if (find || first) {
            if (options->filter)
                return cli_error(argv[0], "use --find or --first only once");
            const char *value = strchr(arg, '=');
            if (value) value++;
            else {
                if (++i == argc)
                    return cli_error(argv[0], "--find/--first requires a TEXT argument");
                value = argv[i];
            }
            options->filter = value;
            options->first = first;
            continue;
        }
        if (!positional_only && arg[0] == '-' && arg[1]) {
            fprintf(stderr, "%s: unknown option '%s'\n", argv[0], arg);
            return cli_error(argv[0], "use -- before a path that starts with '-'");
        }
        if (!options->archive) options->archive = arg;
        else if (!options->member) options->member = arg;
        else return cli_error(argv[0], "expected one ARCHIVE and at most one MEMBER");
    }
    if (!options->archive) return cli_error(argv[0], "an ARCHIVE path is required");
    if (options->release) {
        if (options->member || options->filter || explicit_list)
            return cli_error(argv[0], "--cache release accepts only an ARCHIVE path");
        return 0;
    }
    if (options->member && (options->filter || explicit_list))
        return cli_error(argv[0], "MEMBER extraction cannot be combined with --list, --find or --first");
    options->list = options->member == NULL;
    return 0;
}
#endif
