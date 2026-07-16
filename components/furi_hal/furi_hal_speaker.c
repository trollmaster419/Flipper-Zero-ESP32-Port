/**
 * @file furi_hal_speaker.c
 * Speaker HAL — dual implementation:
 *   - I2S  tone generation  (boards with I2S DAC: T-Embed CC1101)
 *   - LEDC tone generation  (boards with a simple buzzer pin: M5StickC Plus2)
 *   - no-op stubs          (boards without any speaker hardware)
 *
 * Exposes the same API the rest of the firmware expects:
 *   - Mutex-based exclusive ownership (acquire/release/is_mine)
 *   - start(frequency, volume) / set_volume / stop
 *   - Cubic volume scaling (v^3) for perceptually linear loudness
 */

#include "furi_hal_speaker.h"
#include "boards/board.h"
#include <furi.h>

#define TAG "FuriHalSpeaker"

#if BOARD_HAS_SPEAKER

#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ── Common state ──────────────────────────────────────────────────── */

static FuriMutex* speaker_mutex = NULL;

static inline float speaker_volume_curve(float v) {
    if(v < 0.0f) v = 0.0f;
    if(v > 1.0f) v = 1.0f;
    return v * v * v;
}

/* ── Shared public API (ownership model) ──────────────────────────── */

bool furi_hal_speaker_acquire(uint32_t timeout) {
    furi_check(!FURI_IS_IRQ_MODE());
    return furi_mutex_acquire(speaker_mutex, timeout) == FuriStatusOk;
}

void furi_hal_speaker_release(void) {
    furi_check(!FURI_IS_IRQ_MODE());
    furi_check(furi_hal_speaker_is_mine());
    furi_hal_speaker_stop();
    furi_check(furi_mutex_release(speaker_mutex) == FuriStatusOk);
}

bool furi_hal_speaker_is_mine(void) {
    return (FURI_IS_IRQ_MODE()) ||
           (furi_mutex_get_owner(speaker_mutex) == furi_thread_get_current_id());
}

/* ═══════════════════════════════════════════════════════════════════
 *  IMPLEMENTATION SELECTION
 *
 *  Boards with an I2S DAC (T-Embed CC1101) define BOARD_PIN_SPEAKER_BCLK
 *  and friends.  Boards with a simple PWM buzzer (M5StickC Plus2) define
 *  BOARD_PIN_BUZZER.  We pick the right driver at compile time.
 * ═══════════════════════════════════════════════════════════════════ */

/* ── I2S path (T-Embed CC1101) ───────────────────────────────────── */
#if defined(BOARD_PIN_SPEAKER_BCLK) && BOARD_PIN_SPEAKER_BCLK >= 0

#include <driver/i2s_std.h>
#include <driver/gpio.h>
#include <esp_timer.h>

#define SPEAKER_SAMPLE_RATE   44100
#define SPEAKER_BITS          16
#define SPEAKER_DMA_DESC_NUM  4
#define SPEAKER_DMA_FRAME_NUM 256
#define SPEAKER_THREAD_STACK  2048

#define SPEAKER_MAX_CYCLE_SAMPLES 1024

#define MIRROR_SAMPLE_RATE 16000
#define MIRROR_BUF_FRAMES  64

typedef enum {
    SpeakerModeIdle,
    SpeakerModeTone,
    SpeakerModeGdoMirror,
} SpeakerMode;

static i2s_chan_handle_t i2s_tx_handle = NULL;
static bool i2s_channel_enabled = false;

static volatile SpeakerMode speaker_mode = SpeakerModeIdle;
static volatile float speaker_frequency = 0.0f;
static volatile float speaker_volume = 0.0f;
static volatile bool speaker_buffer_dirty = true;

static volatile gpio_num_t speaker_mirror_pin = (gpio_num_t)-1;
static volatile float speaker_mirror_volume = 0.0f;

static FuriThread* speaker_thread = NULL;
static volatile bool speaker_thread_run = false;

static int16_t* wave_buffer = NULL;
static size_t wave_buffer_samples = 0;
static size_t wave_buffer_bytes = 0;

static void speaker_generate_buffer(void) {
    float freq = speaker_frequency;
    float vol = speaker_volume_curve(speaker_volume);

    if(freq < 1.0f) freq = 1.0f;

    uint32_t samples_per_cycle = (uint32_t)(SPEAKER_SAMPLE_RATE / freq);
    if(samples_per_cycle < 2) samples_per_cycle = 2;
    if(samples_per_cycle > SPEAKER_MAX_CYCLE_SAMPLES) samples_per_cycle = SPEAKER_MAX_CYCLE_SAMPLES;

    size_t needed = samples_per_cycle * 2 * sizeof(int16_t);
    if(wave_buffer == NULL || wave_buffer_samples != samples_per_cycle) {
        if(wave_buffer) free(wave_buffer);
        wave_buffer = malloc(needed);
        wave_buffer_samples = samples_per_cycle;
    }
    wave_buffer_bytes = needed;

    float amplitude = vol * 32767.0f;
    for(uint32_t i = 0; i < samples_per_cycle; i++) {
        int16_t sample = (int16_t)(amplitude * sinf(2.0f * (float)M_PI * (float)i / (float)samples_per_cycle));
        wave_buffer[i * 2] = sample;
        wave_buffer[i * 2 + 1] = sample;
    }

    speaker_buffer_dirty = false;
}

static int32_t speaker_writer_thread(void* context) {
    UNUSED(context);
    static int16_t mirror_buf[MIRROR_BUF_FRAMES * 2];

    while(speaker_thread_run) {
        SpeakerMode mode = speaker_mode;

        if(mode == SpeakerModeTone) {
            if(speaker_buffer_dirty) {
                speaker_generate_buffer();
            }
            if(wave_buffer && wave_buffer_bytes > 0) {
                size_t bytes_written = 0;
                i2s_channel_write(i2s_tx_handle, wave_buffer, wave_buffer_bytes, &bytes_written, 100);
            }
            continue;
        }

        if(mode == SpeakerModeGdoMirror) {
            const gpio_num_t pin = speaker_mirror_pin;
            const float vol = speaker_volume_curve(speaker_mirror_volume);
            const int16_t amp = (int16_t)(vol * 32767.0f);
            const int64_t period_us = 1000000 / MIRROR_SAMPLE_RATE;

            int64_t next_us = esp_timer_get_time();
            for(int i = 0; i < MIRROR_BUF_FRAMES; i++) {
                while(esp_timer_get_time() < next_us) { /* spin */ }
                next_us += period_us;
                int16_t s = gpio_get_level(pin) ? amp : (int16_t)-amp;
                mirror_buf[i * 2] = s;
                mirror_buf[i * 2 + 1] = s;
            }

            size_t bytes_written = 0;
            i2s_channel_write(i2s_tx_handle, mirror_buf, sizeof(mirror_buf), &bytes_written, 100);
            continue;
        }

        furi_delay_ms(5);
    }
    return 0;
}

void furi_hal_speaker_init(void) {
    furi_assert(speaker_mutex == NULL);
    speaker_mutex = furi_mutex_alloc(FuriMutexTypeNormal);

    gpio_reset_pin((gpio_num_t)BOARD_PIN_SPEAKER_WCLK);

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = SPEAKER_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = SPEAKER_DMA_FRAME_NUM;
    chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &i2s_tx_handle, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SPEAKER_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)BOARD_PIN_SPEAKER_BCLK,
            .ws = (gpio_num_t)BOARD_PIN_SPEAKER_WCLK,
            .dout = (gpio_num_t)BOARD_PIN_SPEAKER_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_tx_handle, &std_cfg));

    speaker_thread = furi_thread_alloc_ex("SpeakerWorker", SPEAKER_THREAD_STACK, speaker_writer_thread, NULL);
    speaker_thread_run = true;
    furi_thread_start(speaker_thread);

    FURI_LOG_I(TAG, "I2S init OK (BCLK=%d WS=%d DOUT=%d)",
               BOARD_PIN_SPEAKER_BCLK, BOARD_PIN_SPEAKER_WCLK, BOARD_PIN_SPEAKER_DOUT);
}

void furi_hal_speaker_deinit(void) {
    furi_check(speaker_mutex != NULL);

    speaker_thread_run = false;
    furi_thread_join(speaker_thread);
    furi_thread_free(speaker_thread);
    speaker_thread = NULL;

    if(i2s_channel_enabled) {
        i2s_channel_disable(i2s_tx_handle);
        i2s_channel_enabled = false;
    }
    i2s_del_channel(i2s_tx_handle);
    i2s_tx_handle = NULL;

    if(wave_buffer) {
        free(wave_buffer);
        wave_buffer = NULL;
    }

    furi_mutex_free(speaker_mutex);
    speaker_mutex = NULL;
}

void furi_hal_speaker_start(float frequency, float volume) {
    furi_check(furi_hal_speaker_is_mine());
    if(volume <= 0.0f) { furi_hal_speaker_stop(); return; }
    speaker_frequency = frequency;
    speaker_volume = volume;
    speaker_buffer_dirty = true;
    speaker_mode = SpeakerModeTone;
}

void furi_hal_speaker_start_gdo_mirror(const GpioPin* gdo_pin, float volume) {
    furi_check(furi_hal_speaker_is_mine());
    furi_check(gdo_pin);
    furi_check(gdo_pin->pin < GPIO_NUM_MAX);
    if(volume <= 0.0f) { furi_hal_speaker_stop(); return; }
    speaker_mirror_pin = (gpio_num_t)gdo_pin->pin;
    speaker_mirror_volume = volume;
    speaker_mode = SpeakerModeGdoMirror;
}

void furi_hal_speaker_set_volume(float volume) {
    furi_check(furi_hal_speaker_is_mine());
    if(volume <= 0.0f) { furi_hal_speaker_stop(); return; }
    speaker_volume = volume;
    speaker_mirror_volume = volume;
    speaker_buffer_dirty = true;
}

void furi_hal_speaker_stop(void) {
    furi_check(furi_hal_speaker_is_mine());
    speaker_mode = SpeakerModeIdle;
}

void furi_hal_speaker_gdo_set_mute(bool mute) {
    /* No-op on I2S path — GDO mirror uses a different writer thread.
     * The LEDC path (below) implements the actual mute. */
    (void)mute;
}

/* ── LEDC path (M5StickC Plus2 / buzzer boards) ────────────────── */
#elif defined(BOARD_PIN_BUZZER) && BOARD_PIN_BUZZER >= 0

#include <driver/ledc.h>
#include <driver/gpio.h>

/*
 * LEDC timer/channel assignment — no conflict on the M5StickC Plus2
 * (the LCD uses SPI, not LEDC).
 */
#define SPKR_LEDC_SPEED  LEDC_HIGH_SPEED_MODE
#define SPKR_LEDC_TIMER  LEDC_TIMER_0
#define SPKR_LEDC_CH     LEDC_CHANNEL_0
#define SPKR_LEDC_RES    LEDC_TIMER_8_BIT     /* 0..255 */

/* 50 % duty — maximum output from the magnetic buzzer.  18 % was too
 * quiet; 50 % gives a loud square wave the buzzer can drive well. */
#define SPKR_MAX_DUTY     127U

/* ── GDO mirror thread (SubGHz demodulated audio → buzzer) ────── */

static FuriThread* gdo_mirror_thread = NULL;
static volatile bool gdo_thread_run = false;
static const GpioPin* gdo_mirror_gpio = NULL;
static volatile float gdo_mirror_vol = 0.0f;
static volatile bool gdo_muted = false;          /* RSSI-squelch: silences output without stopping thread */

static int32_t gdo_mirror_thread_func(void* context) {
    UNUSED(context);
    const GpioPin* pin = gdo_mirror_gpio;
    if(!pin) return 0;

    float vol = gdo_mirror_vol;
    if(vol > 1.0f) vol = 1.0f;
    if(vol < 0.0f) vol = 0.0f;
    uint32_t duty = (uint32_t)(vol * (float)SPKR_MAX_DUTY);
    if(duty > SPKR_MAX_DUTY) duty = SPKR_MAX_DUTY;

    /* Use 2 kHz carrier — near the buzzer's mechanical resonance */
    ledc_set_freq(SPKR_LEDC_SPEED, SPKR_LEDC_TIMER, 2000);

    int prev_level = -1;
    while(gdo_thread_run) {
        int level = (int)gpio_get_level((gpio_num_t)pin->pin);
        int active = level && !gdo_muted;
        if(active != prev_level) {
            ledc_set_duty(SPKR_LEDC_SPEED, SPKR_LEDC_CH, active ? duty : 0);
            ledc_update_duty(SPKR_LEDC_SPEED, SPKR_LEDC_CH);
            prev_level = active;
        }
        /* Yield to other tasks — 1ms polling (~1 kHz) captures the
         * audible envelope without starving the rest of the system. */
        furi_delay_ms(1);
    }

    /* Buzzer off at exit */
    ledc_set_duty(SPKR_LEDC_SPEED, SPKR_LEDC_CH, 0);
    ledc_update_duty(SPKR_LEDC_SPEED, SPKR_LEDC_CH);
    return 0;
}

void furi_hal_speaker_init(void) {
    furi_assert(speaker_mutex == NULL);
    speaker_mutex = furi_mutex_alloc(FuriMutexTypeNormal);

    ledc_timer_config_t timer_cfg = {
        .speed_mode      = SPKR_LEDC_SPEED,
        .timer_num       = SPKR_LEDC_TIMER,
        .duty_resolution = SPKR_LEDC_RES,
        .freq_hz         = 440,   /* scratch — overridden on first start() */
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

    ledc_channel_config_t chan_cfg = {
        .gpio_num   = BOARD_PIN_BUZZER,
        .speed_mode = SPKR_LEDC_SPEED,
        .channel    = SPKR_LEDC_CH,
        .timer_sel  = SPKR_LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&chan_cfg));

    FURI_LOG_I(TAG, "LEDC init OK (BUZZER=GPIO%d)", BOARD_PIN_BUZZER);
}

void furi_hal_speaker_deinit(void) {
    furi_check(speaker_mutex != NULL);

    if(gdo_thread_run) {
        gdo_thread_run = false;
        if(gdo_mirror_thread) {
            furi_thread_join(gdo_mirror_thread);
            furi_thread_free(gdo_mirror_thread);
            gdo_mirror_thread = NULL;
        }
    }

    ledc_stop(SPKR_LEDC_SPEED, SPKR_LEDC_CH, 0);
    furi_mutex_free(speaker_mutex);
    speaker_mutex = NULL;
}

void furi_hal_speaker_start(float frequency, float volume) {
    furi_check(furi_hal_speaker_is_mine());
    if(volume <= 0.0f) { furi_hal_speaker_stop(); return; }

    /* Clamp to audible range */
    uint32_t freq_hz = (uint32_t)(frequency + 0.5f);
    if(freq_hz < 20)   freq_hz = 20;
    if(freq_hz > 20000) freq_hz = 20000;

    /* Buzzer-friendly volume + duty cap */
    uint32_t duty = (uint32_t)(volume * (float)SPKR_MAX_DUTY);
    if(duty > SPKR_MAX_DUTY) duty = SPKR_MAX_DUTY;

    ledc_set_freq(SPKR_LEDC_SPEED, SPKR_LEDC_TIMER, freq_hz);
    ledc_set_duty(SPKR_LEDC_SPEED, SPKR_LEDC_CH, duty);
    ledc_update_duty(SPKR_LEDC_SPEED, SPKR_LEDC_CH);
}

void furi_hal_speaker_stop(void) {
    furi_check(furi_hal_speaker_is_mine());

    /* Stop GDO mirror thread if running */
    if(gdo_thread_run) {
        gdo_thread_run = false;
        if(gdo_mirror_thread) {
            furi_thread_join(gdo_mirror_thread);
            furi_thread_free(gdo_mirror_thread);
            gdo_mirror_thread = NULL;
        }
        gdo_mirror_gpio = NULL;
    }

    ledc_set_duty(SPKR_LEDC_SPEED, SPKR_LEDC_CH, 0);
    ledc_update_duty(SPKR_LEDC_SPEED, SPKR_LEDC_CH);
}

void furi_hal_speaker_set_volume(float volume) {
    furi_check(furi_hal_speaker_is_mine());
    if(volume <= 0.0f) { furi_hal_speaker_stop(); return; }
    float freq = (float)ledc_get_freq(SPKR_LEDC_SPEED, SPKR_LEDC_TIMER);
    furi_hal_speaker_start(freq, volume);
}

void furi_hal_speaker_start_gdo_mirror(const GpioPin* gdo_pin, float volume) {
    furi_check(furi_hal_speaker_is_mine());
    furi_check(gdo_pin);
    furi_check(gdo_pin->pin < GPIO_NUM_MAX);

    if(volume <= 0.0f) { furi_hal_speaker_stop(); return; }

    gdo_mirror_gpio = gdo_pin;
    gdo_mirror_vol = volume;
    gdo_thread_run = true;

    gdo_mirror_thread = furi_thread_alloc_ex("GdoMirror", 2048, gdo_mirror_thread_func, NULL);
    furi_thread_start(gdo_mirror_thread);

    FURI_LOG_I(TAG, "GDO mirror started on GPIO%d (vol=%.2f)", gdo_pin->pin, (double)volume);
}

void furi_hal_speaker_gdo_set_mute(bool mute) {
    /* Mute/unmute GDO mirror output (RSSI squelch).  The thread checks
     * gdo_muted on every iteration so the change takes effect within ~1 ms. */
    gdo_muted = mute;
}

#endif /* BOARD_PIN_SPEAKER_BCLK / BOARD_PIN_BUZZER */

#else /* !BOARD_HAS_SPEAKER */

/* ── No-op stubs for boards without any speaker hardware ──────────── */

void furi_hal_speaker_init(void) {}
void furi_hal_speaker_deinit(void) {}
bool furi_hal_speaker_acquire(uint32_t timeout) { (void)timeout; return true; }
void furi_hal_speaker_release(void) {}
bool furi_hal_speaker_is_mine(void) { return true; }
void furi_hal_speaker_start(float frequency, float volume) { (void)frequency; (void)volume; }
void furi_hal_speaker_start_gdo_mirror(const GpioPin* gdo_pin, float volume) { (void)gdo_pin; (void)volume; }
void furi_hal_speaker_set_volume(float volume) { (void)volume; }
void furi_hal_speaker_stop(void) {}
void furi_hal_speaker_gdo_set_mute(bool mute) { (void)mute; }

#endif /* BOARD_HAS_SPEAKER */
