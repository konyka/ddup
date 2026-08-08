/* ddup-reshard - move one hash slot between two ddup cluster nodes.
 *
 * Usage:
 *   ddup-reshard --from host:port --to host:port --slot N
 *                [--count K] [--timeout ms]
 *
 * redis-cli --cluster reshard style: marks the slot MIGRATING/IMPORTING,
 * moves keys in batches of --count via MIGRATE ... REPLACE KEYS, then
 * finalizes ownership with SETSLOT NODE on both ends.
 */
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "pal/pal_socket.h"
#include "reshard_args.h"
#include "reshard_client.h"

static void usage(FILE *out)
{
    fprintf(out,
            "usage: ddup-reshard --from host:port --to host:port --slot N\n"
            "                    [--count K] [--timeout ms]\n");
}

int main(int argc, char **argv)
{
    char from_host[256] = "";
    char to_host[256] = "";
    uint16_t from_port = 0, to_port = 0;
    int slot = -1, count = 100, timeout_ms = 5000;
    long long migrated = 0;
    int i, rc;

    for (i = 1; i < argc; i++) {
        const char *val = i + 1 < argc ? argv[i + 1] : NULL;
        if (strcmp(argv[i], "--from") == 0 && val != NULL) {
            if (reshard_parse_addr(val, from_host, sizeof(from_host),
                                   &from_port) != 0) {
                fprintf(stderr, "ddup-reshard: bad --from '%s'\n", val);
                return 2;
            }
            i++;
        } else if (strcmp(argv[i], "--to") == 0 && val != NULL) {
            if (reshard_parse_addr(val, to_host, sizeof(to_host), &to_port) !=
                0) {
                fprintf(stderr, "ddup-reshard: bad --to '%s'\n", val);
                return 2;
            }
            i++;
        } else if (strcmp(argv[i], "--slot") == 0 && val != NULL) {
            if (reshard_parse_int(val, 0, 16383, &slot) != 0) {
                fprintf(stderr, "ddup-reshard: bad --slot '%s'\n", val);
                return 2;
            }
            i++;
        } else if (strcmp(argv[i], "--count") == 0 && val != NULL) {
            if (reshard_parse_int(val, 1, INT_MAX, &count) != 0) {
                fprintf(stderr, "ddup-reshard: bad --count '%s'\n", val);
                return 2;
            }
            i++;
        } else if (strcmp(argv[i], "--timeout") == 0 && val != NULL) {
            if (reshard_parse_int(val, 0, INT_MAX, &timeout_ms) != 0) {
                fprintf(stderr, "ddup-reshard: bad --timeout '%s'\n", val);
                return 2;
            }
            i++;
        } else if (strcmp(argv[i], "--help") == 0) {
            usage(stdout);
            return 0;
        } else {
            fprintf(stderr, "ddup-reshard: unknown argument '%s'\n",
                    argv[i]);
            usage(stderr);
            return 2;
        }
    }
    if (from_port == 0 || to_port == 0 || slot < 0 || count < 1 ||
        timeout_ms < 0) {
        usage(stderr);
        return 2;
    }

    if (pal_socket_init() != 0) {
        fprintf(stderr, "ddup-reshard: socket init failed\n");
        return 1;
    }
    rc = reshard_slot(from_host, from_port, to_host, to_port, slot, count,
                      timeout_ms, &migrated, stderr);
    pal_socket_cleanup();
    if (rc != 0) {
        fprintf(stderr, "ddup-reshard: failed (slot may be left in "
                        "MIGRATING/IMPORTING state)\n");
        return 1;
    }
    printf("ddup-reshard: slot %d moved %s:%u -> %s:%u (%lld keys)\n", slot,
           from_host, (unsigned)from_port, to_host, (unsigned)to_port,
           migrated);
    return 0;
}
