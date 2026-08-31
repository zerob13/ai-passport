// main/adpcm_ima.c —— IMA ADPCM 编码/解码实现(纯 C)。
// 步长表与索引表取自 IMA 标准(与 WAVE IMA ADPCM 一致)。
#include "adpcm_ima.h"

static const int16_t k_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767,
};

static const int8_t k_index[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8,
};

void adpcm_ima_init(adpcm_ima_t *s)
{
    s->predictor = 0;
    s->index = 0;
}

// 编码一个采样,返回 4-bit nibble 并更新状态。
static inline uint8_t encode_sample(adpcm_ima_t *s, int16_t sample)
{
    int32_t diff = (int32_t)sample - s->predictor;
    uint8_t sign = (diff < 0) ? 8 : 0;
    if (sign) diff = -diff;
    if (diff > 32767) diff = 32767;

    uint32_t step = (uint32_t)k_step[s->index];
    uint32_t delta = 0;
    uint32_t vpdiff = step >> 3;
    if (diff >= (int32_t)step) { delta |= 4; diff -= step; vpdiff += step; }
    step >>= 1;
    if (diff >= (int32_t)step) { delta |= 2; diff -= step; vpdiff += step; }
    step >>= 1;
    if (diff >= (int32_t)step) { delta |= 1; vpdiff += step; }

    uint8_t nibble = (uint8_t)(sign | delta);

    int32_t pred = s->predictor + (sign ? -(int32_t)vpdiff : (int32_t)vpdiff);
    if (pred < -32768) pred = -32768;
    if (pred > 32767)  pred = 32767;
    s->predictor = (int16_t)pred;

    int idx = s->index + k_index[nibble];
    if (idx < 0) idx = 0;
    if (idx > 88) idx = 88;
    s->index = (uint8_t)idx;
    return nibble;
}

size_t adpcm_ima_encode(adpcm_ima_t *s, const int16_t *pcm, size_t n, uint8_t *out)
{
    size_t bytes = (n + 1) / 2;
    for (size_t i = 0; i < bytes; i++) {
        uint8_t lo = encode_sample(s, pcm[2 * i]);
        uint8_t hi = (2 * i + 1 < n) ? encode_sample(s, pcm[2 * i + 1]) : 0;
        out[i] = (uint8_t)(lo | (hi << 4));
    }
    return bytes;
}

// 解码一个 nibble,返回重建采样并更新状态。
static inline int16_t decode_sample(adpcm_ima_t *s, uint8_t nibble)
{
    uint32_t step = (uint32_t)k_step[s->index];
    uint32_t vpdiff = step >> 3;
    if (nibble & 4) vpdiff += step;
    if (nibble & 2) vpdiff += step >> 1;
    if (nibble & 1) vpdiff += step >> 2;

    int32_t pred = s->predictor;
    pred += (nibble & 8) ? -(int32_t)vpdiff : (int32_t)vpdiff;
    if (pred < -32768) pred = -32768;
    if (pred > 32767)  pred = 32767;
    s->predictor = (int16_t)pred;

    int idx = s->index + k_index[nibble & 0x0F];
    if (idx < 0) idx = 0;
    if (idx > 88) idx = 88;
    s->index = (uint8_t)idx;
    return s->predictor;
}

size_t adpcm_ima_decode(adpcm_ima_t *s, const uint8_t *in, size_t n, int16_t *out)
{
    // n 为编码字节数,还原出 2n 个采样;末尾多余的半字节按全 0 处理(与编码器对称:
    // 编码时若采样数为奇数,最后一个字节的高半字节补 0)。
    size_t samples = n * 2;
    for (size_t i = 0; i < n; i++) {
        out[2 * i]     = decode_sample(s, in[i] & 0x0F);
        out[2 * i + 1] = decode_sample(s, in[i] >> 4);
    }
    return samples;
}