#include <stdint.h>
volatile float g_voltage = 24.5f;
volatile struct Motor { float speed; uint32_t status; } g_motor = {1.25f, 7};
volatile uint8_t g_samples[4] = {1, 2, 3, 4};
int main(void) { (void)g_voltage; return g_motor.status + g_samples[0]; }
