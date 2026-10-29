/**
 * @file      ota.cpp
 * @brief     Firmware update slots and rollback — see ota.h.
 */
#include "ota.h"

#include "nvs.h"
#include "ota_state.h"

#ifdef ARDUINO
#include <esp32-hal-log.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#else
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
#endif

namespace {

constexpr const char *kNs = "ota";

hal::OtaBootState load_state() {
    hal::OtaBootState s;
    s.pending = hal::nvs_get_bool(kNs, "pending", false);
    s.boots = static_cast<uint8_t>(hal::nvs_get_u16(kNs, "boots", 0));
    return s;
}

#ifdef ARDUINO

void save_state(const hal::OtaBootState &s) {
    hal::nvs_set_bool(kNs, "pending", s.pending);
    hal::nvs_set_u16(kNs, "boots", s.boots);
}

esp_ota_handle_t s_handle = 0;
const esp_partition_t *s_part = nullptr;

void confirm_task(void *) {
    vTaskDelay(pdMS_TO_TICKS(hal::kOtaConfirmMs));
    hal::OtaBootState s = load_state();
    if (s.pending) {
        hal::ota_confirm(s);
        save_state(s);
        hal::nvs_set_str(kNs, "prev", "");
        log_i("ota: image confirmed after %u ms", (unsigned)hal::kOtaConfirmMs);
    }
    vTaskDelete(nullptr);
}

#else

std::string emu_root() {
    const char *r = getenv("PAGERLINK_FS_ROOT");
    return (r && *r) ? r : "/tmp/pagerlink-fs";
}

FILE *s_file = nullptr;
std::string s_part_path;

#endif

}  // namespace

void hw_ota_boot_check() {
#ifdef ARDUINO
    hal::OtaBootState s = load_state();
    const hal::OtaBootAction a = hal::ota_on_boot(s);
    save_state(s);
    if (a == hal::OtaBootAction::Rollback) {
        const std::string prev = hal::nvs_get_str(kNs, "prev");
        const esp_partition_t *p = prev.empty() ? nullptr
            : esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, prev.c_str());
        log_e("ota: %u unconfirmed boots, rolling back to %s", (unsigned)hal::kOtaMaxBoots,
              prev.c_str());
        if (p && esp_ota_set_boot_partition(p) == ESP_OK) {
            hal::nvs_set_str(kNs, "prev", "");
            esp_restart();
        }
        log_e("ota: rollback target missing; keeping this image");
        return;
    }
    if (a == hal::OtaBootAction::Count) {
        log_w("ota: pending image, boot %u of %u", (unsigned)s.boots, (unsigned)hal::kOtaMaxBoots);
        xTaskCreate(confirm_task, "ota_confirm", 3072, nullptr, 1, nullptr);
    }
#endif
}

HwOtaStatus hw_ota_status() {
    HwOtaStatus st;
    const hal::OtaBootState s = load_state();
    st.pending = s.pending;
    st.boots = s.boots;
#ifdef ARDUINO
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
    st.running = run ? run->label : "";
    st.next = next ? next->label : "";
    st.max_size = next ? next->size : 0;
#else
    st.running = "emu";
    st.next = "emu";
    st.max_size = 0x400000;  // the app slot size in partitions.csv
#endif
    return st;
}

bool hw_ota_slot_begin(uint32_t size, std::string &err) {
#ifdef ARDUINO
    hw_ota_slot_abort();
    s_part = esp_ota_get_next_update_partition(nullptr);
    if (!s_part) {
        err = "no OTA slot";
        return false;
    }
    if (size > s_part->size) {
        err = "image larger than the slot";
        return false;
    }
    const esp_err_t e = esp_ota_begin(s_part, size, &s_handle);
    if (e != ESP_OK) {
        err = std::string("ota begin: ") + esp_err_to_name(e);
        s_part = nullptr;
        return false;
    }
    return true;
#else
    (void)size;
    hw_ota_slot_abort();
    const std::string dir = emu_root() + "/ota";
    mkdir(emu_root().c_str(), 0755);
    mkdir(dir.c_str(), 0755);
    s_part_path = dir + "/installed.bin.part";
    s_file = fopen(s_part_path.c_str(), "wb");
    if (!s_file) {
        err = "cannot create " + s_part_path;
        return false;
    }
    return true;
#endif
}

bool hw_ota_slot_write(const uint8_t *data, size_t len, std::string &err) {
#ifdef ARDUINO
    if (!s_part) {
        err = "no slot open";
        return false;
    }
    const esp_err_t e = esp_ota_write(s_handle, data, len);
    if (e != ESP_OK) {
        err = std::string("ota write: ") + esp_err_to_name(e);
        return false;
    }
    return true;
#else
    if (!s_file || fwrite(data, 1, len, s_file) != len) {
        err = "write failed";
        return false;
    }
    return true;
#endif
}

bool hw_ota_slot_finish(std::string &err) {
#ifdef ARDUINO
    if (!s_part) {
        err = "no slot open";
        return false;
    }
    esp_err_t e = esp_ota_end(s_handle);  // validates the image
    s_handle = 0;
    if (e != ESP_OK) {
        err = std::string("image rejected: ") + esp_err_to_name(e);
        s_part = nullptr;
        return false;
    }
    const esp_partition_t *run = esp_ota_get_running_partition();
    e = esp_ota_set_boot_partition(s_part);
    s_part = nullptr;
    if (e != ESP_OK) {
        err = std::string("set boot slot: ") + esp_err_to_name(e);
        return false;
    }
    hal::nvs_set_str(kNs, "prev", run ? run->label : "");
    hal::OtaBootState s;
    hal::ota_mark_applied(s);
    save_state(s);
    return true;
#else
    if (!s_file) {
        err = "no slot open";
        return false;
    }
    const bool ok = fclose(s_file) == 0;
    s_file = nullptr;
    const std::string final_path = emu_root() + "/ota/installed.bin";
    if (!ok || rename(s_part_path.c_str(), final_path.c_str()) != 0) {
        err = "cannot write " + final_path;
        return false;
    }
    return true;
#endif
}

void hw_ota_slot_abort() {
#ifdef ARDUINO
    if (s_part) esp_ota_abort(s_handle);
    s_part = nullptr;
    s_handle = 0;
#else
    if (s_file) {
        fclose(s_file);
        s_file = nullptr;
        remove(s_part_path.c_str());
    }
#endif
}

void hw_ota_reboot() {
#ifdef ARDUINO
    esp_restart();
#endif
}
