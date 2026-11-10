/*
 * storage.h on POSIX: the program's files in the working directory, the
 * user's in data/ there, so an update that replaces the program leaves them.
 * Settings written by CLDuck sit beside the program; they are read from there
 * until written again.
 */
#define _DEFAULT_SOURCE /* fsync, fileno, sync */
#include "storage.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DATA "data"

static void *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *p = n >= 0 ? malloc((size_t)n + 1) : NULL;
    const int ok = p && fread(p, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    if (!ok) {
        free(p);
        return NULL;
    }
    p[n] = 0;
    if (size) *size = (size_t)n;
    return p;
}

/*
 * Writes a temporary file and renames it, so a crash or a power cut leaves the
 * old file or the new one. The data is on the flash before the rename: UBIFS
 * may otherwise keep the rename and lose the data, leaving an empty file.
 */
static int write_file(const char *path, const void *data, size_t len)
{
    char tmp[160];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    const int ok = fwrite(data, 1, len, f) == len && fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f) != 0 || !ok) {
        remove(tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) return -1;
    sync(); /* the rename too */
    return 0;
}

/* data/ and the directory of name in it. */
static void make_dirs(const char *name)
{
    char dir[160];
    mkdir(DATA, 0755);
    const char *slash = strrchr(name, '/');
    if (!slash) return;
    snprintf(dir, sizeof dir, DATA "/%.*s", (int)(slash - name), name);
    mkdir(dir, 0755);
}

/* Read whole into memory kept for the life of the program. */
const void *store_blob(const char *name, size_t *size)
{
    return read_file(name, size);
}

int store_kv_get(const char *key, char *buf, size_t size)
{
    char path[64];
    snprintf(path, sizeof path, DATA "/%s", key);
    FILE *f = fopen(path, "r");
    if (!f) f = fopen(key, "r");
    if (!f) return -1;
    const size_t n = fread(buf, 1, size - 1, f);
    fclose(f);
    buf[n] = 0;
    return (int)n;
}

int store_kv_set(const char *key, const char *text)
{
    char path[64];
    snprintf(path, sizeof path, DATA "/%s", key);
    make_dirs(key);
    return write_file(path, text, strlen(text));
}

int store_put(const char *name, const void *data, size_t len)
{
    char path[160];
    snprintf(path, sizeof path, DATA "/%s", name);
    make_dirs(name);
    return write_file(path, data, len);
}

void *store_get(const char *name, size_t *size)
{
    char path[160];
    snprintf(path, sizeof path, DATA "/%s", name);
    return read_file(path, size);
}

int store_remove(const char *name)
{
    char path[160];
    snprintf(path, sizeof path, DATA "/%s", name);
    return remove(path);
}

int store_list(const char *dir, char (*names)[32], int max)
{
    char path[160];
    snprintf(path, sizeof path, DATA "/%s", dir);
    DIR *d = opendir(path);
    if (!d) return 0;
    int n = 0;
    const struct dirent *e;
    while (n < max && (e = readdir(d))) {
        const size_t len = strlen(e->d_name);
        if (e->d_name[0] == '.' || len >= sizeof names[0] || (len > 4 && !strcmp(e->d_name + len - 4, ".tmp"))) continue;
        memcpy(names[n++], e->d_name, len + 1);
    }
    closedir(d);
    return n;
}
