#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "resource-map.h"

#define OFFSET 0x00ffffffffffffffull

/* Minimal struct to read allocation size from the args field.
 * This must match the beginning of mem_alloc_args_t layout:
 *   { int type; long long size; ... }
 * We define it here to avoid circular header dependencies. */
struct _alloc_size_view {
    int type;
    long long size;
};

resource_map* init_resource_map(uint64_t init_length) {
    resource_map* map = (resource_map*)malloc(sizeof(resource_map));
    if (map == NULL) {
        return NULL;
    }
    map->list = (resource_map_item*)malloc(sizeof(resource_map_item) * init_length);
    if (map->list == NULL) {
        free(map);
        return NULL;
    }
    map->length = init_length;
    map->free_ptr_idx = 1;
    map->tail_idx = 1;
    return map;
}

void free_resource_map(resource_map* map) {
    if (map == NULL) {
        return;
    }
    if (map->list != NULL) {
        free(map->list);
    }
    free(map);
}

void* resource_map_addr_from_index(uint64_t idx) {
    return (void*)(idx + OFFSET);
}

uint64_t resource_map_index_from_addr(void* addr) {
    return (uint64_t)addr - OFFSET;
}

resource_map_item* resource_map_get(resource_map* map, void* addr) {
    return &(map->list[(uint64_t)addr - OFFSET]);
}

/* Range search: when an exact index lookup fails, search all present
 * allocations to find one whose byte range [base, base+size) contains
 * the query address. This handles frameworks like PyTorch that call
 * cudaMalloc once for a large block and then use pointer arithmetic
 * to address sub-regions within it.
 *
 * Returns the translated real GPU address (real_base + offset),
 * or NULL if no containing allocation is found. */
static void* resource_map_find_in_range(resource_map* map, void* addr) {
    uint64_t query = (uint64_t)addr;

    for (uint64_t i = 1; i < map->tail_idx; i++) {
        if (!map->list[i].present) continue;
        if (map->list[i].args == NULL) continue;

        uint64_t base_addr = i + OFFSET;
        struct _alloc_size_view *alloc =
            (struct _alloc_size_view *)map->list[i].args;

        /* Skip entries with invalid or zero size */
        if (alloc->size <= 0) continue;

        uint64_t end_addr = base_addr + (uint64_t)alloc->size;

        if (query >= base_addr && query < end_addr) {
            /* Found it: compute the byte offset within the allocation
             * and apply it to the real GPU pointer */
            uint64_t offset = query - base_addr;
            return (void*)((uint64_t)map->list[i].mapped_addr + offset);
        }
    }
    return NULL;
}

void* resource_map_get_addr(resource_map* map, void *addr) {
    /* Fast path O(1): exact index lookup */
    uint64_t idx = (uint64_t)addr - OFFSET;
    if (idx < map->tail_idx && map->list[idx].present) {
        return map->list[idx].mapped_addr;
    }
    /* Slow path O(n): range search for sub-allocation offsets */
    return resource_map_find_in_range(map, addr);
}

void* resource_map_get_addr_default(resource_map* map, void *addr, void* default_addr) {
    /* Fast path O(1): exact index lookup */
    uint64_t idx = (uint64_t)addr - OFFSET;
    if (idx < map->tail_idx && map->list[idx].present) {
        return map->list[idx].mapped_addr;
    }
    /* Slow path O(n): range search for sub-allocation offsets */
    void *range_result = resource_map_find_in_range(map, addr);
    if (range_result != NULL) {
        return range_result;
    }
    return default_addr;
}

void *resource_map_get_or_null(resource_map *map, void *client_address) {
    return resource_map_get_addr_default(map, client_address, NULL);
}

int resource_map_update_addr_idx(resource_map* map, uint64_t idx, void* new_addr) {
    if (!resource_map_contains(map, resource_map_addr_from_index(idx))) {
        return -1;
    }
    map->list[idx].mapped_addr = new_addr;
    return 0;
}

uint8_t resource_map_contains(resource_map* map, void* addr) {
    return (uint64_t)addr - OFFSET < map->tail_idx && map->list[(uint64_t)addr - OFFSET].present;
}

int resource_map_add(resource_map* map, void* mapped_addr, void *args, void **client_addr) {
    if (map->free_ptr_idx >= map->length && map->tail_idx >= map->length) {
        resource_map_item *new_list = (resource_map_item*)realloc(map->list, sizeof(resource_map_item) * map->length * 2);
        if (new_list == NULL) {
            return -1;
        }
        map->list = new_list;
        map->length *= 2;
    }
    if (map->tail_idx == map->free_ptr_idx) {
        map->list[map->tail_idx].mapped_addr = mapped_addr;
        map->list[map->tail_idx].args = args;
        map->list[map->tail_idx].present = 1;
        *client_addr = (void*)(map->tail_idx + OFFSET);
        map->tail_idx++;
        map->free_ptr_idx++;
    }
    else {
        uint64_t alloc_idx = map->free_ptr_idx;
        map->free_ptr_idx = (uint64_t)map->list[map->free_ptr_idx].mapped_addr;
        map->list[alloc_idx].mapped_addr = (void*)mapped_addr;
        map->list[alloc_idx].args = args;
        map->list[alloc_idx].present = 1;
        *client_addr = (void*)(alloc_idx + OFFSET);
    }
    return 0;
}

void resource_map_unset(resource_map* map, void* client_addr) {
    uint64_t idx = (uint64_t)client_addr - OFFSET;

    map->list[idx].mapped_addr = (void*)map->free_ptr_idx;
    map->list[idx].present = 0;
    free(map->list[idx].args);
    map->free_ptr_idx = idx;
}

resource_map_iter* resource_map_init_iter(resource_map* map) {

    if (map == NULL) {
        return NULL;
    }

    resource_map_iter* iter = (resource_map_iter*)malloc(sizeof(resource_map_iter));
    if (iter == NULL) {
        return NULL;
    }
    iter->map = map;
    iter->current_idx = 0;

    return iter;
}

void resource_map_free_iter(resource_map_iter* iter) {
    if (iter == NULL) {
        return;
    }
    free(iter);
}

uint64_t resource_map_iter_next(resource_map_iter* iter) {
    if (iter == NULL) {
        return 0;
    }
    if (iter->current_idx >= iter->map->tail_idx) {
        return 0;
    }
    while (iter->current_idx < iter->map->tail_idx && iter->map->list[iter->current_idx].present == 0) {
        iter->current_idx++;
    }

    return iter->current_idx >= iter->map->tail_idx ? 0 : iter->current_idx++;
}
