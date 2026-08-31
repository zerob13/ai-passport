// tests/test_adpcm_ima.c —— IMA ADPCM 编码器主机测试。
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "adpcm_ima.h"

// 正弦往返:误差应在量化噪声量级(阈值取宽,杜绝偶发边界抖动)。
static void test_roundtrip_sine(void)
{
    enum { N = 4000 };
    int16_t in[N], dec[N];
    uint8_t enc[(N + 1) / 2];
    adpcm_ima_t e, d;

    for (int i = 0; i < N; i++) {
        in[i] = (int16_t)(1500.0 * sin(2.0 * 3.14159265358979 * 300.0 * i / 16000.0));
    }
    adpcm_ima_init(&e);
    adpcm_ima_init(&d);
    size_t bytes = adpcm_ima_encode(&e, in, N, enc);
    assert(bytes == N / 2);
    assert(adpcm_ima_decode(&d, enc, bytes, dec) == N);

    int max_err = 0;
    // 跳过前 400 个采样:编码器从最小步长启动,开头有瞬态收敛误差(IMA 固有特性)
    for (int i = 400; i < N; i++) {
        int err = abs(in[i] - dec[i]);
        if (err > max_err) max_err = err;
    }
    assert(max_err < 300);
}

// 静音:全部为 0,输出也应为 0(预测器收敛在 0,步长最小)。
static void test_silence(void)
{
    enum { N = 400 };
    int16_t in[N], dec[N];
    uint8_t enc[N / 2];
    adpcm_ima_t e, d;

    memset(in, 0, sizeof(in));
    adpcm_ima_init(&e);
    size_t bytes = adpcm_ima_encode(&e, in, N, enc);
    assert(bytes == N / 2);
    for (size_t i = 0; i < bytes; i++) assert(enc[i] == 0x00);

    adpcm_ima_init(&d);
    assert(adpcm_ima_decode(&d, enc, bytes, dec) == N);
    for (int i = 0; i < N; i++) assert(dec[i] == 0);
}

// 黄金值:从零状态编码 [1000, 0]:
//   s0 diff=1000,step=7 → delta=7(4+2+1),vpdiff=11,predictor=11,index=8 → nibble 7
//   s1 diff=-11,step=16 → delta=2,vpdiff=10,predictor=1,index=7 → nibble 0xA
//   小端 nibble 打包 → 字节 0xA7
static void test_golden(void)
{
    int16_t in[2] = { 1000, 0 };
    uint8_t enc[1], dec_bytes[1];
    int16_t dec[2];
    adpcm_ima_t e, d;

    adpcm_ima_init(&e);
    assert(adpcm_ima_encode(&e, in, 2, enc) == 1);
    assert(enc[0] == 0xA7);

    adpcm_ima_init(&d);
    dec_bytes[0] = enc[0];
    assert(adpcm_ima_decode(&d, dec_bytes, 1, dec) == 2);
    assert(dec[0] == 11);
    assert(dec[1] == 1);
}

// 奇数采样:最后一个字节高半 nibble 补 0,解码对称。
static void test_odd_samples(void)
{
    int16_t in[5] = { 100, 200, 300, -400, 500 };
    uint8_t ref[2];
    uint8_t enc[3];
    int16_t dec[6];
    adpcm_ima_t e, r, d;

    // 同一声源编码两遍:一遍 5 采样整体,一遍 4 采样作参照,第 5 个采样应进低 nibble
    adpcm_ima_init(&e);
    assert(adpcm_ima_encode(&e, in, 5, enc) == 3);
    assert((enc[2] & 0x0F) != 0x00);              // 第 5 个采样在低 nibble
    assert((enc[2] >> 4) == 0x00);                // 补零的高 nibble

    adpcm_ima_init(&r);
    adpcm_ima_encode(&r, in, 4, ref);             // 只作状态延续参照,不校验内容

    adpcm_ima_init(&d);
    assert(adpcm_ima_decode(&d, enc, 3, dec) == 6);
    // 前 5 个采样误差应与普通往返同量级(即编码确实采到了全部 5 个采样)
    for (int i = 0; i < 5; i++) {
        int err = abs(in[i] - dec[i]);
        assert(err < 400);
    }
}

// 状态跨块延续:分两次编码 == 一次编码。
static void test_state_carries_across_chunks(void)
{
    enum { N = 1000 };
    int16_t in[N], dec1[N], dec2[N];
    uint8_t e1[(N + 1) / 2], e2[(N + 1) / 2];
    adpcm_ima_t a, b, d1, d2;

    for (int i = 0; i < N; i++) {
        in[i] = (int16_t)(12000.0 * sin(2.0 * 3.14159265358979 * 300.0 * i / 16000.0));
    }
    adpcm_ima_init(&a);
    adpcm_ima_init(&b);
    adpcm_ima_encode(&a, in, N, e1);
    adpcm_ima_encode(&b, in, N / 2, e2);
    assert(adpcm_ima_encode(&b, in + N / 2, N - N / 2, e2 + N / 4) == (N - N / 2 + 1) / 2);
    assert(memcmp(e1, e2, N / 2) == 0);

    adpcm_ima_init(&d1);
    adpcm_ima_init(&d2);
    adpcm_ima_decode(&d1, e1, N / 2, dec1);
    adpcm_ima_decode(&d2, e2, N / 2, dec2);
    assert(memcmp(dec1, dec2, N * sizeof(int16_t)) == 0);
}

int main(void)
{
    test_roundtrip_sine();
    test_silence();
    test_golden();
    test_odd_samples();
    test_state_carries_across_chunks();
    return 0;
}