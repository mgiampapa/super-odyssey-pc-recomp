#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    float bias[3], last_accel[3];
    uint64_t last_ns;
} MotionBias;

// Learn only small, stationary gyro offsets during a settling window. Real
// turns/shakes do not update the calibration; their values pass through.
static inline void motion_bias_sample(MotionBias *b, const float accel[3], float gyro[3], bool settling, uint64_t now) {
    float gravity = 0, rotation = 0, change = 0;
    for (int i = 0; i < 3; i++) {
        gravity += accel[i] * accel[i];
        rotation += gyro[i] * gyro[i];
        float d = accel[i] - b->last_accel[i];
        change += d * d;
    }
    if (settling && b->last_ns && gravity > 0.8f * 0.8f && gravity < 1.2f * 1.2f &&
        rotation < 0.02f * 0.02f && change < 0.02f * 0.02f) {
        float dt = now > b->last_ns && now - b->last_ns < 50000000ull ? (float)(now - b->last_ns) / 1e9f : 0.005f;
        float alpha = dt / (0.15f + dt);
        for (int i = 0; i < 3; i++) b->bias[i] += alpha * (gyro[i] - b->bias[i]);
    }
    memcpy(b->last_accel, accel, sizeof b->last_accel);
    b->last_ns = now;
    for (int i = 0; i < 3; i++) gyro[i] -= b->bias[i];
}

typedef struct {
    float last[18], resume_accel[3];
    bool blocked;
    uint64_t resume_ns, settle_until_ns;
} MotionGate;

static inline void motion_gate_init(MotionGate *g) {
    memset(g, 0, sizeof *g);
    g->last[2] = -1;
    g->last[9] = g->last[13] = g->last[17] = 1;
}

static inline void motion_gate_settle(MotionGate *g, uint64_t now) {
    g->settle_until_ns = now + 1000000000ull;
    g->blocked = true;
}

// Keep the reported gravity and orientation continuous at menu transitions.
// A synthetic identity pose is a physical jerk to the game's shake detector.
static inline void motion_gate_sample(MotionGate *g, float sample[18], bool blocked, uint64_t now) {
    blocked = blocked || now < g->settle_until_ns;
    if (blocked) {
        memcpy(sample, g->last, sizeof g->last);
        memset(sample + 3, 0, 3 * sizeof(float));
        g->blocked = true;
    } else {
        if (g->blocked) {
            memcpy(g->resume_accel, g->last, sizeof g->resume_accel);
            g->resume_ns = now;
            g->blocked = false;
        }
        if (g->resume_ns && now - g->resume_ns < 250000000ull) {
            float blend = (float)(now - g->resume_ns) / 250000000.0f;
            for (int i = 0; i < 3; i++) sample[i] = g->resume_accel[i] + blend * (sample[i] - g->resume_accel[i]);
            memset(sample + 3, 0, 3 * sizeof(float));
        }
    }
    memcpy(g->last, sample, sizeof g->last);
}
