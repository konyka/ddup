/* crc64.h - CRC-64 used to checksum DUMP/RESTORE payloads.
 *
 * Reflected ECMA polynomial 0xC96C5795D7870F42 with all-ones init/xorout
 * (the CRC-64/XZ parameter set); check value for "123456789" is
 * 0x995DC9BBDF1939FA.
 */
#ifndef DDUP_CRC64_H
#define DDUP_CRC64_H

#include <stddef.h>
#include <stdint.h>

/* Chainable: pass 0 for the first chunk, the previous return value for
 * continuation chunks. */
uint64_t crc64(uint64_t crc, const void *data, size_t len);

#endif /* DDUP_CRC64_H */
