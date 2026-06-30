# ESP-NOW 遥控器接收与串口透传工程
RoboCon遥控器连接
Stm32数据采集 https://gitee.com/syy6688/cqu-remote-control.git
ESP32C5 接收Stm32 通过espnow发送数据 https://github.com/ConQUcode/Remote_espc5.git
ESP32S3 接收esp32c5 数据 通过串口发送数据到主控 https://github.com/ConQUcode/Remote_esp32s3.git
主控代码 https://github.com/ConQUcode/STM32H743VIT6.git


本工程运行在 ESP32-S3 开发板上，用作遥控器数据接收/中转模块。ESP32-S3 在原 ESP-NOW 接收模式下接收发送端传来的遥控器 payload，完成基础校验后，将原始 payload 通过 UART1 发送给 H7 主控，由 H7 继续完成上层控制逻辑解析与执行。当前协议支持两类 ESP-NOW payload：18 字节遥控器主数据帧，以及 3 字节控制/重连命令 `AA A1 A2`。

## 工程功能

当前工程实现的主要功能如下：

1. 初始化 NVS、UART1，以及所选输入模式需要的 WiFi/ESP-NOW 或 WiFi AP + HTTP 服务。
2. 将 WiFi 信道固定为 Channel 3，与遥控器发送端保持一致。
3. 设置接收端 STA MAC 地址为固定值，便于发送端定向发送 ESP-NOW 数据。
4. 注册 ESP-NOW 接收回调函数，监听遥控器数据包。
5. 对收到的数据包做基础校验：18 字节主帧必须为 `0xAA ... 0x55`，3 字节控制帧必须为 `AA A1 A2`。
6. 识别遥控器数据中的按键、摇杆、拨轮和左右开关字段。
7. 将校验通过的原始 payload 按实际长度通过 UART1 透传给 H7 主控。

## 数据流

```text
遥控器发送端
    |
    | ESP-NOW, Channel 3
    v
ESP32-S3 接收端
    |
    | UART1, 115200 8N1
    v
H7 主控
```

ESP32-S3 在本工程中只承担输入接收和串口转发角色，不在本地执行运动控制。有效 payload 会按原始字节顺序直接写入 UART，H7 端需要按照同一协议格式接收和解析。

## 遥控器数据帧格式

原 ESP-NOW 接收/中转模式当前支持两类 payload：

| Payload 长度 | 内容 | 用途 | 透传行为 |
| ---: | --- | --- | --- |
| `18` | `AA ... 55` | 遥控器主数据帧 | 通过 UART1 原样写出 18 字节 |
| `3` | `AA A1 A2` | 控制/重连命令 | 通过 UART1 原样写出 3 字节 |

18 字节遥控器主数据帧格式如下：

| 字段 | 长度 | 说明 |
| --- | --- | --- |
| `header` | 1 字节 | 固定为 `0xAA` |
| `keys` | 4 字节 | 按键状态 |
| `rocker_l_x` | 2 字节 | 左摇杆 X，数据为大端序 |
| `rocker_l_y` | 2 字节 | 左摇杆 Y，数据为大端序 |
| `rocker_r_x` | 2 字节 | 右摇杆 X，数据为大端序 |
| `rocker_r_y` | 2 字节 | 右摇杆 Y，数据为大端序 |
| `dial` | 2 字节 | 拨轮数据，数据为大端序 |
| `switch_left` | 1 字节 | 左侧开关状态 |
| `switch_right` | 1 字节 | 右侧开关状态 |
| `footer` | 1 字节 | 固定为 `0x55` |

代码中对应结构体定义在 `main/receiver_main.c`：

```c
typedef struct {
    uint8_t header;
    uint8_t keys[4];
    uint8_t rocker_l_x[2];
    uint8_t rocker_l_y[2];
    uint8_t rocker_r_x[2];
    uint8_t rocker_r_y[2];
    uint8_t dial[2];
    uint8_t switch_left;
    uint8_t switch_right;
    uint8_t footer;
} __attribute__((packed)) remote_data_t;
```

ESP32-S3 是小端序 MCU，代码中提供了 `be16_to_i16()` 用于将摇杆和拨轮字段从大端序转换为 `int16_t`。当前版本主要用于接收端调试和字段识别，实际转发给 H7 的仍是原始 payload，不重新封包、不改字节序。

## 串口连接

ESP32-S3 通过 UART1 与 H7 主控连接：

| 配置项 | 当前值 |
| --- | --- |
| UART 外设 | `UART_NUM_1` |
| 波特率 | `115200` |
| 数据格式 | 8 数据位，无校验，1 停止位 |
| ESP32-S3 TX | GPIO17，连接到 H7 RX |
| ESP32-S3 RX | GPIO16，连接到 H7 TX，当前工程暂未使用 |

有效 payload 进入转发路径后，会调用：

```c
uart_write_bytes(UART_PORT_NUM, data, len);
```

因此 H7 端接收到的数据内容与发送端通过 ESP-NOW 发出的 payload 保持一致：主数据帧为 18 字节，控制/重连命令为 3 字节。

## ESP-NOW 中转模式配置

当 `REMOTE_INPUT_MODE_WIFI` 为 `0` 时，接收端 WiFi 和 ESP-NOW 配置要点如下：

- WiFi 工作模式：`WIFI_MODE_STA`
- 固定信道：Channel 3
- ESP-NOW 工作模式：只注册接收回调，不主动发送遥控数据
- 当前固定 STA MAC：`1a:2b:3c:4d:5e:6f`

固定 MAC 在 `main/receiver_main.c` 的 `wifi_init()` 中设置：

```c
uint8_t custom_mac[6] = {0x1A, 0x2B, 0x3C, 0x4D, 0x5E, 0x6F};
ESP_ERROR_CHECK(esp_wifi_set_mac(WIFI_IF_STA, custom_mac));
```

发送端需要将目标 MAC 配置为该地址，或根据实际需要同步修改发送端和接收端的 MAC 配置。

## 构建与下载

本工程使用 PlatformIO，目标板配置在 `platformio.ini` 中：

```ini
[env:esp32-s3-devkitc-1]
platform = espressif32
board = esp32-s3-devkitc-1
framework = espidf
monitor_speed = 115200
```

常用命令：

```bash
pio run -e esp32-s3-devkitc-1
pio run -e esp32-s3-devkitc-1 -t upload
pio device monitor -e esp32-s3-devkitc-1 -b 115200
```

如果使用 ESP-IDF 命令行，也可以根据本地 ESP-IDF 环境执行对应的 build、flash 和 monitor 流程。

当前通过 `main/receiver_main.c` 顶部的宏选择输入功能：

```c
// 0 = 原 ESP-NOW 接收/中转模式，透传 18 字节主帧和 3 字节控制帧
// 1 = 手机 WiFi 网页模拟遥控器模式，网页生成 18 字节主帧
#define REMOTE_INPUT_MODE_WIFI 0
```

| 宏值 | 功能 |
| --- | --- |
| `0` | 原 ESP-NOW 接收/中转功能，接收遥控器/ESP32C5 发来的 18 字节主帧和 3 字节控制帧并透传到 UART1 |
| `1` | 手机 WiFi 模拟遥控器功能，ESP32-S3 开热点和网页，手机网页生成同协议 18 字节帧并透传到 UART1 |

## 手机 WiFi 模拟遥控器模式

当暂时没有 ESP32C5 发送端、无法使用实体遥控器链路时，可以把 `REMOTE_INPUT_MODE_WIFI` 改为 `1` 后重新编译烧录。该模式不会改变 H7 侧协议：网页操作会在 ESP32-S3 内部生成同样的 18 字节遥控器数据帧，然后继续通过 UART1 原样发送给 H7。

手机使用方法：

1. 确认 `main/receiver_main.c` 中 `#define REMOTE_INPUT_MODE_WIFI 1`。
2. 编译并烧录 `esp32-s3-devkitc-1` 环境固件。
3. 手机连接 WiFi 热点 `ESP32S3-Remote`，密码 `12345678`。
4. 浏览器打开 `http://192.168.4.1/`。
5. 页面上的左右摇杆、拨轮、左右开关和 K0-K3 按键会映射到原 18 字节协议字段。

原 ESP-NOW 接收模式使用方法：

1. 把 `main/receiver_main.c` 中的宏改为 `#define REMOTE_INPUT_MODE_WIFI 0`。
2. 重新编译并烧录 `esp32-s3-devkitc-1` 环境固件。
3. 发送端发来的 `18` 字节主数据帧和 `AA A1 A2` 控制/重连命令都会按原始 payload 透传到 UART1。

生成帧格式保持不变：

```text
0      : 0xAA
1..4   : keys[0..3]
5..6   : left_x, big-endian int16
7..8   : left_y, big-endian int16
9..10  : right_x, big-endian int16
11..12 : right_y, big-endian int16
13..14 : dial, big-endian int16
15     : switch_left
16     : switch_right
17     : 0x55
```

切换模式只需要改 `REMOTE_INPUT_MODE_WIFI` 的值并重新烧录，不需要切换 PlatformIO 环境。

## 启动日志

当 `REMOTE_INPUT_MODE_WIFI` 为 `0` 时，程序启动后会完成 NVS、UART、WiFi STA 和 ESP-NOW 初始化，并打印当前接收端 MAC 地址。正常情况下可看到类似信息：

```text
UART Initialization Done! TX:17 RX:16 at 115200 bps
===================================================
>>> Receiver MAC Address: 1a:2b:3c:4d:5e:6f <<<
===================================================
ESP-NOW Receiver started. Listening on Channel 3...
```

收到合法数据帧后，ESP32-S3 不额外封包，直接将原始 payload 发送给 H7。收到长度、包头或包尾不匹配的数据时，会输出警告日志，提示未知数据包。

## 关键源码位置

- `main/receiver_main.c`：当前实际编译的接收端主程序，包含 WiFi、ESP-NOW、UART 初始化和接收回调。
- `main/CMakeLists.txt`：组件源文件注册，目前只编译 `receiver_main.c`，并声明 WiFi 网页模式需要的 `esp_http_server` 组件。
- `platformio.ini`：PlatformIO 工程和 ESP32-S3 开发板配置。
- `RECEIVER_GUIDE.md`：ESP32-S3 接收/中转端适配说明。
- `H7_UART_UNPACK_GUIDE.md`：H7 主控端 UART 接收与解包协议说明。

## 注意事项

1. 遥控器发送端和 ESP32-S3 接收端必须工作在同一 WiFi 信道，当前代码固定为 Channel 3。
2. 发送端目标 MAC 必须与接收端 STA MAC 一致，否则接收端不会收到定向 ESP-NOW 数据。
3. H7 串口接收配置需要与 ESP32-S3 UART1 保持一致：`115200 8N1`。
4. 当前工程只校验 18 字节主帧的 `0xAA` 包头和 `0x55` 包尾，以及 3 字节控制帧 `AA A1 A2`，没有额外 CRC 校验。
5. UART RX 引脚已经预留，但当前代码没有处理 H7 回传数据。
