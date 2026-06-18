#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "driver/uart.h"
#include "driver/gpio.h"

#define TAG "RECEIVER"

// 定义串口透传的引脚和配置
#define UART_PORT_NUM      UART_NUM_1
#define UART_TX_PIN        17         // 连接到 STM32 的 RX
#define UART_RX_PIN        16         // 连接到 STM32 的 TX (本工程暂不接收，但备用)
#define UART_BAUD_RATE     115200

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

// 大端序转小端序 (ESP32 是小端序)
static inline int16_t be16_to_i16(const uint8_t *p) {
    return (int16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

// 接收回调函数
static void espnow_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len) {
    if (len == 18 && data[0] == 0xAA && data[17] == 0x55) {
        remote_data_t *remote = (remote_data_t *)data;
        
        int16_t left_x  = be16_to_i16(remote->rocker_l_x);
        int16_t left_y  = be16_to_i16(remote->rocker_l_y);
        int16_t right_x = be16_to_i16(remote->rocker_r_x);
        int16_t right_y = be16_to_i16(remote->rocker_r_y);
        int16_t dial    = be16_to_i16(remote->dial);
        
        // 打印解析出来的数据
       // ESP_LOGI(TAG, "L_JOY:(%5d, %5d) | R_JOY:(%5d, %5d) | DIAL:%5d | SW:(L:%d R:%d)", 
                // left_x, left_y, right_x, right_y, dial, remote->switch_left, remote->switch_right);
                 
        // [新增] 将完整原始的 18 字节通过串口转发给 STM32
        uart_write_bytes(UART_PORT_NUM, data, len);
    } else {
        ESP_LOGW(TAG, "Unknown packet - Length: %d, Header: 0x%02X, Footer: 0x%02X", len, data[0], data[len-1]);
    }
}

// 串口透传初始化
static void uart_transparent_init(void) {
    uart_config_t uart_config = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    // 安装 UART 驱动，分配 TX 缓冲区以防止在 ESP-NOW 回调中阻塞
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT_NUM, 1024, 1024, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT_NUM, &uart_config));
    
    // 设置通信引脚
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT_NUM, UART_TX_PIN, UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_LOGI(TAG, "UART Initialization Done! TX:%d RX:%d at %d bps", UART_TX_PIN, UART_RX_PIN, UART_BAUD_RATE);
}

// WiFi 初始化
static void wifi_init(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    // [新增] 直接强制指定一个固定的 MAC 地址 (可自行修改)
    uint8_t custom_mac[6] = {0x1A, 0x2B, 0x3C, 0x4D, 0x5E, 0x6F};
    ESP_ERROR_CHECK(esp_wifi_set_mac(WIFI_IF_STA, custom_mac));

    ESP_ERROR_CHECK(esp_wifi_start());
    
    // 强制锁定 Channel 为 3，与发送端对齐
    ESP_ERROR_CHECK(esp_wifi_set_channel(3, WIFI_SECOND_CHAN_NONE));
}

void app_main(void) {
    // 1. 初始化 NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. 初始化 WiFi 并锁定信道
    wifi_init();

    // 3. 初始化用作透传的串口 (TX: 引脚17, RX: 引脚16)
    uart_transparent_init();

    // 4. 读取并打印当前正在使用的 MAC 地址
    uint8_t mac_addr[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, mac_addr);
    ESP_LOGI(TAG, "===================================================");
    ESP_LOGI(TAG, ">>> Receiver MAC Address: %02x:%02x:%02x:%02x:%02x:%02x <<<",
             mac_addr[0], mac_addr[1], mac_addr[2], 
             mac_addr[3], mac_addr[4], mac_addr[5]);
    ESP_LOGI(TAG, "===================================================");

    // 4. 初始化 ESP-NOW
    ESP_ERROR_CHECK(esp_now_init());
    
    // 5. 注册接收回调函数
    ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_recv_cb));
    
    ESP_LOGI(TAG, "ESP-NOW Receiver started. Listening on Channel 3...");
}
