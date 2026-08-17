# BK7258 NuttX LED 内核状态逻辑

## 1. 控制对象

本项目仅保留 NuttX 内核自动 LED 控制，不提供应用层灯效和用户态 LED 设备。

| LED | GPIO | 用途 |
| --- | --- | --- |
| 红色 LED | GPIO40 | 断言和 Panic 状态 |
| 绿色 LED | GPIO41 | 内核运行、启动和中断状态 |

GPIO 输出为高电平时 LED 点亮，输出为低电平时 LED 熄灭。

## 2. 启用配置

```text
CONFIG_ARCH_HAVE_LEDS=y
CONFIG_ARCH_LEDS=y
# CONFIG_USERLED is not set
# CONFIG_USERLED_LOWER is not set
```

`CONFIG_ARCH_HAVE_LEDS` 表示板级提供 LED 能力，`CONFIG_ARCH_LEDS` 使能
NuttX 内核对 `board_autoled_on()` 和 `board_autoled_off()` 的调用。

当前没有 `/dev/userleds`，也没有 `led_app_set()`、watchdog 闪烁状态机或应用业务灯效。

## 3. 初始化

LED 初始化位于：

```text
board/beken/boards/bk7258/bk7258-ap/src/bk7258_boot.c
```

`board_early_initialize()` 调用：

```c
bk7258_led_initialize();
```

初始化函数位于：

```text
board/beken/boards/bk7258/bk7258-ap/src/bk7258_leds.c
```

初始化时将 GPIO40 和 GPIO41 配置为普通输出，并设置为低电平，避免 LED 在
NuttX 内核初始化前处于不确定状态。

## 4. 内核状态映射

### 绿色 LED GPIO41

以下状态会点亮绿灯：

```text
LED_INIRQ
LED_SIGNAL
LED_IDLE
LED_HEAPALLOCATE
LED_IRQSENABLED
LED_STACKCREATED
```

最主要的运行时状态是 `LED_INIRQ`：

```text
进入中断 -> board_autoled_on(LED_INIRQ)  -> 绿灯亮
退出中断 -> board_autoled_off(LED_INIRQ) -> 绿灯灭
```

因此 UART、SysTick、Mailbox、定时器等中断频繁发生时，绿灯会快速闪烁。
当闪烁频率较高时，肉眼可能观察为低亮度常亮或亮度变化，这不是 PWM。

### 红色 LED GPIO40

以下状态会点亮红灯：

```text
LED_ASSERTION
LED_PANIC
```

`LED_ASSERTION` 结束后可以关闭红灯。`LED_PANIC` 不响应关闭操作，红灯会保持
点亮直到系统复位，用于保留 Panic 状态。

## 5. 状态位图

为了避免多个内核状态相互覆盖，驱动分别维护两个状态位图：

```c
static volatile uint32_t g_green_states;
static volatile uint32_t g_red_states;
```

点亮某个状态时设置对应 bit，关闭某个状态时清除对应 bit。只有当对应位图
完全为零时，才会真正关闭 LED。

例如：

```text
LED_STACKCREATED on
LED_IRQSENABLED on
LED_IRQSENABLED off
```

此时 `LED_STACKCREATED` 仍然有效，所以绿灯继续保持点亮，不会被提前关闭。

## 6. 控制接口

NuttX 内核通过以下两个板级接口控制 LED：

```c
void board_autoled_on(int led);
void board_autoled_off(int led);
```

源码位置：

```text
board/beken/boards/bk7258/bk7258-ap/src/bk7258_leds.c
```

LED 状态编号在以下板级头文件中定义：

```text
board/beken/boards/bk7258/bk7258-ap/include/board.h
```
