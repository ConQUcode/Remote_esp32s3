#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <assert.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_now.h"
#include "esp_mac.h"
#include "esp_crc.h"
#include "espnow_example.h"

static const char *TAG = "espnow_recv";

// 接收回调函数：当 ESP-NOW 收到数据时触发
void recv_cb(const esp_now_recv_info_t *esp_now_info, const uint8_t *data, int data_len)
{
    // 根据协议文档的 18 字节与首尾要求校验
    if (data_len == 18 && data[0] == 0xAA && data[17] == 0x55) {
        
        // 解析数据帧载荷强制转换
        const remote_data_t *p_data = (const remote_data_t *)data;

        // 这里在回调里打印是为了快速验证，实际生产环境应该发入 Queue 到其他任务来处理
        ESP_LOGI(TAG, "从 MAC: " MACSTR " 接收到了数据", MAC2STR(esp_now_info->src_addr));
        ESP_LOGI(TAG, "左摇杆(X:%d, Y:%d) 右摇杆(X:%d, Y:%d) 拨轮:%d L_SW:%d R_SW:%d",
                 be16_to_i16((int16_t)p_data->rocker_l_),
                 be16_to_i16((int16_t)p_data->rocker_l1),
                 be16_to_i16((int16_t)p_data->rocker_r_),
                 be16_to_i16((int16_t)p_data->rocker_r1),
                 be16_to_i16((int16_t)p_data->dial),
                 p_data->switch_left,
                 p_data->switch_right);
    } else {
        ESP_LOGW(TAG, "收到无效的数据帧, 长度: %d, 或头尾不匹配.", data_len);
    }
}

void app_main(void)
{
    // 初始化 NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK( nvs_flash_erase() );
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK( ret );
    
    // 初始化网络，设置为 STA 模式
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    // 强制固定在信道 3
    ESP_ERROR_CHECK(esp_wifi_set_channel(3, WIFI_SECOND_CHAN_NONE));
    ESP_LOGI(TAG, "WiFi 初始化为 STA 模式完毕, 信道强制设为 3.");
    
    // 初始化 ESP-NOW
    if (esp_now_init() != ESP_OK) {
        ESP_LOGE(TAG, "Error initializing ESP-NOW");
        return;
    }
    ESP_LOGI(TAG, "ESP-NOW 初始化成功.");

    // 注册接收回调函数
    esp_now_register_recv_cb(recv_cb);

    // 获取并打印本接收端 MAC 地址
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    ESP_LOGI(TAG, ">>> 本模块(接收端)的 MAC 地址为: " MACSTR " <<<", MAC2STR(mac));
    ESP_LOGI(TAG, ">>> 请将此 MAC 地址填入发送端的 dest_mac 数组中! <<<");
    
    // 主循环保持运行并打印系统依然存活的心跳或者做其他上层任务
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        // 主循环每 5 秒做一次心跳或者可以在这里处理具体驱动
    }
}
