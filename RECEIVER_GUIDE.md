# 接收端 (Receiver) 开发与适配指南

本文档用于说明当前 `espnow` 工程的接收端中转逻辑。当前实际编译入口不是官方示例文件，而是：

- `main/receiver_main.c`
- `main/CMakeLists.txt`

`main/CMakeLists.txt` 目前只注册 `receiver_main.c`，因此 `main/espnow_example_main.c` 只是遗留参考文件，不能作为当前功能修改依据。

## 1. 当前工程角色

本工程运行在 ESP32-S3 上，负责把上位机/发送端通过 ESP-NOW 发来的遥控器 payload 转发给 H7 主控：

```text
遥控器 / STM32 采集端
    |
    | UART1, 115200 8N1
    v
ESP32-S3 发送端
    |
    | ESP-NOW, Channel 3
    v
ESP32-S3 接收端，本工程
    |
    | UART1, 115200 8N1
    v
H7 主控
```

ESP32-S3 接收端不重新封包、不改变字段顺序、不改变大小端，只在收到合法 payload 后通过 UART1 原样写出。

## 2. 输入模式宏

当前通过 `main/receiver_main.c` 顶部宏选择输入模式：

```c
// 0 = 原 ESP-NOW 接收/中转模式，透传 18 字节主帧和 3 字节控制帧
// 1 = 手机 WiFi 网页模拟遥控器模式，网页生成 18 字节主帧
#define REMOTE_INPUT_MODE_WIFI 0
```

| 宏值 | 功能 |
| --- | --- |
| `0` | 原 ESP-NOW 接收/中转模式，接收发送端 payload 并透传到 H7 |
| `1` | 手机 WiFi 网页模拟遥控器模式，ESP32-S3 开热点，网页生成 18 字节主帧并透传到 H7 |

如果要测试当前遥控器中转链路，需要把该宏改为 `0` 后重新编译烧录。

## 3. ESP-NOW 中转模式

ESP-NOW 中转模式的关键配置如下：

| 配置项 | 当前值 |
| --- | --- |
| WiFi 模式 | `WIFI_MODE_STA` |
| 固定信道 | Channel 3 |
| 固定 STA MAC | `1a:2b:3c:4d:5e:6f` |
| ESP-NOW | 只注册接收回调，不主动发送遥控数据 |
| UART 外设 | `UART_NUM_1` |
| UART 波特率 | `115200` |
| ESP32-S3 TX | GPIO17，连接 H7 RX |
| ESP32-S3 RX | GPIO16，连接 H7 TX，当前未主动解析 H7 回传 |

固定 MAC 在 `wifi_init()` 中设置：

```c
uint8_t custom_mac[6] = {0x1A, 0x2B, 0x3C, 0x4D, 0x5E, 0x6F};
ESP_ERROR_CHECK(esp_wifi_set_mac(WIFI_IF_STA, custom_mac));
```

发送端 `dest_mac` 必须与这里一致，发送端和接收端也必须都锁定在 Channel 3。

## 4. 当前 payload 协议

根据发送端 `DATA_PROTOCOL.md` 和 `main/softap_example_main.c`，接收端需要支持两类 ESP-NOW payload：

| Payload 长度 | 内容 | 用途 | 接收端行为 |
| ---: | --- | --- | --- |
| `18` | `AA ... 55` | 遥控器主数据帧 | 校验包头/包尾，通过 UART1 原样透传 18 字节 |
| `3` | `AA A1 A2` | 控制/重连命令 | 校验 3 个固定字节，通过 UART1 原样透传 3 字节 |

旧逻辑如果只写 `len == 18` 分支，会把 `AA A1 A2` 当作未知包丢弃，导致上位机新增控制/重连命令无法到达 H7。

## 5. 18 字节主数据帧

主数据帧结构如下：

| 偏移量 | 字段 | 长度 | 说明 |
| --- | --- | --- | --- |
| `0` | `header` | 1 字节 | 固定 `0xAA` |
| `1~4` | `keys[4]` | 4 字节 | 按键状态原始数据 |
| `5~6` | `rocker_l_x` | 2 字节 | 左摇杆 X，大端序 |
| `7~8` | `rocker_l_y` | 2 字节 | 左摇杆 Y，大端序 |
| `9~10` | `rocker_r_x` | 2 字节 | 右摇杆 X，大端序 |
| `11~12` | `rocker_r_y` | 2 字节 | 右摇杆 Y，大端序 |
| `13~14` | `dial` | 2 字节 | 拨轮，大端序 |
| `15` | `switch_left` | 1 字节 | 左侧开关状态 |
| `16` | `switch_right` | 1 字节 | 右侧开关状态 |
| `17` | `footer` | 1 字节 | 固定 `0x55` |

ESP32-S3 是小端序 MCU，调试解析时需要用 `be16_to_i16()` 把 16 位字段从大端序转换为本地 `int16_t`：

```c
static inline int16_t be16_to_i16(const uint8_t *p) {
    return (int16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}
```

这个转换只用于日志和调试识别。实际发给 H7 的仍是原始字节，不会改成小端序。

## 6. 当前接收回调处理

`espnow_recv_cb()` 当前按长度和固定字节区分 payload：

```c
if (is_valid_remote_frame(data, len)) {
    forward_remote_payload(data, len);
} else if (is_valid_control_frame(data, len)) {
    forward_remote_payload(data, len);
} else {
    ESP_LOGW(TAG, "Unknown packet ...");
}
```

`forward_remote_payload()` 会再次确认 payload 合法，然后调用：

```c
uart_write_bytes(UART_PORT_NUM, data, len);
```

因此 H7 端需要能同时识别：

- 18 字节 `AA ... 55` 遥控器主数据帧
- 3 字节 `AA A1 A2` 控制/重连命令

H7 主控端的具体串口解包说明见 `H7_UART_UNPACK_GUIDE.md`。

## 7. 手机 WiFi 模拟遥控器模式

当 `REMOTE_INPUT_MODE_WIFI` 为 `1` 时，ESP32-S3 开启热点和网页：

| 配置项 | 当前值 |
| --- | --- |
| 热点 SSID | `ESP32S3-Remote` |
| 热点密码 | `12345678` |
| 访问地址 | `http://192.168.4.1/` |

网页会生成同协议的 18 字节主数据帧，并通过 UART1 发给 H7。该模式用于临时没有实体遥控器/ESP-NOW 发送端时的手动调试，不会生成 `AA A1 A2` 控制帧。

## 8. 适配检查清单

1. 确认 `main/CMakeLists.txt` 仍然只编译 `receiver_main.c`。
2. 如需使用 ESP-NOW 中转链路，确认 `REMOTE_INPUT_MODE_WIFI` 为 `0`。
3. 确认发送端 `dest_mac` 等于接收端固定 STA MAC：`1a:2b:3c:4d:5e:6f`。
4. 确认发送端和接收端都使用 Channel 3。
5. 确认 H7 UART 配置为 `115200 8N1`，H7 RX 接 ESP32-S3 GPIO17。
6. 确认 H7 协议层同时支持 18 字节主帧和 3 字节 `AA A1 A2` 控制帧。
