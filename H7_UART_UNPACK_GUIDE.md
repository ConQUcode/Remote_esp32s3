# H7 串口接收与解包协议说明

本文档专门给 H7 主控端使用，用于说明从 ESP32-S3 接收端 UART1 收到的数据格式、解包方式和注意事项。

当前链路中，ESP32-S3 只做 ESP-NOW 接收和 UART 透传，不重新封包、不改变字段顺序、不改变大小端。因此 H7 端收到的 UART 字节流与发送端通过 ESP-NOW 发出的 payload 保持一致。

## 1. 串口连接与配置

ESP32-S3 接收端到 H7 主控的串口配置如下：

| 配置项 | 当前值 |
| --- | --- |
| ESP32-S3 UART | `UART_NUM_1` |
| ESP32-S3 TX | GPIO17，连接 H7 RX |
| ESP32-S3 RX | GPIO16，连接 H7 TX，当前工程暂未解析 H7 回传 |
| 波特率 | `115200` |
| 数据位 | 8 bit |
| 校验位 | None |
| 停止位 | 1 bit |
| 流控 | None |

H7 端串口应配置为：

```text
115200 8N1
```

## 2. H7 需要识别的两类数据

H7 从串口收到的是连续字节流，需要按帧头和长度进行拆包。当前协议有两类有效数据：

| 类型 | 长度 | 固定格式 | 用途 |
| --- | ---: | --- | --- |
| 遥控器主数据帧 | `18` 字节 | `AA ... 55` | 按键、摇杆、拨轮、左右开关 |
| 控制/重连命令 | `3` 字节 | `AA A1 A2` | 上位机新增控制或重连命令 |

注意：

- 18 字节主数据帧的第 0 字节固定为 `0xAA`，第 17 字节固定为 `0x55`。
- 3 字节控制帧完整内容就是 `AA A1 A2`，它没有 `0x55` 帧尾。
- 如果 H7 端只按 18 字节主帧解析，会丢弃 `AA A1 A2` 控制/重连命令。
- 当前协议没有 CRC，H7 端只能基于长度、帧头、帧尾或固定字节做基础校验。

## 3. 18 字节主数据帧格式

主数据帧按字节偏移定义如下：

| 偏移量 | 字段 | 长度 | 类型 | 说明 |
| ---: | --- | ---: | --- | --- |
| `0` | `header` | 1 | `uint8_t` | 固定 `0xAA` |
| `1` | `KEY[0]` | 1 | `uint8_t` | 按键状态原始字节 0 |
| `2` | `KEY[1]` | 1 | `uint8_t` | 按键状态原始字节 1 |
| `3` | `KEY[2]` | 1 | `uint8_t` | 按键状态原始字节 2 |
| `4` | `KEY[3]` | 1 | `uint8_t` | 按键状态原始字节 3 |
| `5~6` | `rocker_l_x` | 2 | `int16_t` | 左摇杆 X，大端序 |
| `7~8` | `rocker_l_y` | 2 | `int16_t` | 左摇杆 Y，大端序 |
| `9~10` | `rocker_r_x` | 2 | `int16_t` | 右摇杆 X，大端序 |
| `11~12` | `rocker_r_y` | 2 | `int16_t` | 右摇杆 Y，大端序 |
| `13~14` | `dial` | 2 | `int16_t` | 拨轮，大端序 |
| `15` | `switch_left` | 1 | `uint8_t` | 左侧开关状态 |
| `16` | `switch_right` | 1 | `uint8_t` | 右侧开关状态 |
| `17` | `footer` | 1 | `uint8_t` | 固定 `0x55` |

## 4. 大端序转换

主数据帧里的 16 位摇杆和拨轮字段都是大端序，即高字节在前、低字节在后。STM32H7 通常按小端序访问本地整数，因此不能直接把两个字节强转成 `int16_t *` 使用。

推荐在 H7 端使用如下转换函数：

```c
static inline int16_t remote_be16_to_i16(const uint8_t *p)
{
    return (int16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}
```

字段读取示例：

```c
int16_t left_x  = remote_be16_to_i16(&frame[5]);
int16_t left_y  = remote_be16_to_i16(&frame[7]);
int16_t right_x = remote_be16_to_i16(&frame[9]);
int16_t right_y = remote_be16_to_i16(&frame[11]);
int16_t dial    = remote_be16_to_i16(&frame[13]);
```

## 5. 推荐的 H7 结构体

为了避免大小端和结构体填充问题，H7 端建议先用原始字节结构体保存，再显式转换 16 位字段：

```c
#define REMOTE_MAIN_FRAME_LEN      18U
#define REMOTE_CONTROL_FRAME_LEN   3U
#define REMOTE_FRAME_HEADER        0xAAU
#define REMOTE_FRAME_FOOTER        0x55U
#define REMOTE_CONTROL_BYTE1       0xA1U
#define REMOTE_CONTROL_BYTE2       0xA2U

typedef struct __attribute__((packed)) {
    uint8_t header;
    uint8_t key[4];
    uint8_t rocker_l_x[2];
    uint8_t rocker_l_y[2];
    uint8_t rocker_r_x[2];
    uint8_t rocker_r_y[2];
    uint8_t dial[2];
    uint8_t switch_left;
    uint8_t switch_right;
    uint8_t footer;
} remote_raw_frame_t;

typedef struct {
    uint8_t key[4];
    int16_t left_x;
    int16_t left_y;
    int16_t right_x;
    int16_t right_y;
    int16_t dial;
    uint8_t switch_left;
    uint8_t switch_right;
} remote_decoded_t;
```

解码函数示例：

```c
static bool remote_decode_main_frame(const uint8_t *frame, uint16_t len, remote_decoded_t *out)
{
    if (frame == NULL || out == NULL) {
        return false;
    }

    if (len != REMOTE_MAIN_FRAME_LEN ||
        frame[0] != REMOTE_FRAME_HEADER ||
        frame[17] != REMOTE_FRAME_FOOTER) {
        return false;
    }

    out->key[0] = frame[1];
    out->key[1] = frame[2];
    out->key[2] = frame[3];
    out->key[3] = frame[4];
    out->left_x = remote_be16_to_i16(&frame[5]);
    out->left_y = remote_be16_to_i16(&frame[7]);
    out->right_x = remote_be16_to_i16(&frame[9]);
    out->right_y = remote_be16_to_i16(&frame[11]);
    out->dial = remote_be16_to_i16(&frame[13]);
    out->switch_left = frame[15];
    out->switch_right = frame[16];

    return true;
}
```

控制帧判断函数示例：

```c
static bool remote_is_control_frame(const uint8_t *frame, uint16_t len)
{
    return frame != NULL &&
           len == REMOTE_CONTROL_FRAME_LEN &&
           frame[0] == REMOTE_FRAME_HEADER &&
           frame[1] == REMOTE_CONTROL_BYTE1 &&
           frame[2] == REMOTE_CONTROL_BYTE2;
}
```

## 6. 串口字节流拆包建议

UART 接收是字节流，H7 端不应假设一次中断或一次 DMA 回调刚好等于一帧。推荐流程：

1. 将 UART 收到的新数据追加到环形缓冲区或线性缓存。
2. 从缓存中查找 `0xAA`。
3. 找到 `0xAA` 后，优先判断是否满足 3 字节控制帧 `AA A1 A2`。
4. 如果不是控制帧，再等待缓存至少有 18 字节。
5. 判断第 17 字节是否为 `0x55`。
6. 合法则解码 18 字节主帧，并从缓存移除已处理数据。
7. 不合法则丢弃当前 `0xAA`，继续向后寻找下一个 `0xAA`。

伪代码：

```c
while (rx_len >= 3) {
    find 0xAA in rx_buffer;
    discard bytes before 0xAA;

    if (rx_len >= 3 && rx_buffer[0] == 0xAA &&
        rx_buffer[1] == 0xA1 &&
        rx_buffer[2] == 0xA2) {
        handle_control_frame();
        remove 3 bytes from rx_buffer;
        continue;
    }

    if (rx_len < 18) {
        break;
    }

    if (rx_buffer[0] == 0xAA && rx_buffer[17] == 0x55) {
        remote_decoded_t remote;
        if (remote_decode_main_frame(rx_buffer, 18, &remote)) {
            handle_remote_frame(&remote);
        }
        remove 18 bytes from rx_buffer;
    } else {
        discard 1 byte;
    }
}
```

## 7. 关于 `0x55` ACK

发送端协议文档中提到的 1 字节 `0x55` ACK 是 STM32 遥控器端与 ESP32 发送端之间的 UART ACK。

该 ACK 不会通过 ESP-NOW 发给本工程，也不会由本工程转发给 H7。H7 端需要处理的 `0x55` 只有 18 字节主数据帧末尾的帧尾字节。

## 8. H7 侧最小检查清单

1. H7 串口配置为 `115200 8N1`。
2. H7 RX 接 ESP32-S3 GPIO17。
3. 接收缓存按字节流处理，不假设一次接收等于一帧。
4. 同时支持 18 字节 `AA ... 55` 和 3 字节 `AA A1 A2`。
5. 所有 16 位摇杆/拨轮字段按大端序转换。
6. `KEY[4]` 当前作为 4 字节原始按键状态透传，本工程没有定义逐位键位映射。
7. 当前没有 CRC，异常帧只能通过长度、帧头、帧尾和控制帧固定字节过滤。

