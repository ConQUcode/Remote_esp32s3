#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
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
#include "esp_http_server.h"

#define TAG "RECEIVER"

// 功能选择宏：
// 0 = 原 ESP-NOW 接收/中转模式，透传 18 字节主帧和 3 字节控制帧
// 1 = 手机 WiFi 网页模拟遥控器模式，网页生成 18 字节主帧
#define REMOTE_INPUT_MODE_WIFI 0

// 定义串口透传的引脚和配置
#define UART_PORT_NUM      UART_NUM_1
#define UART_TX_PIN        17         // 连接到 STM32 的 RX
#define UART_RX_PIN        16         // 连接到 STM32 的 TX (本工程暂不接收，但备用)
#define UART_BAUD_RATE     115200

#define REMOTE_FRAME_LEN    18
#define CONTROL_FRAME_LEN   3
#define REMOTE_FRAME_HEADER 0xAA
#define REMOTE_FRAME_FOOTER 0x55
#define CONTROL_FRAME_BYTE1 0xA1
#define CONTROL_FRAME_BYTE2 0xA2
#define WIFI_CHANNEL        3

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

static inline void i16_to_be(uint8_t *p, int16_t v) {
    p[0] = (uint8_t)(((uint16_t)v >> 8) & 0xFF);
    p[1] = (uint8_t)((uint16_t)v & 0xFF);
}

static bool is_valid_remote_frame(const uint8_t *data, int len) {
    return data != NULL &&
           len == REMOTE_FRAME_LEN &&
           data[0] == REMOTE_FRAME_HEADER &&
           data[REMOTE_FRAME_LEN - 1] == REMOTE_FRAME_FOOTER;
}

static bool is_valid_control_frame(const uint8_t *data, int len) {
    return data != NULL &&
           len == CONTROL_FRAME_LEN &&
           data[0] == REMOTE_FRAME_HEADER &&
           data[1] == CONTROL_FRAME_BYTE1 &&
           data[2] == CONTROL_FRAME_BYTE2;
}

static bool is_valid_passthrough_payload(const uint8_t *data, int len) {
    return is_valid_remote_frame(data, len) || is_valid_control_frame(data, len);
}

static void forward_remote_payload(const uint8_t *data, int len) {
    if (!is_valid_passthrough_payload(data, len)) {
        ESP_LOGW(TAG, "Drop invalid remote payload, len: %d", len);
        return;
    }

    int written = uart_write_bytes(UART_PORT_NUM, data, len);
    if (written != len) {
        ESP_LOGW(TAG, "UART forward incomplete, len:%d written:%d", len, written);
    }
}

#if !REMOTE_INPUT_MODE_WIFI
// 接收回调函数
static void espnow_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len) {
    (void)recv_info;

    if (is_valid_remote_frame(data, len)) {
        remote_data_t *remote = (remote_data_t *)data;
        
        int16_t left_x  = be16_to_i16(remote->rocker_l_x);
        int16_t left_y  = be16_to_i16(remote->rocker_l_y);
        int16_t right_x = be16_to_i16(remote->rocker_r_x);
        int16_t right_y = be16_to_i16(remote->rocker_r_y);
        int16_t dial    = be16_to_i16(remote->dial);
        
        ESP_LOGI(TAG,
                 "ESP-NOW RX remote: KEY=%02X%02X%02X%02X L(%d,%d) R(%d,%d) dial=%d SW(%u,%u)",
                 remote->keys[0], remote->keys[1], remote->keys[2], remote->keys[3],
                 (int)left_x, (int)left_y, (int)right_x, (int)right_y, (int)dial,
                 (unsigned int)remote->switch_left, (unsigned int)remote->switch_right);
                 
        forward_remote_payload(data, len);
    } else if (is_valid_control_frame(data, len)) {
        ESP_LOGI(TAG, "ESP-NOW RX control frame: AA A1 A2");
        forward_remote_payload(data, len);
    } else {
        uint8_t header = (data != NULL && len > 0) ? data[0] : 0;
        uint8_t footer = (data != NULL && len > 0) ? data[len - 1] : 0;
        ESP_LOGW(TAG, "Unknown packet - Length: %d, Header: 0x%02X, Footer: 0x%02X", len, header, footer);
    }
}
#endif

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

#if REMOTE_INPUT_MODE_WIFI
#define WIFI_REMOTE_AP_SSID      "ESP32S3-Remote"
#define WIFI_REMOTE_AP_PASSWORD  "12345678"
#define WIFI_REMOTE_MAX_STA_CONN 2

static const char wifi_remote_page[] =
"<!doctype html><html><head><meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no\">"
"<title>ESP32S3 Remote</title>"
"<style>"
"body{margin:0;font-family:Arial,sans-serif;background:#101820;color:#f4f7fb;touch-action:none}"
".wrap{max-width:720px;margin:0 auto;padding:16px}"
"h1{font-size:22px;margin:8px 0 14px}"
".grid{display:grid;grid-template-columns:1fr 1fr;gap:14px}"
".panel{background:#1d2a35;border:1px solid #314452;border-radius:8px;padding:12px}"
".joy{height:210px;border-radius:8px;background:#263846;position:relative;overflow:hidden;touch-action:none}"
".knob{width:72px;height:72px;border-radius:50%;background:#4fd1c5;position:absolute;left:50%;top:50%;transform:translate(-50%,-50%)}"
".row{display:flex;gap:10px;align-items:center;margin:10px 0;flex-wrap:wrap}"
"button{min-width:54px;height:42px;border:0;border-radius:6px;background:#334b5f;color:#fff;font-size:16px}"
"button.on{background:#e6a23c;color:#101820}"
"label{font-size:14px;color:#cbd5df}"
"input[type=range]{width:100%}"
".status{font-family:monospace;font-size:13px;color:#a7f3d0;word-break:break-all}"
"@media(max-width:620px){.grid{grid-template-columns:1fr}.joy{height:190px}}"
"</style></head><body><div class=\"wrap\">"
"<h1>ESP32S3 Remote</h1>"
"<div class=\"grid\"><div class=\"panel\"><label>Left rocker</label><div id=\"left\" class=\"joy\"><div class=\"knob\"></div></div></div>"
"<div class=\"panel\"><label>Right rocker</label><div id=\"right\" class=\"joy\"><div class=\"knob\"></div></div></div></div>"
"<div class=\"panel\"><div class=\"row\"><label>Dial</label><input id=\"dial\" type=\"range\" min=\"-1000\" max=\"1000\" value=\"0\"></div>"
"<div class=\"row\"><label>Left switch</label><button id=\"swl0\">0</button><button id=\"swl1\">1</button><button id=\"swl2\">2</button>"
"<label>Right switch</label><button id=\"swr0\">0</button><button id=\"swr1\">1</button><button id=\"swr2\">2</button></div>"
"<div class=\"row\"><button id=\"k0\">K0</button><button id=\"k1\">K1</button><button id=\"k2\">K2</button><button id=\"k3\">K3</button>"
"<button id=\"stop\">STOP</button></div><div id=\"status\" class=\"status\"></div></div></div>"
"<script>"
"const s={lx:0,ly:0,rx:0,ry:0,dial:0,swl:0,swr:0,keys:[0,0,0,0]};"
"function clamp(v){return Math.max(-1000,Math.min(1000,Math.round(v)));}"
"function bindJoy(id,xn,yn){const el=document.getElementById(id),k=el.querySelector('.knob');let active=false;"
"function set(e){const r=el.getBoundingClientRect(),p=e.touches?e.touches[0]:e;"
"const cx=r.left+r.width/2,cy=r.top+r.height/2,rad=Math.min(r.width,r.height)/2-36;"
"let dx=p.clientX-cx,dy=p.clientY-cy,m=Math.hypot(dx,dy);if(m>rad){dx=dx/m*rad;dy=dy/m*rad;}"
"k.style.left=(50+dx/r.width*100)+'%';k.style.top=(50+dy/r.height*100)+'%';s[xn]=clamp(dx/rad*1000);s[yn]=clamp(-dy/rad*1000);send();}"
"function center(){k.style.left='50%';k.style.top='50%';s[xn]=0;s[yn]=0;send();}"
"el.addEventListener('pointerdown',e=>{active=true;el.setPointerCapture(e.pointerId);set(e);});"
"el.addEventListener('pointermove',e=>{if(active)set(e);});"
"el.addEventListener('pointerup',e=>{active=false;center();});el.addEventListener('pointercancel',e=>{active=false;center();});}"
"function bindSwitch(prefix,name){[0,1,2].forEach(v=>document.getElementById(prefix+v).onclick=()=>{s[name]=v;paint();send();});}"
"function paint(){['swl','swr'].forEach(n=>[0,1,2].forEach(v=>document.getElementById(n+v).classList.toggle('on',s[n]===v)));"
"s.keys.forEach((v,i)=>document.getElementById('k'+i).classList.toggle('on',v));"
"document.getElementById('status').textContent=JSON.stringify(s);}"
"let t=0;function send(){paint();const now=Date.now();if(now-t<45)return;t=now;"
"fetch(`/api/control?lx=${s.lx}&ly=${s.ly}&rx=${s.rx}&ry=${s.ry}&dial=${s.dial}&swl=${s.swl}&swr=${s.swr}&k0=${s.keys[0]}&k1=${s.keys[1]}&k2=${s.keys[2]}&k3=${s.keys[3]}`).catch(()=>{});}"
"document.getElementById('dial').oninput=e=>{s.dial=Number(e.target.value);send();};"
"s.keys.forEach((_,i)=>document.getElementById('k'+i).onclick=()=>{s.keys[i]=s.keys[i]?0:1;send();});"
"document.getElementById('stop').onclick=()=>{s.lx=s.ly=s.rx=s.ry=s.dial=0;s.swl=s.swr=0;s.keys=[0,0,0,0];document.getElementById('dial').value=0;send();};"
"bindJoy('left','lx','ly');bindJoy('right','rx','ry');bindSwitch('swl','swl');bindSwitch('swr','swr');paint();setInterval(send,100);"
"</script></body></html>";

static int get_query_i16(httpd_req_t *req, const char *key, int def_value) {
    char query[192];
    char value[16];

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return def_value;
    }
    if (httpd_query_key_value(query, key, value, sizeof(value)) != ESP_OK) {
        return def_value;
    }

    int v = atoi(value);
    if (v > 1000) {
        return 1000;
    }
    if (v < -1000) {
        return -1000;
    }
    return v;
}

static uint8_t get_query_u8(httpd_req_t *req, const char *key, uint8_t def_value) {
    int v = get_query_i16(req, key, def_value);
    if (v < 0) {
        return 0;
    }
    if (v > 255) {
        return 255;
    }
    return (uint8_t)v;
}

static esp_err_t root_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, wifi_remote_page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t control_handler(httpd_req_t *req) {
    uint8_t frame[REMOTE_FRAME_LEN] = {0};

    frame[0] = REMOTE_FRAME_HEADER;
    frame[1] = get_query_u8(req, "k0", 0);
    frame[2] = get_query_u8(req, "k1", 0);
    frame[3] = get_query_u8(req, "k2", 0);
    frame[4] = get_query_u8(req, "k3", 0);
    i16_to_be(&frame[5], (int16_t)get_query_i16(req, "lx", 0));
    i16_to_be(&frame[7], (int16_t)get_query_i16(req, "ly", 0));
    i16_to_be(&frame[9], (int16_t)get_query_i16(req, "rx", 0));
    i16_to_be(&frame[11], (int16_t)get_query_i16(req, "ry", 0));
    i16_to_be(&frame[13], (int16_t)get_query_i16(req, "dial", 0));
    frame[15] = get_query_u8(req, "swl", 0);
    frame[16] = get_query_u8(req, "swr", 0);
    frame[17] = REMOTE_FRAME_FOOTER;

    forward_remote_payload(frame, REMOTE_FRAME_LEN);

    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "ok");
}

static void wifi_remote_http_start(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 4;

    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_handler,
        .user_ctx = NULL,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &root_uri));

    httpd_uri_t control_uri = {
        .uri = "/api/control",
        .method = HTTP_GET,
        .handler = control_handler,
        .user_ctx = NULL,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &control_uri));
}

static void wifi_remote_init(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    wifi_config_t ap_config = {
        .ap = {
            .ssid = WIFI_REMOTE_AP_SSID,
            .ssid_len = strlen(WIFI_REMOTE_AP_SSID),
            .channel = WIFI_CHANNEL,
            .password = WIFI_REMOTE_AP_PASSWORD,
            .max_connection = WIFI_REMOTE_MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK,
            .pmf_cfg = {
                .required = false,
            },
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi remote AP started. SSID:%s PASS:%s URL:http://192.168.4.1/",
             WIFI_REMOTE_AP_SSID, WIFI_REMOTE_AP_PASSWORD);
}
#else
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
    ESP_ERROR_CHECK(esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE));
}
#endif

void app_main(void) {
    // 1. 初始化 NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. 初始化用作透传的串口 (TX: 引脚17, RX: 引脚16)
    uart_transparent_init();

#if REMOTE_INPUT_MODE_WIFI
    // 3. 手机 WiFi 模拟遥控器模式：手机连接 AP 后访问网页，网页生成同协议 18 字节帧。
    wifi_remote_init();
    wifi_remote_http_start();
    ESP_LOGI(TAG, "WiFi phone remote started. Connect to %s and open http://192.168.4.1/",
             WIFI_REMOTE_AP_SSID);
#else
    // 3. 初始化 WiFi 并锁定信道
    wifi_init();

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
#endif
}
