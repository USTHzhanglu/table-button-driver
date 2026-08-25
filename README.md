[English](README_EN.md) | 简体中文

# button_driver

通用嵌入式按键驱动组件：表驱动注册式状态机，支持单击/多击/长按/超长按、组合键与多键抑制。读取源由应用层注册，零硬件依赖、零动态内存。

## 特性

- **表驱动**：一张配置表定义全部按键（事件掩码 + 物理绑定 + 触发时间 + 组合组 + 回调），`btn_create()` 一步完成初始化与注册。
- **事件**：按下 / 释放 / 多击（次数任意，`multi_max` 控上限）/ 长按 / 超长按。
- **长按双语义**：按住触发（如电机）与松开确认（如电源键）互斥校验，超长按档位优先。
- **组合键与多键抑制**：绑定条件可跨 GPIO/ADC/CUSTOM 源按 AND 组合；同组多键同时按压时整周期作废，组合键豁免。
- **零硬件依赖**：电平/ADC 读取由应用层实现并经 `btn_source_register()` 注册。
- **零动态内存**：实例由使用方静态持有，挂链表统一扫描；纯 C，无 RTOS/芯片依赖。

## 快速上手

```c
#include "button_driver.h"

/* 1. 应用层实现读取源: 只返回原始值(电平 0/1), 判定由组件完成 */
static int32_t my_gpio_read(uint8_t key_id, void* ctx) {
    /* 按 key_id 映射到具体 GPIO 引脚 */
    return (int32_t)gpio_get_level(key_id_to_pin(key_id)); /* 示意 */
}

/* 2. 事件回调: 键级自有回调 → 全局默认回调, 两级回退 */
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

/* 3. 表驱动配置: 一行 = 一个逻辑键 */
static btn_t btn_ok, btn_pwr;

static void button_init(void) {
    static const btn_source_t gpio_src = { .read = my_gpio_read, .ctx = NULL };
    btn_source_register(BTN_SRC_GPIO, &gpio_src);

    static const btn_cfg_t cfgs[] = {
        {   /* 确认键: 单击/双击/三击 + 长按(按住触发) */
            .id = 1,
            .events = BTN_EVT_MASK(BTN_EVT_MULTI_CLICK) | BTN_EVT_MASK(BTN_EVT_LONG_PRESS),
            .bind = { .count = 1, .items = { { .type = BTN_SRC_GPIO, .id = 0, .active_level = 0 } } },
            .timing = { .debounce_ms = 20, .click_interval_ms = 300, .long_ms = 1000, .multi_max = 3 },
            .cb = on_btn,
        },
        {   /* 电源键: 长按 2s 松开确认(与按住触发互斥) */
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

/* 4. 扫描: 由上层周期调用, now_ms 为单调递增毫秒时间戳 */
void main_loop(void) {
    uint32_t now_ms = tick_get_ms(); /* 示意 */
    btn_scan(now_ms);
    /* ... 业务逻辑 ... */
}
```

## API 一览

| 函数 | 说明 |
|---|---|
| `btn_source_register(type, src)` | 注册读取源（GPIO/ADC/CUSTOM），同类型重复注册覆盖 |
| `btn_set_default_cb(cb, ud)` | 设置全局默认回调（键级回调缺失时回退） |
| `btn_create(btn, cfg)` | 初始化并注册，返回 0 成功 / -1 重复·id 冲突 / -2 参数无效 / -3 长按语义冲突 |
| `btn_register(btn)` / `btn_unregister(btn)` | 注册 / 摘除实例 |
| `btn_scan(now_ms)` | 扫描全部已注册按键 |
| `btn_attach(btn, evt, cb, ud)` | 运行时为具体事件注册回调 |
| `btn_clicks(btn)` | 多击次数（MULTI_CLICK 回调中有效） |
| `btn_press_time_ms(btn)` | 按住时长（ms），可做更细档位区分 |
| `btn_pressed(btn)` | 当前是否按下（轮询用） |

## 集成

纯 C 实现，零平台依赖：直接拷贝 `button_driver.c/h` 到任意工程编译即可，无 RTOS/芯片限制；驱动不感知硬件，电平/ADC 读取由应用层源回调提供。

## 致谢

本组件基于 deepseek-v4-flash 与 codewhale 实现。

## License

基于 [MIT](LICENSE) 协议开源。
