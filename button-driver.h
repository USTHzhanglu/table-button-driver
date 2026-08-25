/*
 * button_driver.h - generic table-driven button driver
 *
 * - Table-driven: one btn_cfg_t row per key (event mask, physical binding,
 *   timing, combo group, callback); btn_create() registers it.
 * - Callback fallback: per-event (btn_attach) -> per-key (cfg.cb) -> global
 *   default (btn_set_default_cb); only masked events fire callbacks.
 * - Multi-key suppression: simultaneous presses in the same group cancel the
 *   whole cycle; combo keys (bind count > 1) are exempt.
 * - Zero hardware dependency: level/ADC reads come from app-registered
 *   sources. Zero dynamic memory; pure C, no RTOS/chip dependency.
 */

#ifndef BUTTON_DRIVER_H
#define BUTTON_DRIVER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== Events ==================== */

/* Long-press semantics are exclusive per key (checked by btn_create/btn_attach):
 * fire-on-hold (LONG_PRESS) vs fire-on-release (LONG_PRESS_UP/SUPER_LONG_PRESS_UP,
 * which may coexist, super-long fires first). */
typedef enum {
  BTN_EVT_NONE = 0,
  BTN_EVT_PRESS_DOWN,  /* after debounce */
  BTN_EVT_PRESS_UP,    /* multi-click: fires with MULTI_CLICK; long/super-long: on release */
  BTN_EVT_MULTI_CLICK, /* count via btn_clicks(); 0 multi_max = no cap */
  BTN_EVT_LONG_PRESS,
  BTN_EVT_LONG_PRESS_UP,
  BTN_EVT_SUPER_LONG_PRESS_UP,
  BTN_EVT_COUNT,
} btn_evt_t;

typedef uint16_t btn_evt_mask_t;
#define BTN_EVT_MASK(evt) ((btn_evt_mask_t)(1u << (evt)))
#define BTN_EVT_MASK_NONE ((btn_evt_mask_t)0u)
#define BTN_EVT_MASK_ALL  ((btn_evt_mask_t)(((1u << BTN_EVT_COUNT) - 1) & 0xFFFFu))

/* ==================== Internal states (debug) ==================== */

typedef enum {
  BTN_STATE_IDLE = 0,
  BTN_STATE_PRESS,      /* debounced */
  BTN_STATE_RELEASE,    /* in multi-click window */
  BTN_STATE_LONG_HOLD,
  BTN_STATE_SUPER_HOLD,
} btn_state_t;

/* ==================== Bind condition source slots ==================== */

typedef enum {
  BTN_SRC_GPIO,
  BTN_SRC_ADC,
  BTN_SRC_CUSTOM,
  BTN_SRC_COUNT,
} btn_src_t;

#define BTN_MAX_BIND_ITEMS 4

/* ==================== Bind conditions ==================== */

/* One condition = source type + source-local key id + judgement parameters.
 * The source returns the raw value; the driver decides: GPIO raw ==
 * active_level, ADC lo <= raw <= hi, CUSTOM raw != 0. */
typedef struct {
  btn_src_t type;
  uint8_t   id;           /* key id within the source */
  uint8_t   active_level; /* GPIO: 0 low / 1 high */
  int32_t   lo;           /* ADC low bound */
  int32_t   hi;           /* ADC high bound */
} btn_bind_item_t;

/* AND of all conditions; count > 1 = combo key, exempt from suppression */
typedef struct {
  uint8_t         count;
  btn_bind_item_t items[BTN_MAX_BIND_ITEMS];
} btn_bind_t;

/* ==================== Read sources ==================== */

/* Implemented and registered by the app; one source may serve many conditions.
 * Returns the raw value; judgement is done by the driver. Unregistered types
 * are treated as not pressed. */
typedef int32_t (*btn_source_read_t)(uint8_t key_id, void* ctx);

typedef struct {
  btn_source_read_t read;
  void*             ctx; /* opaque context passed to read */
} btn_source_t;

/* ==================== Timing (per key, ms) ==================== */

typedef struct {
  uint16_t debounce_ms;
  uint16_t click_interval_ms; /* multi-click window */
  uint16_t long_ms;           /* threshold from press start */
  uint16_t super_ms;          /* total hold; 0 = disabled, must be > long_ms */
  uint8_t  multi_max;         /* max clicks per sequence; 0 = no cap */
} btn_timing_t;

/* ==================== Key config table (one row = one key) ==================== */

typedef struct button_driver btn_t;
typedef void (*btn_cb_t)(const btn_t* btn, btn_evt_t evt, void* user_data);

typedef struct {
  uint8_t        id;       /* unique, app-defined */
  btn_evt_mask_t events;   /* masked events fire callbacks */
  btn_bind_t     bind;
  btn_timing_t   timing;
  uint8_t        group_id; /* same-group presses cancel this cycle; 0 = off */
  btn_cb_t       cb;       /* NULL = global default */
  void*          cb_user;
} btn_cfg_t;

/* ==================== Button instance ==================== */

struct button_driver {
  btn_cfg_t cfg;
  /* runtime state; ignore below */
  btn_state_t state;
  btn_evt_t   last_evt;
  uint8_t     click_count;
  uint8_t     pressed;
  uint8_t     multi_hit;   /* same-group multi press this cycle */
  uint32_t    press_start;
  uint32_t    press_ms;    /* live while held, keeps last after release */
  uint32_t    t0;          /* state base time */
  uint32_t    debounce_t0; /* debounce start time */
  btn_cb_t    cb[BTN_EVT_COUNT];
  void*       cb_user[BTN_EVT_COUNT];
  btn_t*      next;
};

/* ==================== API ==================== */

/**
 * @brief Register a read source by type; re-registering the same type
 *        overrides the previous source.
 * @param type Source slot (BTN_SRC_GPIO / BTN_SRC_ADC / BTN_SRC_CUSTOM)
 * @param src  Source descriptor with a read callback and optional context
 * @return 0 on success; -1 if type is out of range or src/read is NULL
 */
int btn_source_register(btn_src_t type, const btn_source_t* src);

/* Global default callback for keys without one. Pass NULL to clear */
void btn_set_default_cb(btn_cb_t cb, void* user_data);

/**
 * @brief Initialize and register a button in one call.
 * @param btn Button instance (statically owned by the caller)
 * @param cfg Config row; copied into the instance, may point to a static table
 * @return 0 on success; -1 duplicate register or id conflict; -2 invalid arg;
 *         -3 long-press semantic conflict (fire-on-hold vs fire-on-release)
 */
int btn_create(btn_t* btn, const btn_cfg_t* cfg);

/* Initialize an instance (cfg is copied) */
void btn_init(btn_t* btn, const btn_cfg_t* cfg);

/**
 * @brief Register a button into the driver list for scanning.
 * @param btn Button instance
 * @return 0 on success; -1 duplicate register or id conflict; -2 invalid arg
 */
int btn_register(btn_t* btn);

/* Remove from the list */
void btn_unregister(btn_t* btn);

/* Scan all registered keys. now_ms is a monotonic ms timestamp */
void btn_scan(uint32_t now_ms);

/* Attach a callback to one event at runtime (overrides table config) */
void btn_attach(btn_t* btn, btn_evt_t evt, btn_cb_t cb, void* user_data);

/* Last event; inside a callback it is the current one */
btn_evt_t btn_get_event(const btn_t* btn);

/* Click count; valid inside the MULTI_CLICK callback */
uint8_t btn_clicks(const btn_t* btn);

/* Hold time in ms; live while held, last value after release */
uint32_t btn_press_time_ms(const btn_t* btn);

/* Whether currently pressed */
uint8_t btn_pressed(const btn_t* btn);

#ifdef __cplusplus
}
#endif

#endif /* BUTTON_DRIVER_H */
