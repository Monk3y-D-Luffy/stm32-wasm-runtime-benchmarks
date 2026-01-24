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





void __atomic_store_4(volatile void *ptr, uint32_t val, int memmodel)
{
    ARG_UNUSED(memmodel);
    unsigned int key = irq_lock();
    *(volatile uint32_t *)ptr = val;
    irq_unlock(key);
}

bool __atomic_compare_exchange_4(volatile void *ptr, void *expected,
                                 uint32_t desired, bool weak,
                                 int success_memmodel, int failure_memmodel)
{
    ARG_UNUSED(weak);
    ARG_UNUSED(success_memmodel);
    ARG_UNUSED(failure_memmodel);

    unsigned int key = irq_lock();

    volatile uint32_t *p = (volatile uint32_t *)ptr;
    uint32_t exp = *(uint32_t *)expected;

    if (*p == exp) {
        *p = desired;
        irq_unlock(key);
        return true;
    }

    *(uint32_t *)expected = *p;
    irq_unlock(key);
    return false;
}





/* Se non esiste nella build, resta NULL e non rompe il link */
extern struct sys_heap _system_heap __attribute__((weak));

//#define WAMR_GLOBAL_POOL_SIZE (216 * 1024)
#define WAMR_GLOBAL_POOL_SIZE CONFIG_WAMR_GLOBAL_POOL_SIZE
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

#define LOAD_GUARD_BYTES   (8 * 1024)
#define START_GUARD_BYTES  (16 * 1024)

#define START_GUARD_BYTES_NEED_EXEC_ENV   (16 * 1024)
#define START_GUARD_BYTES_HAVE_EXEC_ENV   (4 * 1024)

#define STOP_FORCE_DELAY_MS 1200

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

    wasm_exec_env_t exec_env;   // << nuovo: exec_env persistente

    volatile bool stop_requested;
    volatile bool busy;
    mod_state_t state;

    struct k_thread thread;
    k_tid_t tid;
    struct k_sem work_sem;

    run_request_t req;

    struct k_work_delayable stop_dwork;
    struct k_work_sync stop_sync;
    volatile bool terminate_requested;
} module_slot_t;

/* ------------------------ Globals ------------------------ */

static module_slot_t g_mods[MAX_MODULES];

K_THREAD_STACK_DEFINE(comm_thread_stack, COMM_THREAD_STACK_SIZE);
static struct k_thread comm_thread;

K_THREAD_STACK_ARRAY_DEFINE(worker_stacks, MAX_MODULES, WORKER_THREAD_STACK_SIZE);

static const struct device *uart_dev;

//static const struct device *gpio_dev;
//static uint32_t gpio_pin;

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
K_MUTEX_DEFINE(uart_mutex);
/* Semaforo per serializzare accesso LED */
K_MUTEX_DEFINE(gpio_mutex);

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

const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

static int gpio_init_for_wasm(void)
{
    
    if (!device_is_ready(led.port)) {
        return -1;
    }
    if (gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE) < 0) {
        return -1;
    }

    //gpio_dev = led.port;
    //gpio_pin = led.pin;
    return 0;
}

static void gpio_toggle_native(wasm_exec_env_t exec_env)
{
    ARG_UNUSED(exec_env);
    //if (!gpio_dev) return;

    k_mutex_lock(&gpio_mutex, K_FOREVER);
    gpio_pin_toggle(led.port, led.pin);
    k_mutex_unlock(&gpio_mutex);

    k_msleep(1000);
}

/* env.uart_print(i32 offset)  */
static void uart_print_native(wasm_exec_env_t exec_env, uint32_t offset)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    if (!wasm_runtime_validate_app_str_addr(module_inst, offset)) return;

    const char *s = (const char *)wasm_runtime_addr_app_to_native(module_inst, offset);
    if (!s) return;

    k_mutex_lock(&uart_mutex, K_FOREVER);
    printk("%s", s);
    k_mutex_unlock(&uart_mutex);

    k_sleep(K_MSEC(1000));
}

/* Native import: env.led_toggle(i32 duration_ms) */
static void led_toggle_native(wasm_exec_env_t exec_env, uint32_t duration_ms)
{
    ARG_UNUSED(exec_env);

    k_mutex_lock(&gpio_mutex, K_FOREVER);

    printk("LED ON (thread %p)\n", k_current_get());
    gpio_pin_set_dt(&led, 1);
    k_sleep(K_MSEC(duration_ms));
    printk("LED OFF (thread %p)\n", k_current_get());
    gpio_pin_set_dt(&led, 0);

    k_mutex_unlock(&gpio_mutex);
}


static NativeSymbol native_symbols[] = {
    { "gpio_toggle",  gpio_toggle_native,  "()"  },
    { "uart_print", (void *)uart_print_native, "(i)" },
    { "led_toggle", (void *)led_toggle_native, "(i)" },
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

static int slot_index(module_slot_t *slot)
{
    return (int)(slot - g_mods);
}

static void slot_abort_worker(module_slot_t *slot)
{
    if (slot && slot->tid) {
        k_thread_abort(slot->tid);
        slot->tid = NULL;
    }
    /* best-effort: thread struct non più usato dopo abort */
    memset(&slot->thread, 0, sizeof(slot->thread));
}

static void slot_ensure_worker(module_slot_t *slot)
{
    if (!slot || slot->tid) {
        return;
    }

    int i = slot_index(slot);
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
}


static void slot_cleanup(module_slot_t *slot)
{
    if (!slot) return;

    slot->stop_requested = false;
    slot->busy = false;
    slot->state = MOD_EMPTY;

    if (slot->exec_env) {
        wasm_runtime_destroy_exec_env(slot->exec_env);
        slot->exec_env = NULL;
    }
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

static void stop_dwork_handler(struct k_work *work)
{
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    module_slot_t *slot = CONTAINER_OF(dwork, module_slot_t, stop_dwork);

    k_mutex_lock(&load_mutex, K_FOREVER);

    /* Se STOP soft ha già funzionato (o slot non valido), non fare nulla */
    if (!slot->used || !slot->busy || !slot->inst || !slot->terminate_requested) {
        k_mutex_unlock(&load_mutex);
        return;
    }

    /* Escalation: stop hard del thread worker */
    slot_abort_worker(slot); /* calls k_thread_abort() and sets slot->tid=NULL */  /* [web:622] */

    /*
     * Riporta lo slot a "LOADED" mantenendo il modulo caricato:
     * deinstanzia e reinstanzia lo stesso wasm_module_t.
     */
    if (slot->exec_env) {
        wasm_runtime_destroy_exec_env(slot->exec_env);
        slot->exec_env = NULL;
    }
    if (slot->inst) {
        wasm_runtime_deinstantiate(slot->inst);
        slot->inst = NULL;
    }

    char error_buf[128];
    slot->inst = wasm_runtime_instantiate(slot->module,
                                         CONFIG_APP_STACK_SIZE,
                                         CONFIG_APP_HEAP_SIZE,
                                         error_buf, sizeof(error_buf));

    if (slot->inst) {
        slot->exec_env = wasm_runtime_create_exec_env(slot->inst, CONFIG_APP_STACK_SIZE);
        if (!slot->exec_env) {
            /* Se fallisce exec_env, degrada a slot vuoto (coerente) */
            wasm_runtime_deinstantiate(slot->inst);
            slot->inst = NULL;
        }
    }

    /* Stato */
    slot->busy = false;
    slot->stop_requested = false;
    slot->terminate_requested = false;
    slot->state = slot->inst ? MOD_LOADED : MOD_EMPTY;

    /* Ricrea il worker thread (necessario dopo abort) */
    slot_ensure_worker(slot);

    /* RESULT finale coerente con il resto del protocollo */
    const char *func = slot->req.func_name[0] ? slot->req.func_name : "<unknown>";
    char out[256];
    snprintf(out, sizeof(out),
             "RESULT status=STOPPED forced=1 module_id=%s func=%s\n",
             slot->module_id, func);
    agent_write_str(out);

    k_mutex_unlock(&load_mutex);
}


static module_slot_t *slot_alloc(const char *module_id)
{
    for (int i = 0; i < MAX_MODULES; i++) {
        module_slot_t *slot = &g_mods[i];
        if (!slot->used) {
            memset(slot, 0, sizeof(*slot));
            slot->used = true;
            strncpy(slot->module_id, module_id, sizeof(slot->module_id) - 1);

            k_sem_init(&slot->work_sem, 0, 1);


            slot_ensure_worker(slot);
            k_work_init_delayable(&slot->stop_dwork, stop_dwork_handler);
            slot->terminate_requested = false;
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

        if (!slot->exec_env) {
            /* safety: se manca l'exec_env, prova a crearlo (può fallire per OOM) */
            slot->exec_env = wasm_runtime_create_exec_env(slot->inst, CONFIG_APP_STACK_SIZE);
            if (!slot->exec_env) {
                mem_alloc_info_t mi;
                if (wasm_runtime_get_mem_alloc_info(&mi)) {
                    char out[128];
                    snprintf(out, sizeof(out),
                            "RESULT status=NO_EXEC_ENV msg=\"free=%u\"\n",
                            mi.total_free_size);
                    agent_write_str(out);
                } else {
                    agent_write_str("RESULT status=NO_EXEC_ENV\n");
                }

                slot->busy = false;
                slot->stop_requested = false;
                slot->state = MOD_LOADED;
                continue;
            }
        }


        uint32 argv_local[MAX_CALL_ARGS];
        uint32 argc = req.argc;
        for (uint32 i = 0; i < argc && i < MAX_CALL_ARGS; i++) {
            argv_local[i] = req.argv[i];
        }

        slot->state = MOD_RUNNING;
        wasm_runtime_clear_exception(slot->inst);
        bool ok = wasm_runtime_call_wasm(slot->exec_env, fn, argc, argv_local);

        k_work_cancel_delayable_sync(&slot->stop_dwork, &slot->stop_sync);
        slot->terminate_requested = false;

        const char *exc = NULL;
        if (!ok) {
            exc = wasm_runtime_get_exception(slot->inst);
        }

        char out[256];
        bool stopped = false;

        if (!ok) {
            /* se STOP ha chiamato wasm_runtime_terminate(), WAMR tipicamente setta
            un'eccezione tipo "terminated by user" */
            if (exc && strstr(exc, "terminated") != NULL) {
                stopped = true;
            }

            if (stopped) {
                snprintf(out, sizeof(out),
                        "RESULT status=STOPPED module_id=%s func=%s msg=\"%s\"\n",
                        slot->module_id, req.func_name, exc);
                /* important: lascia l'istanza pulita per future START */
                wasm_runtime_clear_exception(slot->inst);
            } else {
                snprintf(out, sizeof(out),
                        "RESULT status=EXCEPTION module_id=%s func=%s msg=\"%s\"\n",
                        slot->module_id, req.func_name, exc ? exc : "<none>");
            }
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
        
        wasm_runtime_clear_exception(slot->inst);
        slot->busy = false;
        slot->stop_requested = false;
        slot->state = slot->inst ? MOD_LOADED : MOD_EMPTY;
    }

    wasm_runtime_destroy_thread_env();
}

/* ------------------------ Command handlers ------------------------ */

static void handle_load_cmd(const char *line)
{
    k_mutex_lock(&load_mutex, K_FOREVER);

    bool warn_ignored_victim = false;
    char size_str[16];
    char crc_str[16];
    char module_id_buf[32];
    char victim_id_buf[32] = {0};
    char out_buf[200];

    bool do_replace = false;
    bool have_victim = false;

    const char *p_mod    = find_param(line, "module_id");
    const char *p_size   = find_param(line, "size");
    const char *p_crc    = find_param(line, "crc32");
    const char *p_rep    = find_param(line, "replace");
    const char *p_victim = find_param(line, "replace_victim");

    if (!p_mod || !p_size || !p_crc) {
        agent_write_str("LOAD_ERR code=BAD_PARAMS msg=\"missing module_id/size/crc32\"\n");
        goto out;
    }

    copy_param_value(p_mod,  module_id_buf, sizeof(module_id_buf));
    copy_param_value(p_size, size_str,     sizeof(size_str));
    copy_param_value(p_crc,  crc_str,      sizeof(crc_str));

    if (p_rep) {
        char rep_str[8];
        copy_param_value(p_rep, rep_str, sizeof(rep_str));
        do_replace = (rep_str[0] == '1'); /* semplice: 1 abilita */
    }

    if (p_victim) {
        copy_param_value(p_victim, victim_id_buf, sizeof(victim_id_buf));
        have_victim = (victim_id_buf[0] != '\0');
    }

    uint32_t size = (uint32_t)atoi(size_str);
    if (size == 0) {
        agent_write_str("LOAD_ERR code=BAD_PARAMS msg=\"size=0\"\n");
        goto out;
    }

    uint32_t crc_expected = (uint32_t)strtoul(crc_str, NULL, 16);

    /* Admission control pool (come già fai) ... */

    module_slot_t *slot = slot_find(module_id_buf);

    if (slot) {
        if (have_victim) {
            /* module_id esiste già -> replace in-place, quindi replace_victim non serve */
            warn_ignored_victim = true;
        }
        /* Replace in-place di module_id esistente */
        if (slot->busy) {
            if (!do_replace) {
                agent_write_str("LOAD_ERR code=BUSY msg=\"module running\"\n");
                goto out;
            }
            /* stop forzato */
            slot_abort_worker(slot);
            slot->busy = false;
            slot->stop_requested = false;
            slot_ensure_worker(slot);
        }

        slot_cleanup(slot);
        /* module_id già corretto */
    } else {
        /* module_id nuovo */
        slot = slot_alloc(module_id_buf);

        if (!slot) {
            if (do_replace) {
                if (!have_victim) {
                    agent_write_str("LOAD_ERR code=FULL msg=\"NEED_VICTIM\"\n");
                    goto out;
                }

                module_slot_t *victim = slot_find(victim_id_buf);
                if (!victim) {
                    agent_write_str("LOAD_ERR code=BAD_VICTIM msg=\"NOT_FOUND\"\n");
                    goto out;
                }

                if (victim->busy) {
                    slot_abort_worker(victim);
                    victim->busy = false;
                    victim->stop_requested = false;
                    slot_ensure_worker(victim);
                }

                slot_cleanup(victim);

                /* riuso lo slot del victim per il nuovo module_id */
                strncpy(victim->module_id, module_id_buf, sizeof(victim->module_id) - 1);
                victim->module_id[sizeof(victim->module_id) - 1] = '\0';
                slot = victim;
            } else {
                agent_write_str("LOAD_ERR code=NO_SLOT msg=\"MAX_MODULES reached\"\n");
                goto out;
            }
        }
    }

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

    /* Crea exec_env persistente per lo slot (riuso tra le chiamate) */
    slot->exec_env = wasm_runtime_create_exec_env(slot->inst, CONFIG_APP_STACK_SIZE);
    if (!slot->exec_env) {
        snprintf(out_buf, sizeof(out_buf),
                 "LOAD_ERR code=NO_EXEC_ENV msg=\"create_exec_env failed\"\n");
        agent_write_str(out_buf);
        slot_cleanup(slot);
        goto out;
    }


    slot->state = MOD_LOADED;
    if (warn_ignored_victim) {
        snprintf(out_buf, sizeof(out_buf),
                "LOAD_OK warn=VICTIM_IGNORED replace_victim=%s\n",
                victim_id_buf);
        agent_write_str(out_buf);
    } else {
        agent_write_str("LOAD_OK\n");
    }

out:
    k_mutex_unlock(&load_mutex);
}

static void handle_start_cmd(const char *line)
{
    char func_name[64] = {0};
    char args_buf[64];
    char module_id_buf[32];

    uint32_t argc_local = 0;
    uint32_t argv_local[MAX_CALL_ARGS] = {0};

    const char *p_mod  = find_param(line, "module_id");
    const char *p_func = find_param(line, "func");
    if (!p_mod) {
        agent_write_str("RESULT status=BAD_PARAMS msg=\"missing module_id\"\n");
        return;
    }

    copy_param_value(p_mod, module_id_buf, sizeof(module_id_buf));
    if (p_func) {
        copy_param_value(p_func, func_name, sizeof(func_name));
        func_name[sizeof(func_name) - 1] = '\0';
    }

    module_slot_t *slot = slot_find(module_id_buf);
    if (!slot || !slot->inst) {
        agent_write_str("RESULT status=NO_MODULE\n");
        return;
    }

    /* Admission control: soglia diversa se devo creare exec_env */
    mem_alloc_info_t mi;
    if (wasm_runtime_get_mem_alloc_info(&mi)) {
        uint32_t guard = slot->exec_env ? START_GUARD_BYTES_HAVE_EXEC_ENV
                                        : START_GUARD_BYTES_NEED_EXEC_ENV;

        if (mi.total_free_size < guard) {
            char out[160];
            snprintf(out, sizeof(out),
                     "RESULT status=NO_MEM msg=\"free=%u need>=%u exec_env=%s\"\n",
                     mi.total_free_size, guard, slot->exec_env ? "yes" : "no");
            agent_write_str(out);
            return;
        }
    }


    if (slot->busy) {
        agent_write_str("RESULT status=BUSY\n");
        return;
    }

    /* 1) Parse args -> argc_local/argv_local */
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
            while (tok && argc_local < MAX_CALL_ARGS) {
                char *eq = strchr(tok, '=');
                if (eq) {
                    int val = atoi(eq + 1);
                    argv_local[argc_local++] = (uint32_t)val;
                }
                tok = strtok(NULL, ",");
            }
        }
    }

    /* 2) Default entrypoint */
    if (func_name[0] == '\0') {
        if (!wasm_runtime_lookup_function(slot->inst, "app_main")) {
            if (argc_local > 0) {
                agent_write_str("RESULT status=NO_ENTRYPOINT msg=\"args require app_main\"\n");
            } else {
                agent_write_str("RESULT status=NO_ENTRYPOINT msg=\"expected app_main\"\n");
            }
            return;
        }
        strncpy(func_name, "app_main", sizeof(func_name) - 1);
        func_name[sizeof(func_name) - 1] = '\0';
    }

    /* 3) Fill request */
    memset(&slot->req, 0, sizeof(slot->req));
    strncpy(slot->req.func_name, func_name, sizeof(slot->req.func_name) - 1);
    slot->req.func_name[sizeof(slot->req.func_name) - 1] = '\0'; 
    slot->req.argc = argc_local;
    for (uint32_t i = 0; i < argc_local; i++) {
        slot->req.argv[i] = argv_local[i];
    }

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

    slot->terminate_requested = true;
    /* prova soft-stop */
    wasm_runtime_terminate(slot->inst);
    /* se entro STOP_FORCE_DELAY_MS non arriva RESULT dal worker, scatta escalation */
    k_work_reschedule(&slot->stop_dwork, K_MSEC(STOP_FORCE_DELAY_MS));
    agent_write_str("STOP_OK status=PENDING\n");
}

static void handle_status_cmd(const char *line)
{
    ARG_UNUSED(line);

    char out[512];
    char mods[256] = {0};
    char low[128]  = {0};

    for (int i = 0; i < MAX_MODULES; i++) {
        module_slot_t *s = &g_mods[i];
        if (!s->used || !s->inst) continue;

        const char *st = (s->state == MOD_RUNNING) ? "RUNNING" : "LOADED";

        char one[128];
        size_t free_stack = 0;
        (void)k_thread_stack_space_get(s->tid, &free_stack);

        snprintf(one, sizeof(one),
                 "%s:%s:wasm=%lu:stack_free=%zu",
                 s->module_id, st,
                 (unsigned long)s->wasm_size,
                 free_stack);

        if (mods[0]) strncat(mods, ",", sizeof(mods) - strlen(mods) - 1);
        strncat(mods, one, sizeof(mods) - strlen(mods) - 1);

        if (free_stack < 512) {
            if (low[0]) strncat(low, ",", sizeof(low) - strlen(low) - 1);
            strncat(low, s->module_id, sizeof(low) - strlen(low) - 1);
        }
    }

    if (!mods[0]) strcpy(mods, "none");
    if (!low[0])  strcpy(low, "none");

    mem_alloc_info_t mi;
    if (wasm_runtime_get_mem_alloc_info(&mi)) {
        uint32_t used = mi.total_size - mi.total_free_size;
        snprintf(out, sizeof(out),
                 "STATUS_OK modules=\"%s\" low_stack=\"%s\" "
                 "wamr_total=%u wamr_free=%u wamr_used=%u wamr_highmark=%u\n",
                 mods, low,
                 mi.total_size,
                 mi.total_free_size,
                 used,
                 mi.highmark_size);
    } else {
        snprintf(out, sizeof(out),
                 "STATUS_OK modules=\"%s\" low_stack=\"%s\" wamr_heap=NA\n",
                 mods, low);
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
