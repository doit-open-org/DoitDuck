#ifndef STORAGE_H
#define STORAGE_H

/*
 * Files. Implementations: posix.c (the working directory for the program's
 * files, data/ in it for the user's).
 */
#include <stddef.h>

/* Read-only files shipped with the program (models, robot config), whole and NUL-terminated, or NULL. */
const void *store_blob(const char *name, size_t *size);

/* Small settings kept across restarts and updates, as text. Keys are short file names (≤ 15 chars). */
int store_kv_get(const char *key, char *buf, size_t size);  /* length, or -1 */
int store_kv_set(const char *key, const char *text);        /* 0 on success */

/* The user's files, such as "policies/kick.bin": kept across updates. */
int store_put(const char *name, const void *data, size_t len);   /* 0 on success; replaces a file of that name */
void *store_get(const char *name, size_t *size);                 /* whole and NUL-terminated, free() it; or NULL */
int store_remove(const char *name);                              /* 0 on success */
/* File names in a directory of them, such as "policies"; how many, up to max. */
int store_list(const char *dir, char (*names)[32], int max);

#endif
