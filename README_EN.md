English | [简体中文](README.md)

# button_driver

A generic embedded button driver: table-driven registered state machine with single/multi-click, long/super-long press, combo keys and multi-key suppression. Sources are registered by the app — zero hardware dependency, zero dynamic memory.

## Features

- **Table-driven**: one config table defines every button (event mask + physical binding + timing + combo group + callback); `btn_create()` initializes and registers in one call.
- **Events**: press-down, press-up, multi-click (any count, capped by `multi_max`), long press, super-long press.
- **Two long-press semantics**: fire-on-hold (e.g. motor control) and fire-on-release (e.g. power key) are mutually exclusive; the super-long tier takes priority.
- **Combo keys & multi-key suppression**: bind conditions combine across GPIO/ADC/CUSTOM sources (AND); simultaneous presses within the same group cancel the whole cycle, combo keys are exempt.
- **Hardware-agnostic**: level/ADC reads are implemented by the app and registered via `btn_source_register()`.
- **Zero dynamic memory**: instances are statically owned, linked and scanned together; pure C, no RTOS/chip dependency.

## Quick Start

```c
#include "button_driver.h"

/* 1. App implements the source: return raw values only, judgement is done by the driver */
static int32_t my_gpio_read(uint8_t key_id, void* ctx) {
    /* map key_id to the actual GPIO pin */
    return (int32_t)gpio_get_level(key_id_to_pin(key_id)); /* illustrative */
}

/* 2. Event callback: per-key callback falls back to the global default */
static void on_btn(const btn_t* btn, btn_evt_t evt, void* user_data) {
    switch (evt) {
        case BTN_EVT_MULTI_CLICK:
            printf("btn %d: %d clicks\n", btn->cfg.id, btn_clicks(btn));
            break;
        case BTN_EVT_LONG_PRESS:
            printf("btn %d: long press (%u ms)\n", btn->cfg.id, btn_press_time_ms(btn));
            break;
        case BTN_EVT_LONG_PRESS_UP:
            printf("btn %d: long press released, power off!\n", btn->cfg.id);
            break;
        default:
            break;
    }
}

/* 3. Table-driven config: one row = one logical key */
static btn_t btn_ok, btn_pwr;

static void button_init(void) {
    static const btn_source_t gpio_src = { .read = my_gpio_read, .ctx = NULL };
    btn_source_register(BTN_SRC_GPIO, &gpio_src);

    static const btn_cfg_t cfgs[] = {
        {   /* OK key: single/double/triple click + long press (fire-on-hold) */
            .id = 1,
            .events = BTN_EVT_MASK(BTN_EVT_MULTI_CLICK) | BTN_EVT_MASK(BTN_EVT_LONG_PRESS),
            .bind = { .count = 1, .items = { { .type = BTN_SRC_GPIO, .id = 0, .active_level = 0 } } },
            .timing = { .debounce_ms = 20, .click_interval_ms = 300, .long_ms = 1000, .multi_max = 3 },
            .cb = on_btn,
        },
        {   /* Power key: 2s long press, fire on release (exclusive with fire-on-hold) */
            .id = 2,
            .events = BTN_EVT_MASK(BTN_EVT_LONG_PRESS_UP),
            .bind = { .count = 1, .items = { { .type = BTN_SRC_GPIO, .id = 1, .active_level = 0 } } },
            .timing = { .debounce_ms = 20, .long_ms = 2000 },
            .cb = on_btn,
        },
    };
    btn_create(&btn_ok, &cfgs[0]);
    btn_create(&btn_pwr, &cfgs[1]);
}

/* 4. Scan periodically from the app; now_ms is a monotonically increasing timestamp */
void main_loop(void) {
    uint32_t now_ms = tick_get_ms(); /* illustrative */
    btn_scan(now_ms);
    /* ... app logic ... */
}
```

## API Overview

| Function | Description |
|---|---|
| `btn_source_register(type, src)` | Register a read source (GPIO/ADC/CUSTOM); re-registering the same type overrides |
| `btn_set_default_cb(cb, ud)` | Set the global default callback (used when a key has none) |
| `btn_create(btn, cfg)` | Initialize and register; returns 0 ok / -1 duplicate·id conflict / -2 invalid arg / -3 long-press semantic conflict |
| `btn_register(btn)` / `btn_unregister(btn)` | Register / remove an instance |
| `btn_scan(now_ms)` | Scan all registered buttons |
| `btn_attach(btn, evt, cb, ud)` | Attach a callback to a specific event at runtime |
| `btn_clicks(btn)` | Click count (valid inside the MULTI_CLICK callback) |
| `btn_press_time_ms(btn)` | Press duration (ms), usable for finer tiers |
| `btn_pressed(btn)` | Whether the button is currently pressed (polling) |

## Integration

Pure C with zero platform dependency: copy `button_driver.c/h` into any project and compile — no RTOS or chip restrictions. The driver is hardware-agnostic; level/ADC reads come from app-registered source callbacks.

## Acknowledgements

This component is implemented with deepseek-v4-flash and codewhale.

## License

MIT (LICENSE file to be added with repo initialization)
