/* sim shim — heap_caps_* 映射到普通 malloc 族（主机无 PSRAM 区分）。 */
#ifndef SIM_ESP_HEAP_CAPS_H
#define SIM_ESP_HEAP_CAPS_H

#include <stdlib.h>

#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_INTERNAL 0
#define MALLOC_CAP_8BIT 0
#define MALLOC_CAP_DMA 0

static inline void* heap_caps_malloc(size_t size, unsigned caps) {
    (void)caps;
    return malloc(size);
}

static inline void* heap_caps_aligned_alloc(size_t alignment, size_t size, unsigned caps) {
    (void)caps;
    void* p = NULL;
    if (posix_memalign(&p, alignment, size) != 0) return NULL;
    return p;
}

static inline void heap_caps_free(void* p) { free(p); }

#endif /* SIM_ESP_HEAP_CAPS_H */
