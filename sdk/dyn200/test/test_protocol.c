#include "dyn200.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void add_crc(uint8_t *p, size_t n)
{
        uint16_t c = dyn200_crc16(p, n);
        p[n] = (uint8_t)c; p[n + 1] = (uint8_t)(c >> 8);
}

/**
 * @brief 主动上传：从字节流里挑出最新的一帧（前面有噪声、半帧都不影响）
 */
static void test_scan_active(void)
{
        uint8_t stream[64];
        uint8_t f6[6] = {0x00, 0x7b, 0x00, 0x64, 0, 0};   // torque=123, speed=100
        uint8_t f6b[6] = {0x00, 0xc8, 0x00, 0x32, 0, 0};  // torque=200, speed=50
        uint8_t f8[8] = {0xff, 0xff, 0xfe, 0x01, 0x02, 0x03, 0, 0};
        size_t  offset = 0;
        int32_t torque, speed;
        uint32_t speed24;

        add_crc(f6, 4);
        add_crc(f6b, 4);

        // 噪声 + 老帧 + 半帧 + 新帧：应该拿到新帧
        memset(stream, 0, sizeof(stream));
        stream[0] = 0xaa;
        memcpy(stream + 1, f6, 6);
        stream[7] = 0x55;
        memcpy(stream + 8, f6b, 6);
        assert(dyn200_scan_active_frame(stream, 14, 6, &offset) == 1);
        assert(offset == 8);
        assert(dyn200_decode_active6(stream + offset, &torque, &speed) == 0);
        assert(torque == 200 && speed == 50);

        // 只有一帧也能找到
        assert(dyn200_scan_active_frame(f6, 6, 6, &offset) == 1 && offset == 0);

        // 全是噪声：找不到
        memset(stream, 0xa5, sizeof(stream));
        assert(dyn200_scan_active_frame(stream, sizeof(stream), 6, &offset) == 0);

        // 数据太少 / 帧长非法：找不到
        assert(dyn200_scan_active_frame(f6, 5, 6, &offset) == 0);
        assert(dyn200_scan_active_frame(f6, 6, 7, &offset) == 0);

        // 8 字节模式同样按最新一帧取
        add_crc(f8, 6);
        memcpy(stream, f8, 8);
        memcpy(stream + 8, f6, 6); // 不同长度混在一起时按请求的长度找
        assert(dyn200_scan_active_frame(stream, 16, 8, &offset) == 1 && offset == 0);
        assert(dyn200_decode_active8(stream + offset, &torque, &speed24) == 0);
        assert(torque == -2 && speed24 == 0x010203u);
}

int main(void)
{
        uint8_t request[] = {1,3,0,0,0,2};
        uint8_t f6[6] = {0x00,0x7b,0x80,0x64,0,0};
        uint8_t f8[8] = {0xff,0xff,0xfe,0x01,0x02,0x03,0,0};
        int32_t torque, speed; uint32_t speed24;
        assert(dyn200_crc16(request, sizeof(request)) == 0x0bc4);
        add_crc(f6, 4); assert(dyn200_decode_active6(f6, &torque, &speed) == 0);
        assert(torque == -123 && speed == 100);
        add_crc(f8, 6); assert(dyn200_decode_active8(f8, &torque, &speed24) == 0);
        assert(torque == -2 && speed24 == 0x010203u);
        f8[0] ^= 1; assert(dyn200_decode_active8(f8, &torque, &speed24) == DYN200_ERR_CRC);
        test_scan_active();
        puts("protocol tests passed"); return 0;
}
