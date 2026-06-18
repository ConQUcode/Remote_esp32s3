# ESP-NOW 遥控器接收与串口透传工程
RoboCon遥控器连接
Stm32数据采集 https://gitee.com/syy6688/cqu-remote-control.git
ESP32C5 接收Stm32 通过espnow发送数据 https://github.com/ConQUcode/Remote_espc5.git
ESP32S3 接收esp32c5 数据 通过串口发送数据到主控 https://github.com/ConQUcode/Remote_esp32s3.git
主控代码 https://github.com/ConQUcode/STM32H743VIT6.git


本工程运行在 ESP32-S3 开发板上，用作遥控器数据接收模块。ESP32-S3 通过 ESP-NOW 协议接收遥控器发送的 18 字节控制数据帧，完成基础帧校验后，将原始数据帧通过 UART1 发送给 H7 主控，由 H7 继续完成上层控制逻辑解析与执行。

## 工程功能

当前工程实现的主要功能如下：

1. 初始化 NVS、WiFi STA 模式和 ESP-NOW 接收功能。
2. 将 WiFi 信道固定为 Channel 3，与遥控器发送端保持一致。
3. 设置接收端 STA MAC 地址为固定值，便于发送端定向发送 ESP-NOW 数据。
4. 注册 ESP-NOW 接收回调函数，监听遥控器数据包。
5. 对收到的数据包做基础校验：长度必须为 18 字节，包头必须为 `0xAA`，包尾必须为 `0x55`。
6. 识别遥控器数据中的按键、摇杆、拨轮和左右开关字段。
7. 将校验通过的完整 18 字节原始数据通过 UART1 透传给 H7 主控。

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

ESP32-S3 在本工程中只承担无线接收和串口转发角色，不在本地执行运动控制。有效数据帧会按原始 18 字节顺序直接写入 UART，H7 端需要按照同一协议格式接收和解析。

## 遥控器数据帧格式

接收端当前只接受固定 18 字节遥控器数据帧：

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

ESP32-S3 是小端序 MCU，代码中提供了 `be16_to_i16()` 用于将摇杆和拨轮字段从大端序转换为 `int16_t`。当前版本主要用于接收端调试和字段识别，实际转发给 H7 的仍是原始 18 字节数据帧。

## 串口连接

ESP32-S3 通过 UART1 与 H7 主控连接：

| 配置项 | 当前值 |
| --- | --- |
| UART 外设 | `UART_NUM_1` |
| 波特率 | `115200` |
| 数据格式 | 8 数据位，无校验，1 停止位 |
| ESP32-S3 TX | GPIO17，连接到 H7 RX |
| ESP32-S3 RX | GPIO16，连接到 H7 TX，当前工程暂未使用 |

有效 ESP-NOW 数据包进入 `espnow_recv_cb()` 后，会调用：

```c
uart_write_bytes(UART_PORT_NUM, data, len);
```

因此 H7 端接收到的数据内容与遥控器发送端的 18 字节协议帧保持一致。

## ESP-NOW 接收配置

当前接收端 WiFi 和 ESP-NOW 配置要点：

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
pio run
pio run -t upload
pio device monitor -b 115200
```

如果使用 ESP-IDF 命令行，也可以根据本地 ESP-IDF 环境执行对应的 build、flash 和 monitor 流程。

## 启动日志

程序启动后会完成 NVS、WiFi、UART 和 ESP-NOW 初始化，并打印当前接收端 MAC 地址。正常情况下可看到类似信息：

```text
UART Initialization Done! TX:17 RX:16 at 115200 bps
===================================================
>>> Receiver MAC Address: 1a:2b:3c:4d:5e:6f <<<
===================================================
ESP-NOW Receiver started. Listening on Channel 3...
```

收到合法数据帧后，ESP32-S3 不额外封包，直接将 18 字节原始帧发送给 H7。收到长度、包头或包尾不匹配的数据时，会输出警告日志，提示未知数据包。

## 关键源码位置

- `main/receiver_main.c`：当前实际编译的接收端主程序，包含 WiFi、ESP-NOW、UART 初始化和接收回调。
- `main/CMakeLists.txt`：组件源文件注册，目前只编译 `receiver_main.c`。
- `platformio.ini`：PlatformIO 工程和 ESP32-S3 开发板配置。
- `RECEIVER_GUIDE.md`：早期接收端适配说明，可作为协议背景参考。

## 注意事项

1. 遥控器发送端和 ESP32-S3 接收端必须工作在同一 WiFi 信道，当前代码固定为 Channel 3。
2. 发送端目标 MAC 必须与接收端 STA MAC 一致，否则接收端不会收到定向 ESP-NOW 数据。
3. H7 串口接收配置需要与 ESP32-S3 UART1 保持一致：`115200 8N1`。
4. 当前工程只校验 18 字节长度、`0xAA` 包头和 `0x55` 包尾，没有额外 CRC 校验。
5. UART RX 引脚已经预留，但当前代码没有处理 H7 回传数据。
