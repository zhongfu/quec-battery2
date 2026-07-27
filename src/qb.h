#ifndef QB_H
#define QB_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define QB_MAX_PDOS 10
#define QB_EVENT_CAPACITY 128
#define QB_EVENT_SIZE 32

#define QB_STOCK_MAX_CURRENT_MA 5300
#define QB_STOCK_MIN_SHUTDOWN_MV 3400
#define QB_SAFE_MAX_SHUTDOWN_MV 3800
#define QB_PPS_MIN_VOLTAGE_MV 6600
#define QB_STOCK_MAX_PPS_VOLTAGE_MV 9800
#define QB_MIN_PD_FULL_MV 3401
#define QB_STOCK_PD_FULL_MV 4200
#define QB_MIN_CHARGE_LIMIT_MV 3800
#define QB_MAX_CHARGE_LIMIT_MV 4200
#define QB_CAPACITY_LIMIT_HYSTERESIS 3
#define QB_CAPACITY_LIMIT_SAMPLES 3
#define QB_PUMP_START_RATIO_PERCENT 220
#define QB_PUMP_LIMIT_RATIO_PERCENT 220
#define QB_PUMP_LIMIT_ENTRY_MARGIN_MV 100
#define QB_PPS_VOLTAGE_STEP_MV 20
#define QB_PUMP_LIMIT_FINE_WINDOW_MV 100
#define QB_PUMP_HANDOFF_MARGIN_MV 25
#define QB_PUMP_HANDOFF_OVERSHOOT_MV 50
#define QB_PUMP_HANDOFF_SAMPLES 2

#define QB_LOG_FLAG "/tmp/quec_battery_log"
#define QB_UCI_CONFIG "/etc/config/qlbattery"
#define QB_WDT_SOCKET "/tmp/wdt_server.sock"

#define QB_SGM41542_PATH "/sys/devices/platform/soc/9c0000.qcom,qupv3_0_geni_se/994000.i2c/i2c-2/2-006b/sgm41542s/"
#define QB_SGM41600_PATH "/sys/devices/platform/soc/9c0000.qcom,qupv3_0_geni_se/994000.i2c/i2c-2/2-006f/sgm41600/"
#define QB_BATTERY_PATH "/sys/devices/platform/soc/9c0000.qcom,qupv3_0_geni_se/994000.i2c/i2c-2/2-0064/power_supply/cw221X-bat/"
#define QB_CW2217_PATH "/sys/devices/platform/soc/9c0000.qcom,qupv3_0_geni_se/994000.i2c/i2c-2/2-0064/cw2217/"
#define QB_PDA_PATH "/sys/devices/platform/soc/9c0000.qcom,qupv3_0_geni_se/998000.i2c/i2c-3/3-0022/AW35615-A/"
#define QB_PDB_PATH "/sys/devices/platform/soc/9c0000.qcom,qupv3_0_geni_se/980000.i2c/i2c-0/0-0022/AW35615-B/"
#define QB_SSUSB_PATH "/sys/devices/platform/soc/a600000.ssusb/"

#define QBLOG(line, fmt, ...) qb_log(__func__, (line), (fmt), ##__VA_ARGS__)

enum qb_power_role {
    QB_ROLE_SINK = 0,
    QB_ROLE_SOURCE = 1,
    QB_ROLE_UNKNOWN = 2,
};

enum qb_cc_pin {
    QB_CC_NONE = 0,
    QB_CC1 = 1,
    QB_CC2 = 2,
};

enum qb_battery_presence {
    QB_BATTERY_UNKNOWN = 0,
    QB_BATTERY_ABSENT = 1,
    QB_BATTERY_PRESENT = 2,
};

enum qb_mode {
    QB_MODE_NONE = 0,
    QB_MODE_PORT_A = 1,
    QB_MODE_PORT_B = 2,
    QB_MODE_BOTH = 3,
    QB_MODE_RESELECT = 4,
};

/* Original temp_status values. Values 2..4 permit PPS charge-pump use;
 * values 1..5 permit buck charging. */
enum qb_temp_status {
    QB_TEMP_COLD = 0,
    QB_TEMP_COOL = 1,
    QB_TEMP_NORMAL = 2,
    QB_TEMP_NORMAL_HIGH = 3,
    QB_TEMP_WARM = 4,
    QB_TEMP_HOT = 5,
    QB_TEMP_OVERHEAT = 6,
};

struct qb_pdo {
    int min_voltage_mv;
    int max_voltage_mv;
    int current_ma;
    int number;
    bool selected;
    bool pps;
    int requested_voltage_mv;
    int requested_current_ma;
};

struct qb_manager;

struct qb_pd_port {
    const char *name;
    const char *path;
    struct qb_manager *manager;
    enum qb_cc_pin cc_pin;
    bool data_role_dfp;
    enum qb_power_role power_role;
    struct qb_pdo pdo[QB_MAX_PDOS];
    int pdo_count;

    bool fixed_5v;
    int fixed_5v_current_ma;
    int fixed_5v_number;
    bool fixed_9v;
    int fixed_9v_current_ma;
    int fixed_9v_number;
    bool fixed_12v;
    int fixed_12v_current_ma;
    int fixed_12v_number;

    bool supports_pps;
    int pps_min_voltage_mv;
    int pps_max_voltage_mv;
    int pps_current_ma;
    int max_voltage_mv;

    bool attached;
    bool working;
    bool telemetry_valid;
    unsigned telemetry_failures;
    bool pump_handoff_complete;
    unsigned pump_handoff_samples;
};

struct qb_sgm41542 {
    int charge_en;
    int ichrg_curr_ua;
    int vreg_uv;
    int vbus_ovp_uv;
    int vindpm_uv;
    int vbus_adc_mv;
    int ibus_adc_ma;
    int vbat_adc_mv;
    int ibat_adc_ma;
    bool working;
    int charge_status;
    bool telemetry_valid;
    unsigned telemetry_failures;
};

struct qb_sgm41600 {
    int charge_en;
    int vbus_ocp_ua;
    int vbus_ovp_uv;
    int bat_ovp_uv;
    int bat_ocp_ua;
    int vbus_adc_mv;
    int ibus_adc_ma;
    int vbat_adc_mv;
    int ibat_adc_ma;
    bool working;
    int charge_status;
    bool telemetry_valid;
    unsigned telemetry_failures;
};

struct qb_battery {
    enum qb_battery_presence presence;
    int raw_present;
    int capacity;
    int voltage_mv;
    int current_ma;
    int temp_decic;
    int cycle_count;
    char health[12];
    bool telemetry_valid;
    unsigned telemetry_failures;
};

struct qb_event_queue {
    char items[QB_EVENT_CAPACITY][QB_EVENT_SIZE];
    unsigned front;
    unsigned rear;
    unsigned count;
    pthread_mutex_t mutex;
};

struct qb_manager {
    int max_current_ma;
    int min_shutdown_mv;
    int max_pd_vbus_mv;

    int charge_limit_mv;
    int charge_limit_percent;
    struct qb_pd_port pda;
    struct qb_pd_port pdb;
    struct qb_sgm41542 buck;
    struct qb_sgm41600 pump;
    struct qb_battery battery;

    enum qb_mode mode;
    enum qb_mode work_mode;
    enum qb_temp_status temp_status;
    int current_state;
    int otg_mode;

    int pd_full_mv;
    int full_voltage_mv;
    int charge_current_ma;
    int pps_voltage_mv;
    int qc_max_voltage_mv;
    int fixed_charge_current_ma;
    int buck_input_current_ua;
    int buck_charge_current_ua;
    bool qc_9v_supported;
    bool qc_12v_supported;

    bool charge_mode_switching;
    bool change_power_role;
    bool battery_offline_event;
    bool power_limit;
    int hiz_status;
    bool pwm_enabled;
    bool ovp_status[2];
    bool mos_status[2];
    bool dead_battery_restore;

    int pump_error_count;
    bool pump_error;
    int v42_capacity;
    int buck_error_count;
    int programmed_buck_voltage_mv;
    int programmed_pump_voltage_mv;
    bool capacity_charge_hold;
    unsigned capacity_stop_samples;
    unsigned capacity_resume_samples;

    pthread_mutex_t reset_mutex;
    struct qb_event_queue events;
    volatile bool running;

    pthread_t pump_thread;
    pthread_t buck_thread;
    pthread_t gauge_thread;
    pthread_t watchdog_thread;
    pthread_t event_thread;
    pthread_t thermal_thread;
};

extern volatile int qb_power_limit_state;

void qb_log(const char *func, int line, const char *fmt, ...);
int qb_read_int(const char *dir, const char *attr, int *value);
int qb_read_str(const char *dir, const char *attr, char *buf, size_t size);
int qb_write_str(const char *dir, const char *attr, const char *value);
int qb_write_int(const char *dir, const char *attr, int value);

bool qb_queue_empty(struct qb_manager *cm);
bool qb_queue_full(struct qb_manager *cm);
unsigned qb_queue_length(struct qb_manager *cm);
bool qb_queue_enqueue(struct qb_manager *cm, const char *event);
bool qb_queue_dequeue(struct qb_manager *cm, char out[QB_EVENT_SIZE]);
int qb_interruptible_sleep(struct qb_manager *cm, unsigned seconds);

int qb_parse_pdo_line(const char *line, struct qb_pdo *pdo, int index);
int qb_get_pdo_info(struct qb_pd_port *port);
int qb_get_port_info(struct qb_pd_port *port, bool read_connection);
bool qb_request_pdo(struct qb_pd_port *port, int voltage_or_pdo,
                    int current_ma);
bool qb_pps_voltage_matches(int requested_mv, int measured_mv);

int qb_get_sgm41542_info(struct qb_manager *cm);
int qb_get_sgm41600_info(struct qb_manager *cm);
int qb_get_battery_info(struct qb_manager *cm);
enum qb_battery_presence qb_battery_presence_from_raw(int raw_present);
int qb_get_battery_online(struct qb_manager *cm);
bool qb_battery_present(const struct qb_manager *cm);
bool qb_battery_absent(const struct qb_manager *cm);
int qb_set_sgm41542(struct qb_manager *cm, const char *attr, const char *value);
int qb_set_sgm41542_int(struct qb_manager *cm, const char *attr, int value);
int qb_set_sgm41600(struct qb_manager *cm, const char *attr, const char *value);
int qb_program_buck_voltage_limit(struct qb_manager *cm);
int qb_program_pump_voltage_limit(struct qb_manager *cm);
void qb_ovp_on(struct qb_manager *cm, int port);
void qb_ovp_off(struct qb_manager *cm, int port);
void qb_mos_on(struct qb_manager *cm, int port);
void qb_mos_off(struct qb_manager *cm, int port);
void qb_disable_buck(struct qb_manager *cm);
void qb_disable_pump(struct qb_manager *cm);
void qb_enable_buck_cfg(struct qb_manager *cm, struct qb_pd_port *port);
void qb_disable_buck_cfg(struct qb_manager *cm, struct qb_pd_port *port);
void qb_enable_pump_cfg(struct qb_manager *cm, struct qb_pd_port *port);
void qb_disable_pump_cfg(struct qb_manager *cm, struct qb_pd_port *port);
void qb_enable_buck(struct qb_manager *cm, struct qb_pd_port *port);
void qb_set_pwm(bool enabled);

void qb_no_charge(struct qb_manager *cm);
void qb_pump_pps_control(struct qb_manager *cm);
void qb_fixed_charge_control(struct qb_manager *cm);
int qb_pump_target_mv(const struct qb_manager *cm);
int qb_pump_start_voltage_mv(const struct qb_manager *cm, int battery_mv);
bool qb_pump_entry_voltage_ok(const struct qb_manager *cm, int battery_mv);
int qb_select_qc_max_voltage(struct qb_manager *cm);
void qb_mode1_charge(struct qb_manager *cm);
void qb_mode2_charge(struct qb_manager *cm);
void qb_mode3_charge(struct qb_manager *cm);
int qb_enter_mode0(struct qb_manager *cm);
int qb_enter_mode1(struct qb_manager *cm);
bool qb_pump_allowed(struct qb_manager *cm, struct qb_pd_port *port, int vbat_mv);
bool qb_pump_handoff_ready(struct qb_manager *cm, struct qb_pd_port *port);
int qb_enter_mode2(struct qb_manager *cm);
int qb_enter_mode3(struct qb_manager *cm);
void qb_select_mode(struct qb_manager *cm);
void qb_reset_charge_state(struct qb_manager *cm);

void *qb_pump_monitor(void *arg);
void *qb_buck_monitor(void *arg);
void *qb_event_monitor(void *arg);
int qb_connect_unix_socket(const char *path, int timeout_ms);
void *qb_watchdog_monitor(void *arg);
void *qb_gauge_monitor(void *arg);
void *qb_thermal_monitor(void *arg);
int qb_receive_uevents(struct qb_manager *cm);

int qb_load_config(struct qb_manager *cm);
int qb_load_config_file(struct qb_manager *cm, const char *path);
int qb_limit_charge_current(const struct qb_manager *cm, int requested_ma);
bool qb_pps_enabled(const struct qb_manager *cm);
int qb_limit_pps_voltage(const struct qb_manager *cm, int requested_mv);
bool qb_low_voltage_danger(const struct qb_manager *cm, bool adapter_online,
                           int battery_mv);
int qb_charge_voltage_limit(const struct qb_manager *cm, int policy_mv);
int qb_sgm41600_voltage_registers(int target_mv, unsigned *bat_ovp,
                                  unsigned *regulation);
int qb_update_register(const char *dir, unsigned reg, unsigned mask,
                       unsigned value);
bool qb_update_capacity_charge_limit(struct qb_manager *cm);
void qb_update_charge_limits(struct qb_manager *cm);
void qb_init_temp_status(struct qb_manager *cm);
int qb_set_battery_cycle(struct qb_manager *cm);
int qb_update_battery_cycle(struct qb_manager *cm);

int qb_thermal_netlink_init(struct qb_manager *cm);
#endif
