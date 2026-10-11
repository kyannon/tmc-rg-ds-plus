#ifndef PORT_SAMPLER_H
#define PORT_SAMPLER_H

/* Game-thread sampling profiler (Linux handheld builds, TMC_SAMPLE=1).
 * See port_sampler.c. Both calls are main-thread only and no-ops unless
 * enabled. */

#ifdef __cplusplus
extern "C" {
#endif

void Port_Sampler_Init(void);
/* 1 when the game thread leaves frame pacing (game logic + port per-tick
 * work begins), 0 when it re-enters VBlankIntrWait. Reports are logged from
 * the 0 call every few seconds. */
void Port_Sampler_GameSpan(int inGame);

#ifdef __cplusplus
}
#endif

#endif
