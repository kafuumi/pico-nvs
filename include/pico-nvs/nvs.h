#if !defined(NVS_H_)
#define NVS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "nvs_hal.h"

#if !defined(NVS_FLASH_PAGE_SIZE)
#define NVS_FLASH_PAGE_SIZE 256
#endif

#if !defined(NVS_FLASH_SECTOR_SIZE)
#define NVS_FLASH_SECTOR_SIZE (4 << 10)
#endif

// sector size 4k
#define NVS_SECTOR_SIZE NVS_FLASH_SECTOR_SIZE
#define NVS_EMPTY_VALUE 0xFF

#define PICO_NVS_VERSION 0x00

struct nvs_fs_config_t {
    // first sector address
    off_t offset;
    int page_num;

    const struct nvs_hal_flash *flash_dev;
};

typedef struct nvs_fs *nvs_fs_t;

#endif