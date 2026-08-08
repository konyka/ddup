/* reshard_args.h - strict ddup-reshard command-line value parsing. */
#ifndef DDUP_RESHARD_ARGS_H
#define DDUP_RESHARD_ARGS_H

#include <stddef.h>
#include <stdint.h>

int reshard_parse_addr(const char *s, char *host, size_t hostcap,
                       uint16_t *port);
int reshard_parse_int(const char *s, int min_value, int max_value, int *out);

#endif /* DDUP_RESHARD_ARGS_H */
