// main/adpcm_ima.h —— IMA ADPCM 4-bit 编码器(纯 C,可主机测试)。
// 每采样 4 bit,总体压缩比 4:1。编码状态跨块延续(块间携带预测器/步长),
// 一条录音从头到尾共用一个状态;解码端在 AUDIO_START 时重置标准 IMA 步长表。
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int16_t predictor;   // 上一个重建采样
    uint8_t index;       // 步长表索引 0..88
} adpcm_ima_t;

// 重置编码状态(录音开始时调用一次)。
void adpcm_ima_init(adpcm_ima_t *s);

// 编码 n 个 16-bit 单声道采样到 out(容量至少 (n+1)/2 字节)。
// 打包顺序:第 0 个采样 → 低 4 bit,第 1 个采样 → 高 4 bit(小端 nibble 序,与
// 常见 IMA/WAVE ADPCM 一致)。返回写出的字节数。
size_t adpcm_ima_encode(adpcm_ima_t *s, const int16_t *pcm, size_t n, uint8_t *out);

// IMA 标准解码:把 (n/2) 个编码字节还原为 n 个采样(用于测试/参考,设备端不链接)。
// out 容量至少 n * sizeof(int16_t)。返回写出的采样数。
size_t adpcm_ima_decode(adpcm_ima_t *s, const uint8_t *in, size_t n, int16_t *out);

#ifdef __cplusplus
}
#endif