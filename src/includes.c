/* ─────────────────────────── include files (v4) ───────────────────────────
 *
 * `incl <name>` at the start of a line splices another source file into the
 * translation unit:
 *
 *      incl <core>              →  ./core.lu   (extension added)
 *      incl <core.lu>           →  ./core.lu
 *      incl <X11/Xlib.h>        →  #include <X11/Xlib.h>   (C header)
 *
 * The search order is the same one Lu/Strict v5 uses, so a project works the
 * same way with either compiler:
 *
 *      directory of the including file
 *      .
 *      include
 *      ../include
 *      <dir of the luc binary>/include
 *      <dir of the luc binary>/../include
 *      $LU_INCLUDE (colon separated)
 *
 * A file is pulled in at most once, however many times it is named, and the
 * first line of an included file (`Lu/Language`) is dropped: it belongs to
 * the file, not to the program.  Expansion happens before lexing, so the
 * compiler proper never sees the directive.
 * ────────────────────────────────────────────────────────────────────────── */

/* readlink() lives behind the POSIX 2008 feature test macro, and realpath()
   behind XSI; both must be set before the first system header. */
#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include "lu.h"
#include <errno.h>
#include <limits.h>
#include <unistd.h>

#define INCL_MAX_DEPTH 32

typedef struct {
    char **seen;         /* canonical paths already pulled in */
    int    nseen, cseen;
} Incl;

typedef struct {
    char  *buf;
    size_t len, cap;
} Buf;

static void buf_put(Buf *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->len + n + 1) cap *= 2;
        b->buf = realloc(b->buf, cap);
        if (!b->buf) { fputs("out of memory\n", stderr); exit(1); }
        b->cap = cap;
    }
    memcpy(b->buf + b->len, s, n);
    b->len += n;
    b->buf[b->len] = '\0';
}

static void buf_puts(Buf *b, const char *s) { buf_put(b, s, strlen(s)); }

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char  *d = malloc(n);
    if (!d) { fputs("out of memory\n", stderr); exit(1); }
    memcpy(d, s, n);
    return d;
}

static bool file_readable(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

static char *read_whole_file(const char *p, size_t *out_len)
{
    FILE   *f = fopen(p, "rb");
    long    n;
    size_t  got;
    char   *s;

    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);
    s = malloc((size_t)n + 1);
    if (!s) { fclose(f); return NULL; }
    got = fread(s, 1, (size_t)n, f);
    fclose(f);
    s[got] = '\0';
    if (out_len) *out_len = got;
    return s;
}

static char *path_dir(const char *p)
{
    char *d = xstrdup(p ? p : ".");
    char *slash = strrchr(d, '/');
    if (slash && slash != d) *slash = '\0';
    else if (slash) d[1] = '\0';
    else { free(d); d = xstrdup("."); }
    return d;
}

static char *path_join(const char *dir, const char *name)
{
    size_t n = strlen(dir) + strlen(name) + 2;
    char  *s = malloc(n);
    if (!s) { fputs("out of memory\n", stderr); exit(1); }
    snprintf(s, n, "%s/%s", dir, name);
    return s;
}

static char *exe_dir(void)
{
    char    buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return NULL;
    buf[n] = '\0';
    return path_dir(buf);
}

/* Resolve `name` to a readable path, or NULL.  Byte-identical to the rules
   documented at the top of this file. */
static char *resolve(const char *from_file, const char *name)
{
    char *dirs[20];
    int   nd = 0, i;
    char *dir, *exe, *env, *found = NULL;
    char  ext[512];

    dir = path_dir(from_file);
    dirs[nd++] = dir;
    dirs[nd++] = xstrdup(".");
    dirs[nd++] = xstrdup("include");
    dirs[nd++] = xstrdup("../include");

    exe = exe_dir();
    if (exe) {
        dirs[nd++] = path_join(exe, "include");
        dirs[nd++] = path_join(exe, "../include");
        free(exe);
    }

    env = getenv("LU_INCLUDE");
    if (env && *env) {
        char *copy = xstrdup(env), *p = copy, *q;
        while (p && *p && nd < 18) {
            q = strchr(p, ':');
            if (q) *q = '\0';
            if (*p) dirs[nd++] = xstrdup(p);
            p = q ? q + 1 : NULL;
        }
        free(copy);
    }

    snprintf(ext, sizeof ext, "%s.lu", name);

    for (i = 0; i < nd && !found; i++) {
        char  *cand;
        size_t l = strlen(name);

        if (l > 3 && !strcmp(name + l - 3, ".lu")) {
            cand = path_join(dirs[i], name);
            if (file_readable(cand)) found = cand; else free(cand);
            continue;
        }
        cand = path_join(dirs[i], ext);
        if (file_readable(cand)) { found = cand; }
        else {
            free(cand);
            cand = path_join(dirs[i], name);
            if (file_readable(cand)) found = cand; else free(cand);
        }
    }

    for (i = 0; i < nd; i++) free(dirs[i]);
    return found;
}

static bool already_seen(Incl *st, const char *path)
{
    int i;
    for (i = 0; i < st->nseen; i++)
        if (!strcmp(st->seen[i], path)) return true;
    return false;
}

/* Include-once has to compare the same file reached by different spellings
   (`./core.lu`, `motion/../core.lu`), otherwise a diamond include turns into
   a duplicate definition in the compiled unit. */
static char *canonical(const char *p)
{
    char buf[PATH_MAX];
    if (realpath(p, buf)) return xstrdup(buf);
    return xstrdup(p);
}

static void remember(Incl *st, const char *path)
{
    if (st->nseen == st->cseen) {
        st->cseen = st->cseen ? st->cseen * 2 : 8;
        st->seen = realloc(st->seen, (size_t)st->cseen * sizeof *st->seen);
        if (!st->seen) { fputs("out of memory\n", stderr); exit(1); }
    }
    st->seen[st->nseen++] = xstrdup(path);
}

/* `Lu/Language` on the first line names the file's dialect; it is not part of
   the program the including file builds. */
static const char *skip_header(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    if (strncmp(s, "Lu/Language", 11) == 0) {
        const char *nl = strchr(s, '\n');
        return nl ? nl + 1 : s + strlen(s);
    }
    if (strncmp(s, "Lu/Strict", 9) == 0) return s;   /* caller rejects v5 */
    return s;
}

static bool expand(Incl *st, const char *from_file, const char *src,
                   int depth, Buf *out, char *err, size_t errsz);

/* Recognise `incl <name>` on one line.  Returns false when the line is not a
   directive.  `name` receives the text between the angle brackets. */
static bool match_incl(const char *line, size_t linelen, char *name, size_t namesz)
{
    size_t i = 0, j = 0;

    while (i < linelen && (line[i] == ' ' || line[i] == '\t')) i++;
    if (i + 4 > linelen || strncmp(line + i, "incl", 4) != 0) return false;
    i += 4;
    if (i >= linelen || (line[i] != ' ' && line[i] != '\t' && line[i] != '<'))
        return false;
    while (i < linelen && (line[i] == ' ' || line[i] == '\t')) i++;
    if (i >= linelen || line[i] != '<') return false;
    i++;
    while (i < linelen && line[i] != '>') {
        if (j + 1 >= namesz) return false;
        name[j++] = line[i++];
    }
    if (i >= linelen || line[i] != '>') return false;
    name[j] = '\0';
    return j > 0;
}

static bool expand(Incl *st, const char *from_file, const char *src,
                   int depth, Buf *out, char *err, size_t errsz)
{
    const char *p = src;
    int         line_no = 0;

    if (depth > INCL_MAX_DEPTH) {
        snprintf(err, errsz, "[luc] %s: include nesting deeper than %d — "
                              "is there a cycle?", from_file, INCL_MAX_DEPTH);
        return false;
    }

    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t      linelen = nl ? (size_t)(nl - p) : strlen(p);
        char        name[512];

        line_no++;

        if (match_incl(p, linelen, name, sizeof name)) {
            size_t l = strlen(name);

            /* `incl <foo.h>` — a C header, handed to the C backend as-is. */
            if (l > 2 && !strcmp(name + l - 2, ".h")) {
                buf_puts(out, "Import \"<");
                buf_puts(out, name);
                buf_puts(out, ">\"\n");
                p = nl ? nl + 1 : p + linelen;
                continue;
            }

            char *full = resolve(from_file, name);
            if (!full) {
                snprintf(err, errsz,
                         "[luc] %s:%d: incl <%s> — file not found "
                         "(looked next to the source, in ./include, "
                         "../include, $LU_INCLUDE and next to luc)",
                         from_file, line_no, name);
                return false;
            }
            char *real = canonical(full);
            free(full);
            full = real;
            if (already_seen(st, full)) {
                buf_puts(out, "// incl <");
                buf_puts(out, name);
                buf_puts(out, "> — already included\n");
                free(full);
                p = nl ? nl + 1 : p + linelen;
                continue;
            }
            remember(st, full);

            size_t  ilen;
            char   *isrc = read_whole_file(full, &ilen);
            if (!isrc) {
                snprintf(err, errsz, "[luc] %s:%d: cannot read '%s': %s",
                         from_file, line_no, full, strerror(errno));
                free(full);
                return false;
            }
            const char *body = skip_header(isrc);
            if (strncmp(body, "Lu/Strict", 9) == 0) {
                snprintf(err, errsz,
                         "[luc] %s:%d: incl <%s> is a Lu/Strict (v5) file — "
                         "v4 units can only include Lu/Language files",
                         from_file, line_no, name);
                free(isrc);
                free(full);
                return false;
            }

            buf_puts(out, "// ── incl <");
            buf_puts(out, name);
            buf_puts(out, ">  (");
            buf_puts(out, full);
            buf_puts(out, ") ──\n");

            if (!expand(st, full, body, depth + 1, out, err, errsz)) {
                free(isrc);
                free(full);
                return false;
            }

            buf_puts(out, "// ── end <");
            buf_puts(out, name);
            buf_puts(out, "> ──\n");

            free(isrc);
            free(full);
        } else {
            buf_put(out, p, linelen);
            if (nl) buf_put(out, "\n", 1);
        }

        p = nl ? nl + 1 : p + linelen;
    }

    return true;
}

/* Expand every `incl <...>` in `src`, which came from `path`.  Returns a
   malloc'd string, or NULL with a message in `err`. */
char *lu_expand_includes(const char *path, const char *src,
                         char *err, size_t errsz)
{
    Incl st = { 0 };
    Buf  out = { 0 };
    bool ok;

    err[0] = '\0';
    ok = expand(&st, path, src, 0, &out, err, errsz);

    for (int i = 0; i < st.nseen; i++) free(st.seen[i]);
    free(st.seen);

    if (!ok) {
        free(out.buf);
        return NULL;
    }
    if (!out.buf) return xstrdup("");
    return out.buf;
}
