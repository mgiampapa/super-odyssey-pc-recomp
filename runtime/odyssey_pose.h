#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "cpu.h"

typedef struct OdysseyPoseFrame OdysseyPoseFrame;

bool odyssey_pose_enabled(void);
bool odyssey_motion_enabled(void);
void odyssey_pose_begin(void);
void odyssey_pose_end(uint64_t tick);
void odyssey_pose_player(uint64_t actor);
void odyssey_pose_capture(Cpu *c);
void odyssey_pose_draw(Cpu *c, float lead);  // this draw shows the models `lead` ticks past the last one (-1..1)

void odyssey_pose_render_frame(OdysseyPoseFrame *frame);
const void *odyssey_pose_render_palette(uint64_t address, uint64_t size);
