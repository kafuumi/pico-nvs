#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "pico-nvs/nvs.h"

#define EOK 0

#define NVS_PAGE_HEADER_SIZE 32
#define NVS_PAGE_ENTRY_SIZE NVS_PAGE_HEADER_SIZE
#define NVS_PAGE_ENTRY_OFFSET (NVS_PAGE_HEADER_SIZE + NVS_PAGE_BITMAT_SIZE)
#define NVS_PAGE_BITMAT_SIZE NVS_PAGE_HEADER_SIZE

#define NVS_PAGE_HEADER_MAGIC (('n' << 24) | ('v' << 16) | ('s' << 8) | ('-'))

#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof(a[0]))

enum {
    NVS_PAGE_STAT_EMPTY = 0xFF,
    NVS_PAGE_STAT_ACTIVE = 0xFE,
    NVS_PAGE_STAT_FULL = 0xFC,
};

enum {
    NVS_ENTRY_STAT_EMPTY = 0x03,    // 0b11
    NVS_ENTRY_STAT_WRITE = 0x02,    // 0b10
    NVS_ENTRY_STAT_CONTINUE = 0x01, // 0b01
    NVS_ENTRY_STAT_DELETE = 0x00,   // 0b00
};

/*
 * Entry stat bitmap
 * 11: unused
 * 10: used
 * 01: continue
 * 00: deleted
 */
typedef uint8_t nvs_entry_bitmap[NVS_PAGE_HEADER_SIZE];

/*
 * Page header 32 Bytes
 * | magic(4B) | stat(4B) | seq(2B) | ver(2B) | unused(16B) | crc32(4B) |
 */
struct nvs_page_header {
    uint32_t magic;
    uint32_t stat;
    uint16_t seq;
    uint16_t ver;
    uint32_t crc32_sum;
};

typedef uint16_t entry_key_t;

/*
 * Entry 32 Bytes
 * | key(2B) | size(2B) | crc32(4B) | data(24B) |
 */

struct nvs_entry {
    entry_key_t key;
    uint16_t size;
    uint32_t crc32_sum;
    uint32_t data_addr;
};

struct nvs_fs_page {
    uint32_t address;
    struct nvs_page_header header;
    nvs_entry_bitmap bitmap;
};

struct nvs_fs {
    // first sector address
    off_t offset;
    int page_num;
    // single sector size (default 4K)
    size_t sector_size;
    int cur_page;
    int next_slot;

    volatile bool ready;
    struct nvs_fs_page *page_stat;
    const struct nvs_hal_mutex *nvs_lock;
    const struct nvs_hal_flash *flash_dev;
};

static inline uint32_t read_uint32(uint8_t *buf) {
    return (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | (buf[3]);
}

static inline uint16_t read_uint16(uint8_t *buf) {
    return (buf[0] << 8) | (buf[1]);
}

static inline void write_uint32(uint8_t *buf, int idx, uint32_t value) {
    buf[idx] = (uint8_t)(value >> 24);
    buf[idx + 1] = (uint8_t)(value >> 16);
    buf[idx + 2] = (uint8_t)(value >> 8);
    buf[idx + 3] = (uint8_t)(value & 0xFF);
}

static inline void write_uint16(uint8_t *buf, int idx, uint16_t value) {
    buf[idx] = (uint8_t)(value >> 8);
    buf[idx + 1] = (uint8_t)(value & 0xFF);
}

static void nvs_set_empty_page(struct nvs_fs_page *page, int seq) {
    page->header.magic = NVS_PAGE_HEADER_MAGIC;
    page->header.stat = NVS_PAGE_STAT_EMPTY;
    page->header.seq = seq;
    page->header.ver = PICO_NVS_VERSION;
    // todo: crc32
    memset(page->bitmap, NVS_EMPTY_VALUE, sizeof(page->bitmap));
}

static void nvs_fill_header(struct nvs_fs_page *page, uint8_t *buf) {
    int idx = 0;
    write_uint32(buf, idx, page->header.magic);
    idx += sizeof(page->header.magic);
    write_uint32(buf, idx, page->header.stat);
    idx += sizeof(page->header.stat);
    write_uint16(buf, idx, page->header.seq);
    idx += sizeof(page->header.seq);
    write_uint16(buf, idx, page->header.ver);
    idx += sizeof(page->header.ver);
    for (; idx < (int)(NVS_PAGE_HEADER_SIZE - sizeof(page->header.crc32_sum)); ++idx) {
        buf[idx] = NVS_EMPTY_VALUE;
    }
    write_uint32(buf, idx, page->header.crc32_sum);

    memcpy(buf + NVS_PAGE_HEADER_SIZE, page->bitmap, NVS_PAGE_BITMAT_SIZE);
}

static int nvs_earse_page(nvs_fs_t fs, struct nvs_fs_page *page) {
    uint8_t *buf = malloc(sizeof(uint8_t) * NVS_FLASH_PAGE_SIZE);
    if (!buf) {
        return ENOMEM;
    }

    nvs_fill_header(page, buf);
    memset(buf + NVS_PAGE_ENTRY_OFFSET, NVS_EMPTY_VALUE, NVS_FLASH_PAGE_SIZE - NVS_PAGE_ENTRY_OFFSET);
    int rc;
    rc = fs->flash_dev->erase(page->address, fs->sector_size);
    if (rc) {
        goto cleanup;
    }
    rc = fs->flash_dev->write(page->address, buf, NVS_FLASH_PAGE_SIZE);
cleanup:
    free(buf);
    return rc;
}

static int nvs_update_page_metadata(nvs_fs_t fs, struct nvs_fs_page *page) {
    uint8_t *buf = malloc(sizeof(uint8_t) * NVS_FLASH_PAGE_SIZE);
    if (!buf) {
        return ENOMEM;
    }
    int rc = fs->flash_dev->read(page->address, buf, NVS_FLASH_PAGE_SIZE);
    if (rc) {
        goto cleanup;
    }
    nvs_fill_header(page, buf);
    rc = fs->flash_dev->write(page->address, buf, NVS_FLASH_PAGE_SIZE);
cleanup:
    free(buf);
    return rc;
}

static void nvs_find_next_slot(nvs_fs_t fs) {
    int slot = 0;
    struct nvs_fs_page *const page = &fs->page_stat[fs->cur_page];
    for (int i = 0; i < NVS_PAGE_BITMAT_SIZE; ++i) {
        uint8_t bitmap = page->bitmap[i];
        for (int idx = 3; idx >= 0; --idx) {
            uint8_t value = (bitmap >> (idx * 2)) & 0x03;
            if (value == NVS_ENTRY_STAT_EMPTY) {
                goto loop_end;
            }
            slot++;
        }
    }

loop_end:
    if (slot >= (int)((fs->sector_size - NVS_PAGE_HEADER_SIZE - NVS_PAGE_BITMAT_SIZE) / NVS_PAGE_ENTRY_SIZE)) {
        // jump to next page
        page->header.stat = NVS_PAGE_STAT_FULL;
        int rc = nvs_update_page_metadata(fs, page);
        if (rc) {
            // todo warn
        }
        fs->cur_page = (fs->cur_page + 1) % fs->page_num;
        slot = 0;
    }
    fs->next_slot = slot;
}

static int nvs_init_page(nvs_fs_t fs) {
    uint32_t start;
    const struct nvs_hal_flash *flash = fs->flash_dev;
    int rc;

    struct nvs_fs_page *cur_page;
    uint8_t header_buf[NVS_PAGE_HEADER_SIZE];
    int latest_seq = -1;
    for (int i = 0; i < fs->page_num; ++i) {
        /* read header */
        start = fs->offset + i * fs->sector_size;
        cur_page = &fs->page_stat[i];
        cur_page->address = start;
        rc = flash->read(start, header_buf, NVS_PAGE_HEADER_SIZE);
        if (rc) {
            return rc;
        }

        uint8_t *read_ptr = header_buf;
        uint32_t magic = read_uint32(read_ptr);
        read_ptr += sizeof(uint32_t);
        uint32_t stat = read_uint32(read_ptr);
        read_ptr += sizeof(uint32_t);
        uint16_t seq = read_uint16(read_ptr);
        read_ptr += sizeof(uint16_t);
        uint16_t ver = read_uint16(read_ptr);
        read_ptr += sizeof(uint16_t);
        uint32_t crc32_sum = read_uint32(read_ptr);
        if (magic != NVS_PAGE_HEADER_MAGIC) {
            cur_page->header.stat = NVS_PAGE_STAT_EMPTY;
            continue;
        }
        // todo: crc32 check
        cur_page->header.magic = magic;
        cur_page->header.stat = stat;
        cur_page->header.seq = seq;
        cur_page->header.ver = ver;
        cur_page->header.crc32_sum = crc32_sum;

        /* read bitmap */
        rc = flash->read(start + NVS_PAGE_HEADER_SIZE, cur_page->bitmap, NVS_PAGE_BITMAT_SIZE);
        if (rc) {
            return rc;
        }
        if (stat == NVS_PAGE_STAT_ACTIVE) {
            fs->cur_page = i;
        }
        latest_seq = MAX(latest_seq, seq);
    }
    if (latest_seq < 0) {
        // no valid page, setup
        fs->cur_page = 0;
        nvs_set_empty_page(&fs->page_stat[0], 0);
        fs->page_stat[0].header.stat = NVS_PAGE_STAT_ACTIVE;
        rc = nvs_earse_page(fs, &fs->page_stat[0]);
    }
    if (rc) {
        return rc;
    }
    // set page seq
    for (int i = 0, seq = latest_seq; i < fs->page_num; ++i) {
        cur_page = &fs->page_stat[i];
        if (cur_page->header.stat != NVS_PAGE_STAT_EMPTY) {
            continue;
        }
        seq++;
        nvs_set_empty_page(cur_page, seq);
    }
    nvs_find_next_slot(fs);
    return EOK;
}

static int nvs_read_entry_metadata(nvs_fs_t fs, uint32_t addr, struct nvs_entry *entry) {
    if (!entry) {
        return EINVAL;
    }
    uint8_t buf[8];
    int rc = fs->flash_dev->read(addr, buf, sizeof(buf));
    if (rc) {
        return rc;
    }
    entry->key = read_uint16(buf);
    entry->size = read_uint16(buf + sizeof(entry_key_t));
    entry->crc32_sum = read_uint32(buf + sizeof(entry_key_t) + sizeof(uint16_t));
    entry->data_addr = addr + 8;
    return EOK;
}

static int nvs_search_key_in_page(nvs_fs_t fs, entry_key_t key, const struct nvs_fs_page *page,
                                  struct nvs_entry *out_entry) {
    int slot_id = 0;
    off_t offset;
    struct nvs_entry entry;
    int rc;
    for (int i = NVS_PAGE_BITMAT_SIZE - 1; i >= 0; i--) {
        uint8_t bitmap = page->bitmap[i];
        for (int idx = 3; idx >= 0; --idx) {
            uint8_t value = (bitmap >> (idx * 2)) & 0x03;
            if (value != NVS_ENTRY_STAT_WRITE) {
                continue;
            }
            offset = page->address + NVS_PAGE_ENTRY_OFFSET + (slot_id * NVS_PAGE_ENTRY_SIZE);
            rc = nvs_read_entry_metadata(fs, offset, &entry);
            if (rc) {
                return rc;
            }
            if (entry.key == key) {
                *out_entry = entry;
                return EOK;
            }
        }
    }
    return ENODATA;
}

int nvs_read_key(nvs_fs_t fs, entry_key_t key, uint8_t *out_value, size_t size, size_t *out_size) {
    if (!fs->ready) {
        return false;
    }
    int rc = EOK;
    fs->nvs_lock->lock(NVS_TIMEOUT_FOREVER);
    const struct nvs_fs_page *page;
    struct nvs_entry entry = {.data_addr = 0};
    for (int i = 0; i < fs->page_num; ++i) {
        page = &fs->page_stat[i];
        if (page->header.stat == NVS_PAGE_STAT_EMPTY) {
            continue;
        }
        rc = nvs_search_key_in_page(fs, key, page, &entry);
        if (rc == ENODATA) {
            // not found
            continue;
        } else if (rc) {
            goto finish;
        }
        // found
        break;
    }
    if (entry.data_addr > 0) {
        if (out_size) {
            *out_size = entry.size;
        }
        if (out_value) {
            size = MIN(size, entry.size);
            rc = fs->flash_dev->read(entry.data_addr, out_value, size);
        } else {
            rc = EOK;
        }
    } else {
        rc = ENODATA;
    }
finish:
    fs->nvs_lock->unlock();
    return rc;
}

int nvs_mount(struct nvs_fs_config_t *cfg, nvs_fs_t *out_fs) {
    nvs_fs_t fs = NULL;
    int rc = EOK;
    if (cfg->page_num < 2 || cfg->flash_dev == NULL) {
        return EINVAL;
    }
    if (cfg->offset % NVS_SECTOR_SIZE != 0) {
        return EINVAL;
    }
    fs = malloc(sizeof(struct nvs_fs));
    if (!fs) {
        return ENOMEM;
    }
    fs->offset = cfg->offset;
    fs->page_num = cfg->page_num;
    fs->flash_dev = cfg->flash_dev;
    fs->sector_size = NVS_SECTOR_SIZE;
    fs->page_stat = calloc(fs->page_num, sizeof(struct nvs_fs_page));
    if (!fs->page_stat) {
        goto cleanup;
    }
    rc = nvs_init_page(fs);
    if (rc) {
        goto cleanup;
    }
    fs->ready = true;
    *out_fs = fs;
    return EOK;
cleanup:
    if (fs && fs->page_stat) {
        free(fs->page_stat);
    }
    if (fs) {
        free(fs);
    }
    return rc;
}

int main() {
    return EOK;
}