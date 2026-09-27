#include "../include/e820.h"

/* 引导扇区在 0x5000 处写下的内存布局 */
static const e820_map_t* const boot_map = (const e820_map_t*)E820_ADDR;

const e820_map_t* e820_get_map(void) {
    /* 引导程序写下的条目数如果明显不合理，就当探测失败处理 */
    if (boot_map->count > E820_MAX_ENTRIES) {
        return NULL;
    }
    return boot_map;
}

uint64_t e820_total_usable(void) {
    const e820_map_t* map = e820_get_map();
    uint64_t total = 0;

    if (!map) {
        return 0;
    }

    for (uint32_t i = 0; i < map->count; i++) {
        if (map->entries[i].type == E820_TYPE_USABLE) {
            total += map->entries[i].length;
        }
    }
    return total;
}

uint64_t e820_highest_usable(void) {
    const e820_map_t* map = e820_get_map();
    uint64_t highest = 0;

    if (!map) {
        return 0;
    }

    for (uint32_t i = 0; i < map->count; i++) {
        if (map->entries[i].type != E820_TYPE_USABLE) {
            continue;
        }
        uint64_t end = map->entries[i].base + map->entries[i].length;
        if (end > highest) {
            highest = end;
        }
    }
    return highest;
}

const char* e820_type_name(uint32_t type) {
    switch (type) {
        case E820_TYPE_USABLE:       return "usable";
        case E820_TYPE_RESERVED:     return "reserved";
        case E820_TYPE_ACPI_RECLAIM: return "ACPI reclaim";
        case E820_TYPE_ACPI_NVS:     return "ACPI NVS";
        case E820_TYPE_BAD:          return "bad";
        default:                     return "unknown";
    }
}
