# 接收端 (Receiver) 开发与适配指南

基于发送端协议文档 (`PROTOCOL.md`)，当前的官方 ESP-NOW 示例工程需要进行针对性修改才能正确接收和解析遥控器发送的数据。

## 1. 当前工程现状分析

当前的官方 `espnow` 工程**无法直接处理**遥控器发来的数据。原因如下：
1. **数据包格式不匹配**：官方示例使用的是自定义的 `example_espnow_data_t` 结构体，内部包含了大量的额外校验字段（CRC、通信状态、魔数 Magic 等），并期望在接收端验证这些字段。而遥控器发来的是非常精简的 18 字节控制指令帧。
2. **连接逻辑不同**：官方示例包含了一套通过广播互相发现并记忆 MAC 地址的握手流程；而本协议倾向于“发送端预先写入接收端 MAC 地址”的单向或点对点简单通信。
3. **信道设置**：官方示例的信道是由 Menuconfig 配置的，而遥控器强制要求必须在 **Channel 3**。

---

## 2. 接收端代码改造指南

为了使当前工程适配遥控器，需要在 `main/espnow_example_main.c` 中进行以下改造：

### 2.1 锁定物理信道 (Channel 3)
在 WiFi 初始化完成后（例如在 `example_wifi_init` 之后），强制将信道固定为 3，以匹配遥控器：
```c
// 强制设置 WiFi 工作在信道 3
ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
ESP_ERROR_CHECK(esp_wifi_set_channel(3, WIFI_SECOND_CHAN_NONE));
ESP_ERROR_CHECK(esp_wifi_set_promiscuous(false));
```

### 2.2 定义接收数据结构体
在代码中定义与 `PROTOCOL.md` 严格对应的结构体和解析函数，方便提取摇杆和按键数据。
```c
// 大端序转小端序 (ESP32 是小端序)
static inline int16_t be16_to_i16(const uint8_t *p) {
    return (int16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

// 遥控器数据帧结构 (18 Bytes)
typedef struct {
    uint8_t header;         // 固定 0xAA
    uint8_t keys[4];        // 4字节按键状态
    uint8_t rocker_l_x[2];  // 左摇杆X (大端)
    uint8_t rocker_l_y[2];  // 左摇杆Y (大端)
    uint8_t rocker_r_x[2];  // 右摇杆X (大端)
    uint8_t rocker_r_y[2];  // 右摇杆Y (大端)
    uint8_t dial[2];        // 拨轮 (大端)
    uint8_t switch_left;    // 左开关
    uint8_t switch_right;   // 右开关
    uint8_t footer;         // 固定 0x55
} __attribute__((packed)) remote_data_t;
```

### 2.3 重写 ESP-NOW 接收回调函数
将原本复杂的 `example_espnow_recv_cb` 替换为专门解析遥控器数据的精简版本：
```c
static void example_espnow_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len)
{
    // 判断长度是否为 18 字节，并且包头为 0xAA，包尾为 0x55
    if (len == 18 && data[0] == 0xAA && data[17] == 0x55) {
        remote_data_t *remote_data = (remote_data_t *)data;
        
        // 解析摇杆数据 (处理大小端转换)
        int16_t left_x  = be16_to_i16(remote_data->rocker_l_x);
        int16_t left_y  = be16_to_i16(remote_data->rocker_l_y);
        int16_t right_x = be16_to_i16(remote_data->rocker_r_x);
        int16_t right_y = be16_to_i16(remote_data->rocker_r_y);
        int16_t dial    = be16_to_i16(remote_data->dial);
        
        ESP_LOGI("RECV", "Left Rocker: X=%d, Y=%d | Switches: L=%d, R=%d", 
                 left_x, left_y, remote_data->switch_left, remote_data->switch_right);
                 
        // 在这里添加将解析后的数据传递给电机/飞控控制逻辑的代码
    } else {
        ESP_LOGW("RECV", "Received unknown packet, len: %d", len);
    }
}
```

### 2.4 获取并记录接收端的 MAC 地址
为了让发送端能够定向发送（填入 `dest_mac`），接收端必须知道自己的 MAC 地址。在 `app_main` 中读取并打印出来：
```c
uint8_t mac[6];
esp_read_mac(mac, ESP_MAC_WIFI_STA);
ESP_LOGI("MAC", "Receiver MAC Address: %02x:%02x:%02x:%02x:%02x:%02x", 
         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
```

---

## 3. 下一步建议工作

由于官方的 ESP-NOW 示例附带了大量不需要的广播配对逻辑，建议对 `espnow_example_main.c` 进行**大瘦身**。
您可以：
1. 删除 `example_espnow_send_cb`（接收端不需要发送回调）。
2. 删除 `example_espnow_task` 和相关的队列传输（接收控制信号通常需要低延迟，可以直接在接收回调中或者单独的专用高频控制任务中处理）。
3. 只保留 WiFi 初始化、ESP-NOW 初始化、以及注册接收回调函数的骨架。

