/*
 * Audio backend: PIO I2S into a MAX98357A pair, mixed on core 1.
 *
 * Core 1 exists for this. The mixer is the only hard-real-time work in the
 * firmware and it is about to get much more expensive (OPL3 music), so it gets
 * its own core rather than stealing time from the game loop.
 *
 * Two things here are load-bearing and easy to get wrong:
 *
 * 1. Everything on the audio path is __not_in_flash_func. BTstack persists
 *    pairings to flash, and a flash erase stops XIP - any code fetched from
 *    flash during that window hard-faults. Core 1 is locked out while it
 *    happens (see multicore_lockout_victim_init below), but an interrupt at a
 *    higher priority than the lockout's could still preempt the spin, so the
 *    handler must not need flash to run. Audio glitches for the duration of a
 *    pairing write; that is the correct trade.
 *
 * 2. PioI2S_init is called *from core 1*. irq_set_exclusive_handler and
 *    irq_set_enabled act on the calling core's NVIC, so calling them from core 0
 *    would leave the DMA interrupt firing on the wrong core and the mixer never
 *    running.
 */
#include <stdio.h>

#include "hardware/clocks.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pico/sync.h"

#include "pio-i2s.h"
#include "psdl_internal.h"
#include "psdl_pico.h"

static struct PioI2S_Config s_i2s_config;
static struct PioI2S        s_i2s;

/* The DMA double buffer, and one block of int16 stereo for the SDL callback to
 * fill before it is widened into the DMA buffer. */
static int32_t s_dma_buffer[PSDL_AUDIO_BLOCK_FRAMES * PioI2S_NUM_CHANNELS * 2];
static int16_t s_mix_buffer[PSDL_AUDIO_BLOCK_FRAMES * 2];

static volatile bool s_running;
static volatile bool s_core1_up;
static int           s_open_rate;

/*
 * SDL_LockAudio has to hold off a mixer running on the other core, so this is a
 * real hardware spinlock rather than a flag.
 *
 * It has to be recursive, and an RP2040 spinlock is not: psdl_audio_render holds
 * it across the client's callback, so a callback that itself calls
 * SDL_LockAudio - which is legal in SDL and easy to reach through a helper -
 * would take the same spinlock twice on the same core and hang forever.
 * Tracking the owning core makes the second acquisition a counter bump.
 *
 * Reading s_audio_lock_owner unlocked is safe: only the owner ever writes it,
 * so no other core can make it read as ours.
 */
static spin_lock_t *s_audio_lock;
static uint32_t     s_audio_lock_state;
static volatile int s_audio_lock_owner = -1;
static volatile int s_audio_lock_depth;

void psdl_backend_audio_lock(void)
{
	int core = (int)get_core_num();

	if (s_audio_lock_owner == core) {
		s_audio_lock_depth++;
		return;
	}

	uint32_t save = spin_lock_blocking(s_audio_lock);
	s_audio_lock_state = save;
	s_audio_lock_owner = core;
	s_audio_lock_depth = 1;
}

void psdl_backend_audio_unlock(void)
{
	if (--s_audio_lock_depth > 0)
		return;
	s_audio_lock_owner = -1;
	spin_unlock(s_audio_lock, s_audio_lock_state);
}

/* ------------------------------------------------------- the mixer path */

/*
 * Master volume, 0..PSDL_VOLUME_UNITY, applied to everything on its way to the
 * DAC. This is the last point where every source - music, sound effects, the
 * demo's synth - has been summed, so it is the only place a single control can
 * cover all of them.
 *
 * A MAX98357A into a small speaker is loud, and a bug in the mixer is loud at
 * full scale, so the default is deliberately not unity: see
 * PSDL_DEFAULT_VOLUME in psdl_internal.h.
 */
static volatile int s_volume = PSDL_DEFAULT_VOLUME;

void PSDL_SetMasterVolume(int volume)
{
	if (volume < 0)
		volume = 0;
	if (volume > PSDL_VOLUME_UNITY)
		volume = PSDL_VOLUME_UNITY;
	s_volume = volume;
}

int PSDL_GetMasterVolume(void)
{
	return s_volume;
}

static void __not_in_flash_func(fill_block)(int32_t *out)
{
	if (!s_running) {
		memset(out, 0, sizeof(int32_t) * PSDL_AUDIO_BLOCK_FRAMES * PioI2S_NUM_CHANNELS);
		return;
	}

	psdl_audio_render(s_mix_buffer, PSDL_AUDIO_BLOCK_FRAMES);

	int volume = s_volume;   /* read once; core 0 can change it mid-block */

	/* The MAX98357A takes 32-bit I2S frames; the PIO program drops the low
	 * bit of each. Shifting a 16-bit sample up by 16 puts it in the top half,
	 * which is where the DAC expects the magnitude.
	 *
	 * The clamp matters: at unity a full-scale -32768 scales to exactly
	 * -32768, and 32767 is the largest positive that survives the shift. */
	for (int i = 0; i < PSDL_AUDIO_BLOCK_FRAMES * 2; ++i) {
		int32_t s = ((int32_t)s_mix_buffer[i] * volume) >> PSDL_VOLUME_SHIFT;
		if (s >  32767) s =  32767;
		if (s < -32768) s = -32768;
		out[i] = s << 16;
	}
}

static void __not_in_flash_func(i2s_dma_handler)(void)
{
	int32_t *buffer = PioI2S_nextOutputBuffer(&s_i2s);
	fill_block(buffer);
	PioI2S_endDMAInterruptHandler(&s_i2s);
}

static void core1_audio_main(void)
{
	/* Let core 0 stop us cleanly when it writes flash - without this, the
	 * SDK's flash_safe_execute has no way to park core 1 and either fails the
	 * write or corrupts the chip. */
	multicore_lockout_victim_init();

	PioI2S_init(&s_i2s, &s_i2s_config, s_dma_buffer, i2s_dma_handler);
	PioI2S_start(&s_i2s);

	s_core1_up = true;

	/* All the work happens in the DMA interrupt, which is now bound to this
	 * core. Nothing else runs here. */
	for (;;)
		tight_loop_contents();
}

/* ------------------------------------------------------ backend interface */

void psdl_backend_audio_open(int freq, int channels, int block_frames)
{
	if (channels != PioI2S_NUM_CHANNELS)
		psdl_panic("picosdl: I2S output is stereo only (asked for %d)", channels);
	if (block_frames != PSDL_AUDIO_BLOCK_FRAMES)
		psdl_panic("picosdl: audio block is fixed at %d frames (asked for %d)",
		           PSDL_AUDIO_BLOCK_FRAMES, block_frames);

	if (s_core1_up) {
		if (freq != s_open_rate)
			psdl_panic("picosdl: cannot change the sample rate after start "
			           "(open at %d, now %d)", s_open_rate, freq);
		return;
	}

	s_audio_lock = spin_lock_instance(spin_lock_claim_unused(true));

	s_i2s_config.dataPin    = PSDL_PICO_I2S_DATA_PIN;
	s_i2s_config.bclkPin    = PSDL_PICO_I2S_BCLK_PIN;
	s_i2s_config.bitDepth   = 32;
	s_i2s_config.sampleRate = freq;
	s_i2s_config.blockSize  = PSDL_AUDIO_BLOCK_FRAMES;
	s_i2s_config.pio        = pio1;   /* pio0 belongs to the display */
	s_open_rate             = freq;

	multicore_launch_core1(core1_audio_main);

	/* Wait for the mixer to come up, so a caller that immediately unpauses
	 * does not race the DMA start. */
	while (!s_core1_up)
		tight_loop_contents();

	printf("picosdl: audio %d Hz stereo, %d-frame blocks (%.1f ms), mixing on core 1\n",
	       freq, PSDL_AUDIO_BLOCK_FRAMES,
	       1000.0 * PSDL_AUDIO_BLOCK_FRAMES / (double)freq);
}

void psdl_backend_audio_pause(int pause_on)
{
	s_running = !pause_on;
}

void psdl_backend_audio_close(void)
{
	s_running = false;
	/* Core 1 keeps running and keeps clocking out silence. Tearing down the
	 * DMA chain would buy nothing on a device that never exits. */
}
