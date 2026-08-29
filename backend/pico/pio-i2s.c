/*
* Copyright 2025 Kinetic Light and Lichen Community Systems
* Licensed under the BSD-3 License.
*/

#include <math.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "pio-i2s.h"

float PioI2S_calculateClockDivision(struct PioI2S* self) {
    float clockSpeed = (float) clock_get_hz(clk_sys);
    float bclkSpeed = (float) self->config->sampleRate *
        (float) PioI2S_NUM_CHANNELS * (float) self->config->bitDepth;
    float pioSpeed = bclkSpeed * (float) PioI2S_PIO_INSTRUCTIONS_PER_BIT;
    float clockDivision = clockSpeed / pioSpeed;

    return clockDivision;
}

/*
 * Patched for picopop.
 *
 * The original rejected any ratio whose fractional part was not a multiple of
 * 1/16, on the grounds that a PIO clock divider has "16 fractional bits". It
 * has eight - the divider is 16.8 fixed point - so the representable step is
 * 1/256, and the check was both wrong about the hardware and far stricter than
 * it needed to be. 8 kHz at 128 MHz happens to divide exactly, so nothing
 * noticed.
 *
 * That matters here because the game wants a real sample rate. 22050 Hz at
 * 128 MHz needs a divider of 45.3514, which the old check panicked on; rounded
 * to the nearest 1/256 it gives 22049.95 Hz, an error of 0.0002% - four orders
 * of magnitude below anything audible.
 *
 * So: check the range the hardware can actually express, and warn on the error
 * that rounding will cause rather than on whether it is zero.
 */
void PicoI2S_verifyPIOClockDivision(float frequencyRatio) {
    if (frequencyRatio < 1.0f) {
        panic("PIO clock ratio %f is faster than the system clock.",
            frequencyRatio);
    }
    if (frequencyRatio >= 65536.0f) {
        panic("PIO clock ratio %f exceeds the 16-bit integer divider.",
            frequencyRatio);
    }

    // The divider is 16.8 fixed point; work out what we will actually get.
    float rounded = ((float)(int)(frequencyRatio * 256.0f + 0.5f)) / 256.0f;
    float errorPpm = (rounded - frequencyRatio) / frequencyRatio * 1e6f;
    if (errorPpm < 0.0f) {
        errorPpm = -errorPpm;
    }

    if (errorPpm > 5000.0f) {   // 0.5%, about a tenth of a semitone
        panic("PIO clock ratio %f rounds to %f: %f ppm sample rate error.",
            frequencyRatio, rounded, errorPpm);
    }
}

void PioI2s_initPIOProgram(struct PioI2S* self) {
    int offset = pio_add_program(self->config->pio, &PioI2S_out_program);
    pio_sm_config sm_config = PioI2S_out_program_get_default_config(
        offset);

    pio_gpio_init(self->config->pio, self->config->dataPin);
    pio_gpio_init(self->config->pio, self->config->bclkPin);
    pio_gpio_init(self->config->pio, self->config->bclkPin + 1);
    sm_config_set_out_pins(&sm_config, self->config->dataPin, 1);
    sm_config_set_sideset_pin_base(&sm_config, self->config->bclkPin);
    sm_config_set_out_shift(&sm_config, false, false, self->config->bitDepth);
    sm_config_set_fifo_join(&sm_config, PIO_FIFO_JOIN_TX);

    float clockDivision = PioI2S_calculateClockDivision(self);
    PicoI2S_verifyPIOClockDivision(clockDivision);
    sm_config_set_clkdiv(&sm_config, clockDivision);
    pio_sm_init(self->config->pio, self->sm, offset, &sm_config);

    uint32_t pinMask = (1u << self->config->dataPin) |
        (3u << self->config->bclkPin);
    pio_sm_set_pindirs_with_mask(self->config->pio, self->sm, pinMask, pinMask);
    pio_sm_set_pins(self->config->pio, self->sm, 0);
}

void PioI2S_initControlChannel(struct PioI2S* self) {
    dma_channel_config config = dma_channel_get_default_config(
        self->controlChannel);
    channel_config_set_transfer_data_size(&config, DMA_SIZE_32);
    channel_config_set_read_increment(&config, true);
    channel_config_set_write_increment(&config, false);
    channel_config_set_ring(&config, false, 3);

    dma_channel_configure(self->controlChannel, &config,
        &dma_hw->ch[self->dataChannel].al3_read_addr_trig,
        &self->bufferPointers, 1, false);
}

void PioI2S_initDataChannel(struct PioI2S* self) {
    dma_channel_config config = dma_channel_get_default_config(
        self->dataChannel);
    channel_config_set_transfer_data_size(&config, DMA_SIZE_32);
    channel_config_set_read_increment(&config, true);
    channel_config_set_write_increment(&config, false);
    channel_config_set_dreq(&config, pio_get_dreq(
        self->config->pio, self->sm, true));
    channel_config_set_chain_to(&config, self->controlChannel);

    dma_channel_configure(self->dataChannel, &config,
        &self->config->pio->txf[self->sm],
        NULL, // Read address is set by the control channel.
        self->stereoBlockSize, false);
}

void PioI2S_initDMAIRQ(struct PioI2S* self) {
    dma_channel_set_irq0_enabled(self->dataChannel, true);
    irq_set_exclusive_handler(DMA_IRQ_0, self->dmaHandler);
    irq_set_enabled(DMA_IRQ_0, true);
}

void PioI2S_initDMA(struct PioI2S* self) {
    PioI2S_initDataChannel(self);
    PioI2S_initControlChannel(self);
    PioI2S_initDMAIRQ(self);
}

void PioI2S_init(struct PioI2S* self, struct PioI2S_Config* config,
    int32_t* outputDoubleBuffer, void(*dmaHandler)(void)) {
    self->config = config;
    self->sm = pio_claim_unused_sm(self->config->pio, true);
    self->dataChannel = dma_claim_unused_channel(true);
    self->controlChannel = dma_claim_unused_channel(true);
    self->stereoBlockSize = self->config->blockSize * PioI2S_NUM_CHANNELS;
    self->doubleBufferSize = self->stereoBlockSize * 2;
    self->outputDoubleBuffer = outputDoubleBuffer;
    self->bufferPointers[0] = &self->outputDoubleBuffer[0];
    self->bufferPointers[1] = &self->outputDoubleBuffer[self->stereoBlockSize];
    self->bufferPointerIdx = 0;
    self->dmaHandler = dmaHandler;

    PioI2s_initPIOProgram(self);
    PioI2S_initDMA(self);
}

void PioI2S_start(struct PioI2S* self) {
    dma_channel_start(self->controlChannel);
    pio_sm_set_enabled(self->config->pio, self->sm, true);
}

inline int32_t* PioI2S_nextOutputBuffer(struct PioI2S* self) {
    int32_t* bufferToFill = self->bufferPointers[self->bufferPointerIdx];
    self->bufferPointerIdx = 1 - self->bufferPointerIdx;
    return bufferToFill;
}

inline void PioI2S_endDMAInterruptHandler(struct PioI2S* self) {
    dma_hw->ints0 = 1u << self->dataChannel;
}

inline int32_t PioI2s_floatToInt32(float sample) {
    int32_t converted = (int32_t)(sample * 2147483647.0f);
    return converted;
}

inline void PioI2s_writeSamples(int32_t left, int32_t right,
    int32_t* interleavedOutput, size_t i) {
    interleavedOutput[i * 2] = left;
    interleavedOutput[i * 2 + 1] = right;
}

inline void PioI2s_writeStereo(float* left, float* right,
    size_t blockSize, int32_t* interleavedOutput) {
    for (size_t i = 0; i < blockSize; i++) {
        float leftSample = left[i];
        int32_t leftConverted = PioI2s_floatToInt32(leftSample);
        float rightSample = right[i];
        int32_t rightConverted = PioI2s_floatToInt32(rightSample);
        PioI2s_writeSamples(leftConverted, rightConverted,
            interleavedOutput, i);
    }
}

inline void PioI2s_writeMono(float* mono, size_t blockSize,
    int32_t* interleavedOutput) {
    for (size_t i = 0; i < blockSize; i++) {
        float sample = mono[i];
        sample = sample;
        int32_t converted = PioI2s_floatToInt32(sample);
        PioI2s_writeSamples(converted, converted, interleavedOutput, i);
    }
}
