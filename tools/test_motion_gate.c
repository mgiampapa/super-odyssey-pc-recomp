// Standalone regression: clang tools/test_motion_gate.c -o test_motion_gate
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "../runtime/hle/motion_gate.h"

static void near(float a, float b) { assert(fabsf(a - b) < 0.00001f); }

int main(void) {
    MotionGate g;
    motion_gate_init(&g);
    float tilted[18] = {0.4f, 0.8f, -0.3f, 0.1f, 0.2f, -0.1f, 1, 2, 3,
                        0, 1, 0, -1, 0, 0, 0, 0, 1};
    float s[18];
    memcpy(s, tilted, sizeof s);
    motion_gate_sample(&g, s, false, 1000000000ull);
    // Opening a menu must not replace tilted gravity or orientation with identity.
    memset(s, 0, sizeof s);
    motion_gate_sample(&g, s, true, 1010000000ull);
    for (int i = 0; i < 18; i++) near(s[i], i >= 3 && i < 6 ? 0 : tilted[i]);
    // Physical movement during the menu must not enter the frozen game sample.
    for (int i = 0; i < 18; i++) s[i] = 9;
    motion_gate_sample(&g, s, true, 2000000000ull);
    for (int i = 0; i < 18; i++) near(s[i], i >= 3 && i < 6 ? 0 : tilted[i]);
    // Resume blends gravity and suppresses transient gyro readings for 250 ms.
    memcpy(s, tilted, sizeof s); s[0] = -0.6f;
    motion_gate_sample(&g, s, false, 3000000000ull);
    near(s[0], 0.4f); near(s[3], 0);
    memcpy(s, tilted, sizeof s); s[0] = -0.6f;
    motion_gate_sample(&g, s, false, 3125000000ull);
    near(s[0], -0.1f); near(s[3], 0);
    // Genuine movement passes unchanged once the transition is over.
    memcpy(s, tilted, sizeof s); s[0] = 2.5f;
    motion_gate_sample(&g, s, false, 3250000000ull);
    near(s[0], 2.5f); near(s[3], tilted[3]);
    // A second menu opening holds the newly delivered pose, not an older pose.
    motion_gate_sample(&g, s, true, 3300000000ull);
    near(s[0], 2.5f); near(s[3], 0);
    motion_gate_settle(&g, 4000000000ull);
    memcpy(s, tilted, sizeof s); s[0] = -5;
    motion_gate_sample(&g, s, false, 4500000000ull);
    near(s[0], 2.5f); near(s[3], 0);
    memcpy(s, tilted, sizeof s);
    motion_gate_sample(&g, s, false, 5000000000ull);
    near(s[0], 2.5f); near(s[3], 0);
    memcpy(s, tilted, sizeof s);
    motion_gate_sample(&g, s, false, 5250000000ull);
    near(s[0], tilted[0]); near(s[3], tilted[3]);
    MotionBias b = {0};
    const float accel[3] = {0, 0.13f, -1.03f};
    float gyro[3];
    for (int i = 0; i < 500; i++) {
        gyro[0] = -0.001f; gyro[1] = 0.006f; gyro[2] = 0;
        motion_bias_sample(&b, accel, gyro, true, 6000000000ull + i * 5000000ull);
    }
    assert(fabsf(gyro[1]) < 0.00001f);
    float saved_bias = b.bias[1];
    gyro[0] = 0; gyro[1] = 0.25f; gyro[2] = 0;
    motion_bias_sample(&b, accel, gyro, true, 8500000000ull);
    near(b.bias[1], saved_bias); near(gyro[1], 0.25f - saved_bias);
    gyro[0] = 0; gyro[1] = 0.01f; gyro[2] = 0;
    motion_bias_sample(&b, accel, gyro, false, 8600000000ull);
    near(b.bias[1], saved_bias); near(gyro[1], 0.01f - saved_bias);
    puts("motion gate: menu/load settling, gyro bias and intentional motion passed");
    return 0;
}
