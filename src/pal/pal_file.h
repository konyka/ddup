/* pal_file.h - thin cross-platform file IO wrapper over C stdio.
 *
 * stdio is already portable; this wrapper centralizes the few semantic
 * differences (e.g. rename-on-existing-target on Windows) behind the pal
 * boundary. Uses C streams (buffered); durability fsync is a documented
 * future addition.
 */
#ifndef DDUP_PAL_FILE_H
#define DDUP_PAL_FILE_H

#include <stddef.h>

typedef struct pal_file pal_file;

/* Open for appending (created if missing). NULL on error. */
pal_file *pal_file_open_append(const char *path);
/* Open for reading. NULL on error. */
pal_file *pal_file_open_read(const char *path);
/* Open for writing (truncates/creates). NULL on error. */
pal_file *pal_file_open_write(const char *path);

/* Bytes written/read, or -1 on error. */
ptrdiff_t pal_file_write(pal_file *f, const void *buf, size_t n);
ptrdiff_t pal_file_read(pal_file *f, void *buf, size_t n);
/* Flush the stdio buffer. 0 on success. */
int pal_file_flush(pal_file *f);
void pal_file_close(pal_file *f);

int pal_file_exists(const char *path);
/* Rename, replacing the target if it exists. 0 on success. */
int pal_file_rename(const char *from, const char *to);
int pal_file_unlink(const char *path);

#endif /* DDUP_PAL_FILE_H */
