#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/sys_heap.h>
#include <zephyr/sys/mem_stats.h>

#include "bh_platform.h"
#include "bh_assert.h"
#include "bh_log.h"
#include "wasm_export.h"

/* Se non esiste nella build, resta NULL e non rompe il link */
extern struct sys_heap _system_heap __attribute__((weak));

#define WAMR_GLOBAL_POOL_SIZE (216 * 1024)
static uint8_t g_wamr_pool[WAMR_GLOBAL_POOL_SIZE] __attribute__((aligned(8)));

/* ------------------------ Config ------------------------ */

#define MAX_MODULES 2

#define LINE_BUF_SIZE 256
#define MAX_CALL_ARGS 4

#define CONFIG_APP_STACK_SIZE 4096
#define CONFIG_APP_HEAP_SIZE  4096

#define COMM_THREAD_STACK_SIZE   4096 
#define COMM_THREAD_PRIORITY     5

#define WORKER_THREAD_STACK_SIZE 4096 
#define WORKER_THREAD_PRIORITY   6

#define UART_DEVICE_NODE DT_CHOSEN(zephyr_shell_uart)
#define LED0_NODE        DT_ALIAS(led0)

/* ------------------------ UART MsgQ ------------------------ */

K_MSGQ_DEFINE(uart_msgq, LINE_BUF_SIZE, 4, 4);

/* ------------------------ Types ------------------------ */

typedef enum { MOD_EMPTY=0, MOD_LOADED, MOD_RUNNING } mod_state_t;

typedef struct {
    char     func_name[64];
    uint32_t argc;
    uint32_t argv[MAX_CALL_ARGS];
} run_request_t;

typedef struct {
    bool used;
    char module_id[32];

    uint8_t *wasm_buf;
    uint32_t wasm_size;
    wasm_module_t module;
    wasm_module_inst_t inst;

    volatile bool stop_requested;
    volatile bool busy;
    mod_state_t state;

    struct k_thread thread;
    k_tid_t tid;
    struct k_sem work_sem;   /* init runtime con k_sem_init(&sem,0,1) [web:322] */

    run_request_t req;
} module_slot_t;

/* ------------------------ Globals ------------------------ */

static module_slot_t g_mods[MAX_MODULES];

K_THREAD_STACK_DEFINE(comm_thread_stack, COMM_THREAD_STACK_SIZE);
static struct k_thread comm_thread;

K_THREAD_STACK_ARRAY_DEFINE(worker_stacks, MAX_MODULES, WORKER_THREAD_STACK_SIZE);

static const struct device *uart_dev;

static const struct device *gpio_dev;
static uint32_t gpio_pin;

/* mutex per evitare interleaving output UART tra worker/comm */
K_MUTEX_DEFINE(uart_tx_mutex);

/* mutex per il caricamento dei moduli*/
K_MUTEX_DEFINE(load_mutex);

/* RX ISR state */
static char rx_buf[LINE_BUF_SIZE];
static int  rx_buf_pos;

typedef enum { RX_STATE_LINE=0, RX_STATE_BINARY } rx_state_t;
static volatile rx_state_t g_rx_state = RX_STATE_LINE;

/* buffer binario (usato solo durante LOAD, 1 alla volta) */
static uint8_t *g_bin_buf      = NULL;
static size_t   g_bin_expected = 0;
static size_t   g_bin_received = 0;
K_SEM_DEFINE(bin_sem, 0, 1);

/* ------------------------ Prototypes ------------------------ */

static void agent_write_str(const char *s);
static int  agent_read_line(char *buf, size_t max_len);

static uint32_t crc32_calc(const uint8_t *data, size_t len);

static void handle_command_line(char *line);
static void handle_load_cmd(const char *line);
static void handle_start_cmd(const char *line);
static void handle_stop_cmd(const char *line);
static void handle_status_cmd(const char *line);

static bool wasm_runtime_init_all(void);

static module_slot_t *slot_find(const char *module_id);
static module_slot_t *slot_alloc(const char *module_id);
static void slot_cleanup(module_slot_t *slot);

static void module_worker(void *p1, void *p2, void *p3);
static module_slot_t *slot_from_current_thread(void);

/* ------------------------ CRC32 (zlib) ------------------------ */

static uint32_t crc32_calc(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            uint32_t lsb  = crc & 1u;
            uint32_t mask = -(int32_t)lsb;
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

/* ------------------------ UART ISR ------------------------ */

static void serial_cb(const struct device *dev, void *user_data)
{
    uint8_t c;
    ARG_UNUSED(dev);
    ARG_UNUSED(user_data);

    if (!uart_irq_update(uart_dev) || !uart_irq_rx_ready(uart_dev)) {
        return;
    }

    while (uart_fifo_read(uart_dev, &c, 1) == 1) {
        if (g_rx_state == RX_STATE_LINE) {
            if ((c == '\n' || c == '\r') && rx_buf_pos > 0) {
                rx_buf[rx_buf_pos] = '\0';
                k_msgq_put(&uart_msgq, &rx_buf, K_NO_WAIT);
                rx_buf_pos = 0;
            } else if (rx_buf_pos < (int)(sizeof(rx_buf) - 1)) {
                rx_buf[rx_buf_pos++] = (char)c;
            }
        } else { /* RX_STATE_BINARY */
            if (g_bin_buf != NULL && g_bin_received < g_bin_expected) {
                g_bin_buf[g_bin_received++] = c;
                if (g_bin_received == g_bin_expected) {
                    g_rx_state = RX_STATE_LINE;
                    k_sem_give(&bin_sem);
                }
            }
        }
    }
}

/* ------------------------ GPIO native ------------------------ */

static int gpio_init_for_wasm(void)
{
    const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

    if (!device_is_ready(led.port)) {
        return -1;
    }
    if (gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE) < 0) {
        return -1;
    }

    gpio_dev = led.port;
    gpio_pin = led.pin;
    return 0;
}

static void gpio_toggle_native(wasm_exec_env_t exec_env)
{
    ARG_UNUSED(exec_env);
    if (!gpio_dev) {
        return;
    }
    gpio_pin_toggle(gpio_dev, gpio_pin);
    k_msleep(1000);
}

static int32_t should_stop_native(wasm_exec_env_t exec_env)
{
    ARG_UNUSED(exec_env);
    module_slot_t *slot = slot_from_current_thread();
    return (slot && slot->stop_requested) ? 1 : 0;
}

static NativeSymbol native_symbols[] = {
    { "gpio_toggle",  gpio_toggle_native,  "()"  },
    { "should_stop", (void *)should_stop_native, "()i" },
};

/* ------------------------ Param parsing ------------------------ */

static const char *find_param(const char *line, const char *key)
{
    size_t key_len = strlen(key);
    const char *p = line;

    while ((p = strstr(p, key)) != NULL) {
        if (p[key_len] == '=') {
            return p + key_len + 1;
        }
        p++;
    }
    return NULL;
}

static void copy_param_value(const char *start, char *dst, size_t dst_len)
{
    size_t i = 0;
    while (start[i] != '\0' && start[i] != ' ' && start[i] != '\r' && start[i] != '\n') {
        if (i + 1 < dst_len) {
            dst[i] = start[i];
        }
        i++;
    }
    if (dst_len > 0) {
        dst[i < dst_len ? i : dst_len - 1] = '\0';
    }
}

/* ------------------------ Slot management ------------------------ */

static module_slot_t *slot_find(const char *module_id)
{
    for (int i = 0; i < MAX_MODULES; i++) {
        if (g_mods[i].used && (strncmp(g_mods[i].module_id, module_id, sizeof(g_mods[i].module_id)) == 0)) {
            return &g_mods[i];
        }
    }
    return NULL;
}

static void slot_cleanup(module_slot_t *slot)
{
    if (!slot) return;

    slot->stop_requested = false;
    slot->busy = false;
    slot->state = MOD_EMPTY;

    if (slot->inst) {
        wasm_runtime_deinstantiate(slot->inst);
        slot->inst = NULL;
    }
    if (slot->module) {
        wasm_runtime_unload(slot->module);
        slot->module = NULL;
    }
    if (slot->wasm_buf) {
        wasm_runtime_free(slot->wasm_buf);
        slot->wasm_buf = NULL;
    }
    slot->wasm_size = 0;
}

static module_slot_t *slot_alloc(const char *module_id)
{
    for (int i = 0; i < MAX_MODULES; i++) {
        module_slot_t *slot = &g_mods[i];
        if (!slot->used) {
            memset(slot, 0, sizeof(*slot));
            slot->used = true;
            strncpy(slot->module_id, module_id, sizeof(slot->module_id) - 1);

            /* work_sem è dentro struct => init runtime con k_sem_init [web:322] */
            k_sem_init(&slot->work_sem, 0, 1);

            slot->tid = k_thread_create(
                &slot->thread,
                worker_stacks[i],
                K_THREAD_STACK_SIZEOF(worker_stacks[i]),
                module_worker,
                slot, NULL, NULL,
                WORKER_THREAD_PRIORITY,
                0,
                K_NO_WAIT
            );

            slot->state = MOD_EMPTY;
            return slot;
        }
    }
    return NULL;
}

/* ------------------------ Worker thread ------------------------ */

static module_slot_t *slot_from_current_thread(void)
{
    k_tid_t me = k_current_get();
    for (int i = 0; i < MAX_MODULES; i++) {
        if (g_mods[i].used && g_mods[i].tid == me) {
            return &g_mods[i];
        }
    }
    return NULL;
}

static void module_worker(void *p1, void *p2, void *p3)
{
    module_slot_t *slot = (module_slot_t *)p1;
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    /* thread creato dall’host (Zephyr) => init/destroy thread env WAMR [web:33][web:51] */
    if (!wasm_runtime_init_thread_env()) {
        agent_write_str("ERROR code=WAMR_THREAD_ENV_INIT_FAIL\n");
        return;
    }

    for (;;) {
        k_sem_take(&slot->work_sem, K_FOREVER);

        if (!slot->inst) {
            slot->busy = false;
            slot->stop_requested = false;
            slot->state = MOD_LOADED;
            continue;
        }

        run_request_t req;
        memcpy(&req, &slot->req, sizeof(req));

        wasm_function_inst_t fn = wasm_runtime_lookup_function(slot->inst, req.func_name);
        if (!fn) {
            agent_write_str("RESULT status=NO_FUNC\n");
            slot->busy = false;
            slot->stop_requested = false;
            slot->state = MOD_LOADED;
            continue;
        }

        uint32_t result_count = wasm_func_get_result_count(fn, slot->inst);

        wasm_exec_env_t exec_env = wasm_runtime_create_exec_env(slot->inst, CONFIG_APP_STACK_SIZE);
        if (!exec_env) {
            agent_write_str("RESULT status=NO_EXEC_ENV\n");
            slot->busy = false;
            slot->stop_requested = false;
            slot->state = MOD_LOADED;
            continue;
        }

        uint32 argv_local[MAX_CALL_ARGS];
        uint32 argc = req.argc;
        for (uint32 i = 0; i < argc && i < MAX_CALL_ARGS; i++) {
            argv_local[i] = req.argv[i];
        }

        slot->state = MOD_RUNNING;
        bool ok = wasm_runtime_call_wasm(exec_env, fn, argc, argv_local);

        const char *exc = NULL;
        if (!ok) {
            exc = wasm_runtime_get_exception(slot->inst);
        }

        char out[256];
        if (!ok) {
            snprintf(out, sizeof(out),
                     "RESULT status=EXCEPTION module_id=%s func=%s msg=\"%s\"\n",
                     slot->module_id, req.func_name, exc ? exc : "<none>");
        } else if (slot->stop_requested) {
            snprintf(out, sizeof(out),
                     "RESULT status=STOPPED module_id=%s func=%s\n",
                     slot->module_id, req.func_name);
        } else if (result_count > 0) {
            uint32_t ret_i32 = argv_local[0];
            snprintf(out, sizeof(out),
                     "RESULT status=OK module_id=%s func=%s ret_i32=%lu\n",
                     slot->module_id, req.func_name, (unsigned long)ret_i32);
        } else {
            snprintf(out, sizeof(out),
                     "RESULT status=OK module_id=%s func=%s\n",
                     slot->module_id, req.func_name);
        }

        agent_write_str(out);

        wasm_runtime_destroy_exec_env(exec_env);

        slot->busy = false;
        slot->stop_requested = false;
        slot->state = slot->inst ? MOD_LOADED : MOD_EMPTY;
    }

    wasm_runtime_destroy_thread_env();
}

/* ------------------------ Command handlers ------------------------ */

static void handle_load_cmd(const char *line)
{
    k_mutex_lock(&load_mutex, K_FOREVER);  /* serializza LOAD */

    char size_str[16];
    char crc_str[16];
    char module_id_buf[32];
    char out_buf[200];

    const char *p_mod  = find_param(line, "module_id");
    const char *p_size = find_param(line, "size");
    const char *p_crc  = find_param(line, "crc32");

    if (!p_mod || !p_size || !p_crc) {
        agent_write_str("LOAD_ERR code=BAD_PARAMS msg=\"missing module_id/size/crc32\"\n");
        goto out;
    }

    copy_param_value(p_mod, module_id_buf, sizeof(module_id_buf));
    copy_param_value(p_size, size_str, sizeof(size_str));
    copy_param_value(p_crc,  crc_str,  sizeof(crc_str));

    uint32_t size = (uint32_t)atoi(size_str);
    if (size == 0) {
        agent_write_str("LOAD_ERR code=BAD_PARAMS msg=\"size=0\"\n");
        goto out;
    }

    uint32_t crc_expected = (uint32_t)strtoul(crc_str, NULL, 16);

    module_slot_t *slot = slot_find(module_id_buf);
    if (!slot) {
        slot = slot_alloc(module_id_buf);
    }
    if (!slot) {
        agent_write_str("LOAD_ERR code=NO_SLOT msg=\"MAX_MODULES reached\"\n");
        goto out;
    }

    if (slot->busy) {
        agent_write_str("LOAD_ERR code=BUSY msg=\"module running\"\n");
        goto out;
    }

    /* ricarico lo slot */
    slot_cleanup(slot);

    slot->wasm_buf = (uint8_t *)wasm_runtime_malloc(size);
    if (!slot->wasm_buf) {
        agent_write_str("LOAD_ERR code=NO_MEM\n");
        goto out;
    }
    slot->wasm_size = size;

    /* prepara RX binaria (1 LOAD alla volta) */
    unsigned int key = irq_lock();
    g_bin_buf      = slot->wasm_buf;
    g_bin_expected = slot->wasm_size;
    g_bin_received = 0;
    g_rx_state     = RX_STATE_BINARY;
    k_sem_reset(&bin_sem);
    irq_unlock(key);

    snprintf(out_buf, sizeof(out_buf),
             "LOAD_READY module_id=%s size=%lu crc32=%s\n",
             slot->module_id, (unsigned long)slot->wasm_size, crc_str);
    agent_write_str(out_buf);

    if (k_sem_take(&bin_sem, K_SECONDS(5)) != 0) {
        agent_write_str("LOAD_ERR code=TIMEOUT msg=\"binary payload not received\"\n");
        key = irq_lock();
        g_rx_state = RX_STATE_LINE;
        g_bin_buf = NULL;
        irq_unlock(key);
        slot_cleanup(slot);
        g_bin_expected=0; 
        g_bin_received=0;
        goto out;
    }

    /* stop bin RX pointers */
    key = irq_lock();
    g_bin_buf = NULL;
    irq_unlock(key);

    uint32_t crc_calc = crc32_calc(slot->wasm_buf, slot->wasm_size);
    if (crc_calc != crc_expected) {
        snprintf(out_buf, sizeof(out_buf),
                 "LOAD_ERR code=BAD_CRC msg=\"expected=%08lx got=%08lx\"\n",
                 (unsigned long)crc_expected, (unsigned long)crc_calc);
        agent_write_str(out_buf);
        slot_cleanup(slot);
        goto out;
    }

    char error_buf[128];
    slot->module = wasm_runtime_load(slot->wasm_buf, slot->wasm_size, error_buf, sizeof(error_buf));
    if (!slot->module) {
        snprintf(out_buf, sizeof(out_buf),
                 "LOAD_ERR code=LOAD_FAIL msg=\"%s\"\n", error_buf);
        agent_write_str(out_buf);
        slot_cleanup(slot);
        goto out;
    }

    slot->inst = wasm_runtime_instantiate(slot->module,
                                         CONFIG_APP_STACK_SIZE,
                                         CONFIG_APP_HEAP_SIZE,
                                         error_buf, sizeof(error_buf));
    if (!slot->inst) {
        snprintf(out_buf, sizeof(out_buf),
                 "LOAD_ERR code=INSTANTIATE_FAIL msg=\"%s\"\n", error_buf);
        agent_write_str(out_buf);
        slot_cleanup(slot);
        goto out;
    }

    slot->state = MOD_LOADED;
    agent_write_str("LOAD_OK\n");

out:
    k_mutex_unlock(&load_mutex);
}

static void handle_start_cmd(const char *line)
{
    char func_name[64];
    char args_buf[64];
    char module_id_buf[32];

    uint32_t argc = 0;

    const char *p_mod  = find_param(line, "module_id");
    const char *p_func = find_param(line, "func");
    if (!p_mod || !p_func) {
        agent_write_str("RESULT status=BAD_PARAMS msg=\"missing module_id/func\"\n");
        return;
    }

    copy_param_value(p_mod, module_id_buf, sizeof(module_id_buf));
    copy_param_value(p_func, func_name, sizeof(func_name));

    module_slot_t *slot = slot_find(module_id_buf);
    if (!slot || !slot->inst) {
        agent_write_str("RESULT status=NO_MODULE\n");
        return;
    }

    if (slot->busy) {
        agent_write_str("RESULT status=BUSY\n");
        return;
    }

    memset(&slot->req, 0, sizeof(slot->req));
    strncpy(slot->req.func_name, func_name, sizeof(slot->req.func_name) - 1);

    const char *p_args = find_param(line, "args");
    if (p_args && *p_args == '\"') {
        p_args++;
        const char *p_end = strchr(p_args, '\"');
        if (p_end) {
            size_t len = (size_t)(p_end - p_args);
            if (len >= sizeof(args_buf)) len = sizeof(args_buf) - 1;
            memcpy(args_buf, p_args, len);
            args_buf[len] = '\0';

            char *tok = strtok(args_buf, ",");
            while (tok && argc < MAX_CALL_ARGS) {
                char *eq = strchr(tok, '=');
                if (eq) {
                    int val = atoi(eq + 1);
                    slot->req.argv[argc++] = (uint32_t)val;
                }
                tok = strtok(NULL, ",");
            }
        }
    }
    slot->req.argc = argc;

    slot->stop_requested = false;
    slot->busy = true;

    k_sem_give(&slot->work_sem);

    agent_write_str("START_OK\n");
}

static void handle_stop_cmd(const char *line)
{
    char module_id_buf[32];
    const char *p_mod = find_param(line, "module_id");

    if (!p_mod) {
        agent_write_str("STOP_OK status=NO_JOB\n");
        return;
    }
    copy_param_value(p_mod, module_id_buf, sizeof(module_id_buf));

    module_slot_t *slot = slot_find(module_id_buf);
    if (!slot || !slot->busy) {
        agent_write_str("STOP_OK status=IDLE\n");
        return;
    }

    slot->stop_requested = true;
    agent_write_str("STOP_OK status=PENDING\n");
}

static void handle_status_cmd(const char *line)
{
    ARG_UNUSED(line);

    char out[512];
    char mods[256] = {0};

    for (int i = 0; i < MAX_MODULES; i++) {
        module_slot_t *s = &g_mods[i];
        if (!s->used || !s->inst) continue;

        const char *st = (s->state == MOD_RUNNING) ? "RUNNING" : "LOADED";

        char one[128];
        uint32_t free_stack = 0;
        (void)k_thread_stack_space_get(s->tid, &free_stack);

        snprintf(one, sizeof(one),
                 "%s:%s:wasm=%lu:stack_free=%lu",
                 s->module_id, st,
                 (unsigned long)s->wasm_size,
                 (unsigned long)free_stack);

        if (mods[0]) strncat(mods, ",", sizeof(mods) - strlen(mods) - 1);
        strncat(mods, one, sizeof(mods) - strlen(mods) - 1);
    }

    if (!mods[0]) strcpy(mods, "none");

    mem_alloc_info_t mi;
    if (wasm_runtime_get_mem_alloc_info(&mi)) {
        uint32_t used = mi.total_size - mi.total_free_size;
        snprintf(out, sizeof(out),
                "STATUS_OK modules=\"%s\" wamr_heap_total=%u wamr_heap_free=%u wamr_heap_used=%u wamr_heap_highmark=%u\n",
                mods,
                mi.total_size,
                mi.total_free_size,
                used,
                mi.highmark_size);
    } else {
        snprintf(out, sizeof(out),
                "STATUS_OK modules=\"%s\" wamr_heap=NA\n",
                mods);
    }

    agent_write_str(out);
}





/* ------------------------ Command dispatcher ------------------------ */

static void handle_command_line(char *line)
{
    size_t len = strlen(line);
    if (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
        line[len-1] = '\0';
    }

    char *cmd  = strtok(line, " ");
    char *rest = strtok(NULL, "");

    if (!cmd) return;

    if (strcmp(cmd, "LOAD") == 0) {
        handle_load_cmd(rest ? rest : "");
    } else if (strcmp(cmd, "START") == 0) {
        handle_start_cmd(rest ? rest : "");
    } else if (strcmp(cmd, "STOP") == 0) {
        handle_stop_cmd(rest ? rest : "");
    } else if (strcmp(cmd, "STATUS") == 0) {
        handle_status_cmd(rest ? rest : "");
    } else {
        agent_write_str("ERROR code=UNKNOWN_COMMAND\n");
    }
}

/* ------------------------ WAMR init ------------------------ */

static bool wasm_runtime_init_all(void)
{
    RuntimeInitArgs init_args;
    memset(&init_args, 0, sizeof(init_args));

    init_args.mem_alloc_type = Alloc_With_Pool;
    init_args.mem_alloc_option.pool.heap_buf  = g_wamr_pool;
    init_args.mem_alloc_option.pool.heap_size = sizeof(g_wamr_pool);

    init_args.native_module_name = "env";
    init_args.native_symbols = native_symbols;
    init_args.n_native_symbols = sizeof(native_symbols) / sizeof(native_symbols[0]);

    if (!wasm_runtime_full_init(&init_args)) {
        agent_write_str("ERROR code=WAMR_INIT_FAIL\n");
        return false;
    }

#if WASM_ENABLE_LOG != 0
    bh_log_set_verbose_level(0);
#endif

    return true;
}

/* ------------------------ COMM thread ------------------------ */

static void comm_thread_entry(void *arg1, void *arg2, void *arg3)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    uart_dev = DEVICE_DT_GET(UART_DEVICE_NODE);
    if (!device_is_ready(uart_dev)) {
        printk("UART device not ready!\n");
        return;
    }

    int ret = uart_irq_callback_user_data_set(uart_dev, serial_cb, NULL);
    if (ret < 0) {
        printk("Error setting UART callback: %d\n", ret);
        return;
    }
    uart_irq_rx_enable(uart_dev);

    if (!wasm_runtime_init_all()) {
        return;
    }

    if (gpio_init_for_wasm() != 0) {
        agent_write_str("ERROR code=GPIO_INIT_FAIL\n");
        return;
    }

    agent_write_str("HELLO device_id=nucleo_f746zg rtos=Zephyr runtime=WAMR fw_version=1.0.0\n");

    char line_buf[LINE_BUF_SIZE];
    for (;;) {
        int n = agent_read_line(line_buf, sizeof(line_buf));
        if (n <= 0) {
            continue;
        }
        handle_command_line(line_buf);
    }
}

/* ------------------------ Init + main ------------------------ */

bool iwasm_init(void)
{
    k_tid_t tid_comm = k_thread_create(
        &comm_thread,
        comm_thread_stack,
        COMM_THREAD_STACK_SIZE,
        comm_thread_entry,
        NULL, NULL, NULL,
        COMM_THREAD_PRIORITY,
        0,
        K_NO_WAIT
    );

    return (tid_comm != NULL);
}

int main(void)
{
    (void)iwasm_init();
    while (1) {
        k_sleep(K_FOREVER);
    }
}

/* ------------------------ UART I/O ------------------------ */

static void agent_write_str(const char *buf)
{
    if (!uart_dev || !buf) {
        return;
    }

    k_mutex_lock(&uart_tx_mutex, K_FOREVER);

    int msg_len = strlen(buf);
    for (int i = 0; i < msg_len; i++) {
        uart_poll_out(uart_dev, buf[i]);
    }

    k_mutex_unlock(&uart_tx_mutex);
}

static int agent_read_line(char *buf, size_t max_len)
{
    if (!buf || max_len == 0) {
        return -1;
    }

    char local_buf[LINE_BUF_SIZE];
    if (k_msgq_get(&uart_msgq, &local_buf, K_FOREVER) != 0) {
        return -1;
    }

    size_t len = strlen(local_buf);
    if (len >= max_len) {
        len = max_len - 1;
    }

    memcpy(buf, local_buf, len);
    buf[len] = '\0';
    return (int)len;
}
