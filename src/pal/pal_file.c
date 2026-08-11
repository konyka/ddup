/* pal_file.c - thin cross-platform file IO wrapper; see pal_file.h. */
#include "pal/pal_file.h"

#include <stdio.h>
#include <stdlib.h>

#include "pal/pal_platform.h"

#if DDUP_OS_WINDOWS
#include <windows.h>
#endif

struct pal_file {
    FILE *fp;
};

#ifdef DDUP_TESTING
static int pal_file_fail_next_rename;
static int pal_file_fail_next_flush;
static int pal_file_fail_next_close;
static int pal_file_open_write_count;

void pal_file_test_fail_next_rename(void)
{
    pal_file_fail_next_rename = 1;
}

void pal_file_test_fail_next_flush(void)
{
    pal_file_fail_next_flush = 1;
}

void pal_file_test_fail_next_close(void)
{
    pal_file_fail_next_close = 1;
}

int pal_file_test_open_write_attempts(void)
{
    return pal_file_open_write_count;
}

void pal_file_test_reset(void)
{
    pal_file_fail_next_rename = 0;
    pal_file_fail_next_flush = 0;
    pal_file_fail_next_close = 0;
    pal_file_open_write_count = 0;
}
#endif

static pal_file *wrap(FILE *fp)
{
    pal_file *f;
    if (fp == NULL)
        return NULL;
    f = (pal_file *)malloc(sizeof(*f));
    if (f == NULL) {
        fclose(fp);
        return NULL;
    }
    f->fp = fp;
    return f;
}

pal_file *pal_file_open_append(const char *path)
{
    return wrap(fopen(path, "ab"));
}

pal_file *pal_file_open_read(const char *path)
{
    return wrap(fopen(path, "rb"));
}

pal_file *pal_file_open_write(const char *path)
{
#ifdef DDUP_TESTING
    pal_file_open_write_count++;
#endif
    return wrap(fopen(path, "wb"));
}

ptrdiff_t pal_file_write(pal_file *f, const void *buf, size_t n)
{
    size_t w = fwrite(buf, 1, n, f->fp);
    if (w < n && ferror(f->fp))
        return -1;
    return (ptrdiff_t)w;
}

ptrdiff_t pal_file_read(pal_file *f, void *buf, size_t n)
{
    size_t r = fread(buf, 1, n, f->fp);
    if (r < n && ferror(f->fp))
        return -1;
    return (ptrdiff_t)r;
}

int pal_file_flush(pal_file *f)
{
#ifdef DDUP_TESTING
    if (pal_file_fail_next_flush) {
        pal_file_fail_next_flush = 0;
        return -1;
    }
#endif
    return fflush(f->fp) == 0 ? 0 : -1;
}

int pal_file_close(pal_file *f)
{
    int rc;
    if (f == NULL)
        return 0;
    rc = fclose(f->fp);
#ifdef DDUP_TESTING
    if (pal_file_fail_next_close) {
        pal_file_fail_next_close = 0;
        rc = EOF;
    }
#endif
    free(f);
    return rc == 0 ? 0 : -1;
}

int pal_file_exists(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (fp == NULL)
        return 0;
    fclose(fp);
    return 1;
}

int pal_file_rename(const char *from, const char *to)
{
#ifdef DDUP_TESTING
    if (pal_file_fail_next_rename) {
        pal_file_fail_next_rename = 0;
        return -1;
    }
#endif
#if DDUP_OS_WINDOWS
    /* MoveFileEx replaces the target without first deleting the last snapshot. */
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING) != 0 ? 0 : -1;
#else
    return rename(from, to) == 0 ? 0 : -1;
#endif
}

int pal_file_unlink(const char *path)
{
    return remove(path) == 0 ? 0 : -1;
}
