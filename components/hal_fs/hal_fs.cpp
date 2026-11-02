#include "hal_fs.h"

#include "board_config.h"
#include "esp_littlefs.h"

namespace hal_fs {

esp_err_t mount() {
    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path = board::kFsBasePath;
    conf.partition_label = board::kFsPartitionLabel;
    conf.format_if_mount_failed = 0;  // never format: a failed mount is reported, not repaired
    conf.read_only = 1;
    conf.dont_mount = 0;
    conf.grow_on_mount = 0;
    return esp_vfs_littlefs_register(&conf);
}

esp_err_t info(Info* out) {
    if (out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    return esp_littlefs_info(board::kFsPartitionLabel, &out->total_bytes, &out->used_bytes);
}

}  // namespace hal_fs
