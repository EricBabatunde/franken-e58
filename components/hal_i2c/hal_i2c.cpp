#include "hal_i2c.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

namespace hal_i2c {
namespace {

const char* TAG = "hal_i2c";
constexpr int kMaxDevicesPerBus = 2;

struct BusState {
    bool inited = false;
    int sda = -1;
    int scl = -1;
    uint32_t hz = 0;
    i2c_master_bus_handle_t handle = nullptr;
    uint8_t n_dev = 0;
    uint8_t addr[kMaxDevicesPerBus] = {};
    i2c_master_dev_handle_t dev[kMaxDevicesPerBus] = {};
};

BusState g_bus[2];

BusState& state(Bus b) { return g_bus[static_cast<int>(b)]; }

esp_err_t create_bus(BusState& b, int port) {
    i2c_master_bus_config_t cfg = {};
    cfg.i2c_port = static_cast<i2c_port_num_t>(port);
    cfg.sda_io_num = static_cast<gpio_num_t>(b.sda);
    cfg.scl_io_num = static_cast<gpio_num_t>(b.scl);
    cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    cfg.glitch_ignore_cnt = 7;
    cfg.flags.enable_internal_pullup = false;  // external 4.7 kOhm pull-ups only
    return i2c_new_master_bus(&cfg, &b.handle);
}

esp_err_t create_device(BusState& b, uint8_t slot) {
    i2c_device_config_t dc = {};
    dc.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dc.device_address = b.addr[slot];
    dc.scl_speed_hz = b.hz;
    return i2c_master_bus_add_device(b.handle, &dc, &b.dev[slot]);
}

// Releases a slave that is holding SDA low mid-byte, then issues a STOP.
void bitbang_clear(int sda, int scl) {
    gpio_config_t c = {};
    c.intr_type = GPIO_INTR_DISABLE;
    c.pull_up_en = GPIO_PULLUP_DISABLE;
    c.pull_down_en = GPIO_PULLDOWN_DISABLE;

    c.pin_bit_mask = 1ULL << scl;
    c.mode = GPIO_MODE_OUTPUT_OD;
    gpio_config(&c);
    c.pin_bit_mask = 1ULL << sda;
    c.mode = GPIO_MODE_INPUT_OUTPUT_OD;
    gpio_config(&c);

    const gpio_num_t sda_pin = static_cast<gpio_num_t>(sda);
    const gpio_num_t scl_pin = static_cast<gpio_num_t>(scl);
    gpio_set_level(scl_pin, 1);
    gpio_set_level(sda_pin, 1);
    esp_rom_delay_us(10);

    for (int i = 0; i < 9 && gpio_get_level(sda_pin) == 0; ++i) {
        gpio_set_level(scl_pin, 0);
        esp_rom_delay_us(5);
        gpio_set_level(scl_pin, 1);
        esp_rom_delay_us(5);
    }

    // STOP condition: SDA low to high while SCL is high.
    gpio_set_level(sda_pin, 0);
    esp_rom_delay_us(5);
    gpio_set_level(scl_pin, 1);
    esp_rom_delay_us(5);
    gpio_set_level(sda_pin, 1);
    esp_rom_delay_us(5);
}

}  // namespace

esp_err_t init(Bus bus, int sda_gpio, int scl_gpio, uint32_t scl_hz) {
    BusState& b = state(bus);
    if (b.inited) {
        return ESP_ERR_INVALID_STATE;
    }
    b.sda = sda_gpio;
    b.scl = scl_gpio;
    b.hz = scl_hz;
    const esp_err_t err = create_bus(b, static_cast<int>(bus));
    b.inited = (err == ESP_OK);
    return err;
}

esp_err_t add_device(Bus bus, uint8_t addr7, Device* out) {
    BusState& b = state(bus);
    if (!b.inited || b.n_dev >= kMaxDevicesPerBus || out == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t slot = b.n_dev;
    b.addr[slot] = addr7;
    const esp_err_t err = create_device(b, slot);
    if (err != ESP_OK) {
        return err;
    }
    b.n_dev++;
    out->bus = static_cast<uint8_t>(bus);
    out->slot = slot;
    return ESP_OK;
}

esp_err_t probe(Bus bus, uint8_t addr7, int timeout_ms) {
    BusState& b = state(bus);
    if (!b.inited) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_probe(b.handle, addr7, timeout_ms);
}

esp_err_t read_reg8(Device d, uint8_t reg, uint8_t* buf, size_t len, int timeout_ms) {
    BusState& b = g_bus[d.bus];
    return i2c_master_transmit_receive(b.dev[d.slot], &reg, 1, buf, len, timeout_ms);
}

esp_err_t read_reg16(Device d, uint16_t reg, uint8_t* buf, size_t len, int timeout_ms) {
    BusState& b = g_bus[d.bus];
    const uint8_t a[2] = {static_cast<uint8_t>(reg >> 8), static_cast<uint8_t>(reg & 0xFF)};
    return i2c_master_transmit_receive(b.dev[d.slot], a, sizeof(a), buf, len, timeout_ms);
}

esp_err_t write_reg8(Device d, uint8_t reg, uint8_t value, int timeout_ms) {
    BusState& b = g_bus[d.bus];
    const uint8_t tx[2] = {reg, value};
    return i2c_master_transmit(b.dev[d.slot], tx, sizeof(tx), timeout_ms);
}

esp_err_t recover(Bus bus) {
    BusState& b = state(bus);
    if (!b.inited) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGW(TAG, "recovering bus %d (SDA=%d SCL=%d)", static_cast<int>(bus), b.sda, b.scl);

    for (uint8_t i = 0; i < b.n_dev; ++i) {
        if (b.dev[i] != nullptr) {
            i2c_master_bus_rm_device(b.dev[i]);
            b.dev[i] = nullptr;
        }
    }
    if (b.handle != nullptr) {
        i2c_del_master_bus(b.handle);
        b.handle = nullptr;
    }

    bitbang_clear(b.sda, b.scl);

    esp_err_t err = create_bus(b, static_cast<int>(bus));
    if (err != ESP_OK) {
        b.inited = false;
        return err;
    }
    for (uint8_t i = 0; i < b.n_dev; ++i) {
        err = create_device(b, i);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

}  // namespace hal_i2c
