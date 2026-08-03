/* pal_file.c - thin cross-platform file IO wrapper; see pal_file.h. */
#include "pal/pal_file.h"

#include <stdio.h>
#include <stdlib.h>

#include "pal/pal_platform.h"

struct pal_file {
    FILE *fp;
};

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
    return fflush(f->fp) == 0 ? 0 : -1;
}

void pal_file_close(pal_file *f)
{
    if (f == NULL)
        return;
    fclose(f->fp);
    free(f);
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
#if DDUP_OS_WINDOWS
    /* rename() on Windows fails when the target exists. */
    remove(to);
#endif
    return rename(from, to) == 0 ? 0 : -1;
}

int pal_file_unlink(const char *path)
{
    return remove(path) == 0 ? 0 : -1;
}
