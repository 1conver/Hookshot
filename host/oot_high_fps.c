/* (hookshot) The sound manager's job at 60 Hz and the physics solver's steps rounded up, under a Frame rate cap above 60: the
   lifted f_80435300, f_838ceb80 and f_838cfc10 call these (nx2/oot_patches.py); host/oot_high_fps.h has why and the logic.
   HOS_FPS_LOG (set): what they do, every 600 calls. */
#include "hos.h"
#include "oot_high_fps.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>

extern volatile uint32_t hos_frame_seconds_bits;   /* (host/nn_vi.c: the frame's seconds, 0 at a cap of 60) */

static int timed(void) { return __atomic_load_n(&hos_frame_seconds_bits, __ATOMIC_RELAXED) != 0; }
static int fps_log(void) {
    static int on = -1;
    if (on < 0) on = getenv("HOS_FPS_LOG") != NULL;
    return on;
}

static OotSoundGate sound_gate;
static SRWLOCK sound_lock = SRWLOCK_INIT;   /* (the job's phases may come from different threads) */

/* f_80435300's head: phase w2, slot the guest address of the pointer to the object whose +0x10 is the game step (the job's own
   load). 0: the job returns at once */
int hos_oot_sound_gate(Ctx *c, uint32_t phase, uint64_t slot) {
    (void)c;
    float step = 1.0f;
    const int t = timed();
    if (t && phase == 0) {
        const uint64_t object = G64(slot);
        const uint64_t time = object ? G64(object) : 0;
        if (time) step = oot_bits_float(*(const uint32_t *)GP(time + 0x10));
    }
    AcquireSRWLockExclusive(&sound_lock);
    const int run = oot_sound_gate(&sound_gate, phase, step, t);
    ReleaseSRWLockExclusive(&sound_lock);
    if (phase == 0 && fps_log()) {
        static long long calls, runs;
        runs += run;
        if (!(++calls % 600))
            hos_log("oot: sound job ran %lld of the last 600 frames (step %.3f%s)", runs, step,
                    oot_sound_partial(step, t) ? ", held to 60 Hz" : ""), runs = 0;
    }
    return run;
}

/* The step f_80435300's phase 0 hands to the sound engine (its float's bits) */
uint32_t hos_oot_sound_step(uint32_t step_bits) {
    return oot_sound_step(step_bits, timed());
}

/* The solver steps of f_838ceb80 / f_838cfc10: game, the game's count; scaled_bits, the float it cut; per_frame_bits, the steps
   of a 60 fps frame (a float) */
uint32_t hos_oot_solver_steps(uint32_t game, uint32_t scaled_bits, uint32_t per_frame_bits) {
    const int32_t steps = oot_solver_steps((int32_t)game, scaled_bits, per_frame_bits, timed());
    if (fps_log()) {
        static volatile LONG64 calls;
        const LONG64 n = InterlockedIncrement64(&calls);
        if (n % 600 == 1)
            hos_log("oot: physics solver steps %d (the game's %d: frame step %.3f x %.0f steps a 60 fps frame)", steps, (int32_t)game,
                    (oot_bits_float(scaled_bits) - 0.001f) / oot_bits_float(per_frame_bits), oot_bits_float(per_frame_bits));
    }
    return (uint32_t)steps;
}
