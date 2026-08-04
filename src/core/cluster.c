/* cluster.c - single-node cluster mode: node identity; see cluster.h. */
#include "core/cluster.h"

#include <stdio.h>
#include <string.h>

#include "pal/pal_file.h"
#include "pal/pal_time.h"

static void gen_node_id(char out[41])
{
    static const char hex[] = "0123456789abcdef";
    uint64_t x = pal_now_us() ^ 0x9E3779B97F4A7C15ULL;
    int i;
    for (i = 0; i < 40; i++) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        out[i] = hex[x & 0xF];
    }
    out[40] = '\0';
}

int cluster_node_id_load_or_create(const char *path, char out_id[41])
{
    if (pal_file_exists(path)) {
        pal_file *f = pal_file_open_read(path);
        char buf[41];
        ptrdiff_t n;
        int i;
        if (f == NULL)
            return -1;
        memset(buf, 0, sizeof(buf));
        n = pal_file_read(f, buf, 40);
        pal_file_close(f);
        if (n != 40)
            return -1;
        for (i = 0; i < 40; i++) {
            char c = buf[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
                return -1;
        }
        memcpy(out_id, buf, 40);
        out_id[40] = '\0';
        return 0;
    }

    gen_node_id(out_id);
    {
        pal_file *f = pal_file_open_write(path);
        char line[128];
        int len;
        if (f == NULL)
            return -1;
        len = snprintf(line, sizeof(line),
                       "%s :0@0 myself,master - 0 0 1 connected 0-16383\n",
                       out_id);
        if (pal_file_write(f, line, (size_t)len) != len) {
            pal_file_close(f);
            return -1;
        }
        pal_file_close(f);
    }
    return 0;
}
