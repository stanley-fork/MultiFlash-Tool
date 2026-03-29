#ifndef EDL_CRC32_H
#define EDL_CRC32_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t edl_crc32(const uint8_t *data, size_t len);
uint32_t edl_crc32_update(uint32_t crc, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* EDL_CRC32_H */
