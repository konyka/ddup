/* pal_file.h - thin cross-platform file IO wrapper over C stdio.
 *
 * stdio is already portable; this wrapper centralizes the few semantic
 * differences (e.g. rename-on-existing-target on Windows) behind the pal
 * boundary. Uses C streams (buffered); pal_file_sync() adds durability
 * (fsync / FlushFileBuffers) for the AOF appendfsync policy.
 */
#ifndef DDUP_PAL_FILE_H
#define DDUP_PAL_FILE_H

#include <stddef.h>
#include <stdint.h>

typedef struct pal_file pal_file;

/* Open for appending (created if missing). NULL on error. */
pal_file *pal_file_open_append(const char *path);
/* Open for reading. NULL on error. */
pal_file *pal_file_open_read(const char *path);
/* Open for writing (truncates/creates). NULL on error. */
pal_file *pal_file_open_write(const char *path);
/* Open existing file for update (read+write) or create it when absent. */
pal_file *pal_file_open_update(const char *path);

/* Bytes written/read, or -1 on error. */
ptrdiff_t pal_file_write(pal_file *f, const void *buf, size_t n);
ptrdiff_t pal_file_read(pal_file *f, void *buf, size_t n);
/* Flush the stdio buffer. 0 on success. */
int pal_file_flush(pal_file *f);
/* Seek to an absolute byte offset. 0 on success, -1 on error. */
int pal_file_seek(pal_file *f, uint64_t pos);
/* Current byte offset, or -1 on error (returned as uint64_t). */
uint64_t pal_file_tell(pal_file *f);
/* Durability sync of the flushed bytes to storage (POSIX fsync, Windows
 * FlushFileBuffers). 0 on success, -1 on error. */
int pal_file_sync(pal_file *f);
/* Close the file, returning 0 on success or -1 if the close fails. */
int pal_file_close(pal_file *f);

int pal_file_exists(const char *path);
/* Rename, replacing the target if it exists. 0 on success. */
int pal_file_rename(const char *from, const char *to);
int pal_file_unlink(const char *path);

#ifdef DDUP_TESTING
/* Test seams for save failure and dynamic temporary-path coverage. */
void pal_file_test_fail_next_rename(void);
void pal_file_test_fail_next_flush(void);
void pal_file_test_fail_next_sync(void);
void pal_file_test_fail_next_close(void);
int pal_file_test_open_write_attempts(void);
void pal_file_test_reset(void);
#endif

#endif /* DDUP_PAL_FILE_H */
