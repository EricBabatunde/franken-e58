#pragma once
#include <cstddef>

#include "esp_err.h"

// LittleFS on the "storage" partition (board_config), mounted READ-ONLY at board::kFsBasePath.
// Never formats, never grows, never writes: the no-flash-writes-while-armed rule holds from
// the start. The partition also carries the "readonly" flag, so esp_partition refuses writes.
// Reads go through the cache-mapped partition (CONFIG_LITTLEFS_MMAP_PARTITION), not through
// esp_flash_read(), which would disable the cache on both cores.
namespace hal_fs {

struct Info {
    size_t total_bytes = 0;
    size_t used_bytes = 0;
};

esp_err_t mount();              // call once, from Core 0, while disarmed
esp_err_t info(Info* out);

}  // namespace hal_fs
