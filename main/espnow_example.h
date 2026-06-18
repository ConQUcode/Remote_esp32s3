#ifndef ESPNOW_EXAMPLE_H
#define ESPNOW_EXAMPLE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 定义数据帧格式，严格按照 18 字节对齐
typedef struct {
    uint8_t header;         // 固定为 0xAA
    uint8_t KEY[4];         // 按键状态数据
    int16_t rocker_l_;      // 大端序 左摇杆 X轴
    int16_t rocker_l1;      // 大端序 左摇杆 Y轴
    int16_t rocker_r_;      // 大端序 右摇杆 X轴
    int16_t rocker_r1;      // 大端序 右摇杆 Y轴
    int16_t dial;           // 大端序 拨轮数据
    uint8_t switch_left;    // 左侧开关状态
    uint8_t switch_right;   // 右侧开关状态
    uint8_t footer;         // 固定为 0x55
} __attribute__((packed)) remote_data_t;

// 大端序到系统原生端序 (小端) 的转换函数
static inline int16_t be16_to_i16(int16_t val) {
    const uint8_t *p = (const uint8_t *)&val;
    return (int16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

#ifdef __cplusplus
}
#endif

#endif

