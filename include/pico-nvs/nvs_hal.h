#if !defined(NVS_HAL_H_)
#define NVS_HAL_H_

#include <stddef.h>
#include <stdint.h>

typedef uint32_t nvs_hal_timeout_t;

#define NVS_TIMEOUT_FOREVER UINT32_MAX

struct nvs_hal_mutex {
    int (*lock)(nvs_hal_timeout_t timeout);
    void (*unlock)();
};

struct nvs_hal_flash {
    int (*read)(uint32_t addr, uint8_t *buffer, size_t size);
    int (*write)(uint32_t addr, uint8_t *buffer, size_t size);
    int (*erase)(uint32_t offset, size_t size);
};

#endif