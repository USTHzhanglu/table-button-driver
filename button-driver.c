/*
 * button_driver.c - generic table-driven button driver implementation
 *
 * State machine:
 *   IDLE --press debounce--> PRESS --release debounce--> RELEASE --window timeout--> MULTI_CLICK
 *                            |                          |--pressed in window--> PRESS (next click)
 *                            |--t>=long_ms--> LONG_HOLD --t>=super_ms--> SUPER_HOLD
 *   LONG_HOLD / SUPER_HOLD release --> IDLE (fires PRESS_UP, not part of multi-click)
 *
 * Multi-click: each press-release bumps click_count; the window timeout fires
 * MULTI_CLICK with the final count. Over multi_max voids the sequence; a long
 * press aborts it.
 *
 * Long-press semantics (exclusive per key):
 *   - Fire on hold (LONG_PRESS): once when held >= long_ms, no release needed.
 *   - Fire on release (LONG_PRESS_UP / SUPER_LONG_PRESS_UP): only on release
 *     after the threshold; tiers may coexist, super-long fires first.
 *   - Release always fires PRESS_UP (stop on release); thresholds count from
 *     press start.
 *
 * Callback fallback: per-event (attach) -> per-key (cfg.cb) -> global default.
 * Multi-key suppression: same-group simultaneous presses cancel the cycle;
 * combo keys (bind count > 1) are exempt.
 * Bind: pressed = all conditions hold (AND); unregistered source types count
 * as not pressed.
 */

#include "button_driver.h"

#include <string.h>

/* ==================== Source table / global default callback ==================== */

static btn_source_t s_sources[BTN_SRC_COUNT];
static btn_cb_t     s_default_cb;
static void*        s_default_cb_ud;

/* Judge one condition from the source's raw value */
static uint8_t item_level(const btn_bind_item_t* item) {
  if (item->type >= BTN_SRC_COUNT || !s_sources[item->type].read) {
    return 0; /* source not registered */
  }
  int32_t v = s_sources[item->type].read(item->id, s_sources[item->type].ctx);
  switch (item->type) {
    case BTN_SRC_GPIO:
      return (uint8_t)(v == item->active_level);
    case BTN_SRC_ADC:
      return (uint8_t)(v >= item->lo && v <= item->hi);
    default: /* BTN_SRC_CUSTOM */
      return (uint8_t)(v != 0);
  }
}

static uint8_t bind_active(const btn_bind_t* bind) {
  for (uint8_t i = 0; i < bind->count; i++) {
    if (!item_level(&bind->items[i])) {
      return 0;
    }
  }
  return 1;
}

/* ==================== Internals ==================== */

static btn_t* s_head = NULL;

/* Mark the cycle cancelled when another same-group key is pressed in it */
static void check_multi_hit(btn_t* btn) {
  if (btn->cfg.group_id == 0 || btn->multi_hit) {
    return;
  }
  for (btn_t* b = s_head; b; b = b->next) {
    if (b == btn) {
      continue;
    }
    if (b->cfg.group_id == btn->cfg.group_id && b->state != BTN_STATE_IDLE) {
      btn->multi_hit = 1;
      return;
    }
  }
}

/* Update last_evt and call the callback (runs in the btn_scan caller's thread) */
static void fire(btn_t* btn, btn_evt_t evt) {
  btn->last_evt = evt;
  /* suppression: same-group multi press cancels the cycle (combo keys exempt) */
  if (btn->multi_hit && btn->cfg.bind.count <= 1) {
    return;
  }
  btn_cb_t cb = btn->cb[evt];
  void*    ud = btn->cb_user[evt];
  if (!cb && btn->cfg.cb) {
    cb = btn->cfg.cb;
    ud = btn->cfg.cb_user;
  }
  if (!cb && s_default_cb) {
    cb = s_default_cb;
    ud = s_default_cb_ud;
  }
  if (cb) {
    cb(btn, evt, ud);
  }
}

/* ==================== State machine ==================== */

static void handle(btn_t* btn, uint32_t now_ms) {
  /* multi-hit flag: clear at IDLE, detect during a press cycle */
  if (btn->state == BTN_STATE_IDLE) {
    btn->multi_hit = 0;
  } else {
    check_multi_hit(btn);
  }

  /* hold time: live while held, keep the last value after release */
  if (btn->state != BTN_STATE_IDLE && btn->state != BTN_STATE_RELEASE) {
    btn->press_ms = now_ms - btn->press_start;
  }

  uint8_t active = bind_active(&btn->cfg.bind);
  btn->pressed   = active;

  switch (btn->state) {
    case BTN_STATE_IDLE:
      if (active) {
        /* level must hold debounce_ms */
        if (btn->debounce_t0 == 0) {
          btn->debounce_t0 = now_ms;
        } else if (now_ms - btn->debounce_t0 >= btn->cfg.timing.debounce_ms) {
          btn->debounce_t0 = 0;
          btn->press_start = now_ms;
          btn->t0          = now_ms;
          btn->state       = BTN_STATE_PRESS;
          fire(btn, BTN_EVT_PRESS_DOWN);
        }
      } else {
        btn->debounce_t0 = 0;
      }
      break;

    case BTN_STATE_PRESS:
      if (!active) {
        if (btn->debounce_t0 == 0) {
          btn->debounce_t0 = now_ms;
        } else if (now_ms - btn->debounce_t0 >= btn->cfg.timing.debounce_ms) {
          btn->debounce_t0 = 0;
          btn->click_count++;
          btn->t0    = now_ms;
          btn->state = BTN_STATE_RELEASE;
          /* PRESS_UP of multi-click sequences is deferred to the window
             timeout (gesture end) so intermediate releases of e.g. a double
             click do not fire; long/super-long paths fire it on physical
             release (stop-on-release needs immediacy) */
        }
      } else {
        btn->debounce_t0 = 0;
        if (now_ms - btn->press_start >= btn->cfg.timing.long_ms) {
          btn->click_count = 0; /* long press aborts the multi-click sequence */
          btn->state       = BTN_STATE_LONG_HOLD;
          if (btn->cfg.events & BTN_EVT_MASK(BTN_EVT_LONG_PRESS)) {
            fire(btn, BTN_EVT_LONG_PRESS); /* fire-on-hold only */
          }
        }
      }
      break;

    case BTN_STATE_RELEASE:
      if (active) {
        /* next click within the window (over multi_max voids the sequence) */
        btn->debounce_t0 = 0;
        btn->press_start = now_ms;
        btn->t0          = now_ms;
        btn->state       = BTN_STATE_PRESS;
        fire(btn, BTN_EVT_PRESS_DOWN);
      } else if (now_ms - btn->t0 >= btn->cfg.timing.click_interval_ms) {
        /* window timeout: decide multi-click, then PRESS_UP (gesture end) */
        if (!btn->cfg.timing.multi_max || btn->click_count <= btn->cfg.timing.multi_max) {
          fire(btn, BTN_EVT_MULTI_CLICK);
        }
        fire(btn, BTN_EVT_PRESS_UP);
        btn->click_count = 0;
        btn->state       = BTN_STATE_IDLE;
      }
      break;

    case BTN_STATE_LONG_HOLD:
      if (!active) {
        if (btn->debounce_t0 == 0) {
          btn->debounce_t0 = now_ms;
        } else if (now_ms - btn->debounce_t0 >= btn->cfg.timing.debounce_ms) {
          btn->debounce_t0 = 0;
          btn->state       = BTN_STATE_IDLE;
          /* fire-on-release: confirm the long tier */
          if (btn->cfg.events & BTN_EVT_MASK(BTN_EVT_LONG_PRESS_UP)) {
            fire(btn, BTN_EVT_LONG_PRESS_UP);
          }
          fire(btn, BTN_EVT_PRESS_UP); /* stop on release */
        }
      } else {
        btn->debounce_t0 = 0;
        /* total hold >= super_ms: enter SUPER_HOLD */
        if (btn->cfg.timing.super_ms && (now_ms - btn->press_start >= btn->cfg.timing.super_ms)) {
          btn->state = BTN_STATE_SUPER_HOLD;
        }
      }
      break;

    case BTN_STATE_SUPER_HOLD:
      if (!active) {
        if (btn->debounce_t0 == 0) {
          btn->debounce_t0 = now_ms;
        } else if (now_ms - btn->debounce_t0 >= btn->cfg.timing.debounce_ms) {
          btn->debounce_t0 = 0;
          btn->state       = BTN_STATE_IDLE;
          /* fire-on-release: longest tier first, fall back to the long tier */
          if (btn->cfg.events & BTN_EVT_MASK(BTN_EVT_SUPER_LONG_PRESS_UP)) {
            fire(btn, BTN_EVT_SUPER_LONG_PRESS_UP);
          } else if (btn->cfg.events & BTN_EVT_MASK(BTN_EVT_LONG_PRESS_UP)) {
            fire(btn, BTN_EVT_LONG_PRESS_UP);
          }
          fire(btn, BTN_EVT_PRESS_UP);
        }
      }
      break;

    default:
      btn->state = BTN_STATE_IDLE;
      break;
  }
}

/* ==================== Long-press semantic conflict check ==================== */

/* Fire-on-hold (LONG_PRESS) excludes any fire-on-release event; the two
 * fire-on-release tiers may coexist (super-long fires first) */
static uint8_t evt_conflict(btn_evt_mask_t mask) {
  if (mask & BTN_EVT_MASK(BTN_EVT_LONG_PRESS)) {
    if ((mask & BTN_EVT_MASK(BTN_EVT_LONG_PRESS_UP)) || (mask & BTN_EVT_MASK(BTN_EVT_SUPER_LONG_PRESS_UP))) {
      return 1;
    }
  }
  return 0;
}

/* ==================== API ==================== */

int btn_source_register(btn_src_t type, const btn_source_t* src) {
  if (type >= BTN_SRC_COUNT || !src || !src->read) {
    return -1;
  }
  s_sources[type] = *src;
  return 0;
}

void btn_set_default_cb(btn_cb_t cb, void* user_data) {
  s_default_cb    = cb;
  s_default_cb_ud = user_data;
}

int btn_create(btn_t* btn, const btn_cfg_t* cfg) {
  if (cfg && evt_conflict(cfg->events)) {
    return -3; /* long-press semantic conflict */
  }
  btn_init(btn, cfg);
  return btn_register(btn);
}

void btn_init(btn_t* btn, const btn_cfg_t* cfg) {
  if (!btn || !cfg) {
    return;
  }
  memset(btn, 0, sizeof(*btn));
  btn->cfg      = *cfg;
  btn->state    = BTN_STATE_IDLE;
  btn->last_evt = BTN_EVT_NONE;

  /* bind per-key callback to masked event slots (unmasked stay NULL) */
  for (uint8_t e = 1; e < BTN_EVT_COUNT; e++) { /* 0 is NONE, skip */
    if (cfg->events & BTN_EVT_MASK((btn_evt_t)e)) {
      btn->cb[e]      = cfg->cb;
      btn->cb_user[e] = cfg->cb_user;
    }
  }
}

int btn_register(btn_t* btn) {
  if (!btn) {
    return -2;
  }
  for (btn_t* b = s_head; b; b = b->next) {
    if (b == btn || b->cfg.id == btn->cfg.id) {
      return -1; /* duplicate register or id conflict */
    }
  }
  btn->next = s_head;
  s_head    = btn;
  return 0;
}

void btn_unregister(btn_t* btn) {
  if (!btn) {
    return;
  }
  btn_t** pp;
  for (pp = &s_head; *pp; pp = &(*pp)->next) {
    if (*pp == btn) {
      *pp       = btn->next;
      btn->next = NULL;
      return;
    }
  }
}

void btn_scan(uint32_t now_ms) {
  for (btn_t* b = s_head; b; b = b->next) {
    handle(b, now_ms);
  }
}

void btn_attach(btn_t* btn, btn_evt_t evt, btn_cb_t cb, void* user_data) {
  if (!btn || evt >= BTN_EVT_COUNT) {
    return;
  }
  /* long-press exclusivity: fire-on-hold vs fire-on-release; the two
   * fire-on-release tiers may coexist */
  if (evt == BTN_EVT_LONG_PRESS) {
    if ((btn->cfg.events & BTN_EVT_MASK(BTN_EVT_LONG_PRESS_UP)) || btn->cb[BTN_EVT_LONG_PRESS_UP] ||
        (btn->cfg.events & BTN_EVT_MASK(BTN_EVT_SUPER_LONG_PRESS_UP)) || btn->cb[BTN_EVT_SUPER_LONG_PRESS_UP]) {
      return; /* conflicts with fire-on-release */
    }
  } else if (evt == BTN_EVT_LONG_PRESS_UP || evt == BTN_EVT_SUPER_LONG_PRESS_UP) {
    if ((btn->cfg.events & BTN_EVT_MASK(BTN_EVT_LONG_PRESS)) || btn->cb[BTN_EVT_LONG_PRESS]) {
      return; /* conflicts with fire-on-hold */
    }
  }
  btn->cb[evt]      = cb;
  btn->cb_user[evt] = user_data;
}

btn_evt_t btn_get_event(const btn_t* btn) {
  return btn ? btn->last_evt : BTN_EVT_NONE;
}

uint8_t btn_clicks(const btn_t* btn) {
  return btn ? btn->click_count : 0;
}

uint32_t btn_press_time_ms(const btn_t* btn) {
  return btn ? btn->press_ms : 0;
}

uint8_t btn_pressed(const btn_t* btn) {
  return btn ? btn->pressed : 0;
}
