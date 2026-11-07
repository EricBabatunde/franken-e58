#include "hal_mcpwm.h"

#include "board_config.h"
#include "driver/mcpwm_prelude.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "hal/mcpwm_ll.h"
#include "output_gate.h"

// mcpwm_generator_set_force_level() is not placed in IRAM in IDF 5.5.3, even with
// CONFIG_MCPWM_CTRL_FUNC_IN_IRAM (esp_driver_mcpwm/linker.lf lists only the compare and period
// setters), and its error path logs. The force path therefore writes the generator force fields
// through the IDF low-level layer (hal/mcpwm_ll.h, static inline, so it compiles into the IRAM
// functions below). The operator and generator IDs it needs are not public: init() forces each
// generator through the public API and confirms which register field changed.
namespace hal_mcpwm {

namespace {

constexpr int kGroup = 0;
constexpr int kOutputs = 4;

mcpwm_timer_handle_t s_timer = nullptr;
mcpwm_oper_handle_t s_oper[2] = {nullptr, nullptr};
mcpwm_cmpr_handle_t s_cmpr[kOutputs] = {};
mcpwm_gen_handle_t s_gen[kOutputs] = {};
int s_oper_id[kOutputs] = {0, 0, 1, 1};
int s_gen_id[kOutputs] = {0, 1, 0, 1};
bool s_ready = false;     // timer running
bool s_force_ok = false;  // force fields identified: the IRAM force path is usable
volatile uint16_t s_duty = 0;
// Panic force-low (M7b, D1b): the gen_force register value of each operator with both generators
// forced low, captured from the hardware once the verified force path has set it. The panic
// wrapper writes these constants and nothing else.
uint32_t s_panic_force_val[2] = {0, 0};
bool s_panic_ready = false;

// 0: not forced, 1: forced low, 2: forced high (mcpwm_ll_gen_set_continue_force_level: level + 1).
inline uint32_t IRAM_ATTR force_mode(int oper, int gen) {
    mcpwm_dev_t* dev = MCPWM_LL_GET_HW(kGroup);
    return gen == 0 ? dev->operators[oper].gen_force.gen_a_cntuforce_mode
                    : dev->operators[oper].gen_force.gen_b_cntuforce_mode;
}

// Register layer for the output gate (output_gate.h). Interrupts are masked on the calling core
// (portSET_INTERRUPT_MASK_FROM_ISR nests, so the watchdog ISR may use it too).
struct DevOps {
    static UBaseType_t IRAM_ATTR mask() { return portSET_INTERRUPT_MASK_FROM_ISR(); }
    static void IRAM_ATTR unmask(UBaseType_t m) { portCLEAR_INTERRUPT_MASK_FROM_ISR(m); }
    static void IRAM_ATTR force_all_low() {
        if (!s_force_ok) return;  // before init the pins are plain GPIO outputs held low (hal_gpio)
        mcpwm_dev_t* dev = MCPWM_LL_GET_HW(kGroup);
        for (int i = 0; i < kOutputs; ++i) {
            mcpwm_ll_gen_set_continue_force_level(dev, s_oper_id[i], s_gen_id[i], 0);
        }
    }
    static void IRAM_ATTR release_all() {
        if (!s_ready) return;
        mcpwm_dev_t* dev = MCPWM_LL_GET_HW(kGroup);
        for (int i = 0; i < kOutputs; ++i) {
            mcpwm_ll_gen_disable_continue_force_action(dev, s_oper_id[i], s_gen_id[i]);
        }
    }
    static void IRAM_ATTR set_compare(uint16_t permille) {
        if (!s_ready) return;
        const uint32_t ticks = static_cast<uint32_t>(permille) * board::kPwmPeriodTicks / 1000u;
        for (int i = 0; i < kOutputs; ++i) {
            mcpwm_comparator_set_compare_value(s_cmpr[i], ticks);  // IRAM with CONFIG_MCPWM_CTRL_FUNC_IN_IRAM, no lock
        }
        s_duty = permille;
    }
};

OutputGate<DevOps> s_gate;

}  // namespace

InitReport init() {
    InitReport r;
    r.period_ticks = board::kPwmPeriodTicks;
    auto keep = [&r](esp_err_t e) {
        if (r.err == ESP_OK && e != ESP_OK) r.err = e;
        return e == ESP_OK;
    };

    mcpwm_timer_config_t tc = {};
    tc.group_id = kGroup;
    tc.clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT;  // PLL_F160M on the ESP32-S3
    tc.resolution_hz = board::kPwmResolutionHz;
    tc.count_mode = MCPWM_TIMER_COUNT_MODE_UP;
    tc.period_ticks = board::kPwmPeriodTicks;
    if (!keep(mcpwm_new_timer(&tc, &s_timer))) return r;
    // The driver warns ("adjust timer resolution") if the prescalers cannot give this exactly.
    r.resolution_hz = board::kPwmResolutionHz;

    for (int o = 0; o < 2; ++o) {
        mcpwm_operator_config_t oc = {};
        oc.group_id = kGroup;
        if (!keep(mcpwm_new_operator(&oc, &s_oper[o]))) return r;
        if (!keep(mcpwm_operator_connect_timer(s_oper[o], s_timer))) return r;
    }

    bool ids_ok = true;
    for (int i = 0; i < kOutputs; ++i) {
        mcpwm_oper_handle_t oper = s_oper[i / 2];
        mcpwm_comparator_config_t cc = {};
        cc.flags.update_cmp_on_tez = 1;  // new duty takes effect at the next period start
        if (!keep(mcpwm_new_comparator(oper, &cc, &s_cmpr[i]))) return r;
        if (!keep(mcpwm_comparator_set_compare_value(s_cmpr[i], 0))) return r;

        mcpwm_generator_config_t gc = {};
        gc.gen_gpio_num = board::kMotorGpio[i];
        gc.flags.pull_down = 1;  // belt and braces beside the 10 kOhm gate pull-down
        // The timer is not running and no action is set yet, so the generator holds its reset
        // level (low) from the moment the pin is routed (verify against the TRM MCPWM chapter).
        if (!keep(mcpwm_new_generator(oper, &gc, &s_gen[i]))) return r;

        // Force low first, through the public API, and confirm which register field moved.
        const uint32_t before = force_mode(s_oper_id[i], s_gen_id[i]);
        if (!keep(mcpwm_generator_set_force_level(s_gen[i], 0, true))) return r;
        ids_ok = ids_ok && before == 0 && force_mode(s_oper_id[i], s_gen_id[i]) == 1;

        if (!keep(mcpwm_generator_set_action_on_timer_event(
                s_gen[i], MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, MCPWM_TIMER_EVENT_EMPTY,
                                                       MCPWM_GEN_ACTION_HIGH)))) {
            return r;
        }
        if (!keep(mcpwm_generator_set_action_on_compare_event(
                s_gen[i], MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, s_cmpr[i], MCPWM_GEN_ACTION_LOW)))) {
            return r;
        }
    }
    for (int i = 0; i < kOutputs; ++i) {
        r.oper_id[i] = s_oper_id[i];
        r.gen_id[i] = s_gen_id[i];
    }
    r.ids_verified = ids_ok;
    s_force_ok = ids_ok;
    if (!ids_ok) {
        r.err = ESP_ERR_INVALID_STATE;  // the IRAM force path would act on the wrong fields
        return r;
    }
    r.forced_before_start = all_forced_low();
    if (!r.forced_before_start) {
        r.err = ESP_ERR_INVALID_STATE;
        return r;
    }
    // Capture the panic constants: both generators of operators 0 and 1 forced low, immediate update.
    {
        mcpwm_dev_t* dev = MCPWM_LL_GET_HW(kGroup);
        bool ok = true;
        for (int o = 0; o < 2; ++o) {
            mcpwm_gen_force_reg_t f;
            f.val = dev->operators[o].gen_force.val;
            ok = ok && f.gen_a_cntuforce_mode == 1 && f.gen_b_cntuforce_mode == 1 && f.gen_cntuforce_upmethod == 0;
            s_panic_force_val[o] = f.val;
        }
        s_panic_ready = ok;
        r.panic_force_ready = ok;
        r.panic_force_val[0] = s_panic_force_val[0];
        r.panic_force_val[1] = s_panic_force_val[1];
    }
    if (!keep(mcpwm_timer_enable(s_timer))) return r;
    if (!keep(mcpwm_timer_start_stop(s_timer, MCPWM_TIMER_START_NO_STOP))) return r;
    s_ready = true;
    return r;
}

uint16_t IRAM_ATTR set_all_permille(uint16_t permille) { return s_gate.set_permille(permille); }

void IRAM_ATTR force_low_all() { s_gate.force_low_all(); }

bool IRAM_ATTR release_all() { return s_gate.release_all(); }

void IRAM_ATTR hard_cut(uint8_t reason) { s_gate.hard_cut(reason == 0 ? 1 : reason); }

void IRAM_ATTR clear_hard_cut() { s_gate.clear_hard_cut(); }

uint8_t IRAM_ATTR hard_latched() { return s_gate.latched(); }

bool IRAM_ATTR all_forced_low() {
    if (!s_force_ok) return false;
    for (int i = 0; i < kOutputs; ++i) {
        if (force_mode(s_oper_id[i], s_gen_id[i]) != 1) return false;
    }
    return true;
}

uint16_t duty_permille() { return s_duty; }

bool panic_force_ready() { return s_panic_ready; }

}  // namespace hal_mcpwm

// Panic force-low (M7b, D1b). Linked with -Wl,--wrap=esp_panic_handler (CMakeLists.txt), the same
// mechanism IDF 5.5.3 uses in components/bt. Every panic (interrupt watchdog, task watchdog with
// CONFIG_ESP_TASK_WDT_PANIC, abort, exceptions, on either core) enters here first. Minimal by
// design: in IRAM, two constant register writes captured at init, then the real handler, which
// prints the backtrace and reboots. Nothing else: no lock, no read-modify-write, no logging.
extern "C" void __real_esp_panic_handler(void* info);
extern "C" void IRAM_ATTR __wrap_esp_panic_handler(void* info) {
    if (hal_mcpwm::s_panic_ready) {
        mcpwm_dev_t* dev = MCPWM_LL_GET_HW(hal_mcpwm::kGroup);
        dev->operators[0].gen_force.val = hal_mcpwm::s_panic_force_val[0];
        dev->operators[1].gen_force.val = hal_mcpwm::s_panic_force_val[1];
    }
    __real_esp_panic_handler(info);
}
