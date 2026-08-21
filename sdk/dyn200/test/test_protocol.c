#include "dyn200.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static void add_crc(uint8_t *p, size_t n)
{
        uint16_t c = dyn200_crc16(p, n);
        p[n] = (uint8_t)c; p[n + 1] = (uint8_t)(c >> 8);
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
        puts("protocol tests passed"); return 0;
}
