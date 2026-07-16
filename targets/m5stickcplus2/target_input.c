#include "target_input.h"
#include <furi_hal_resources.h>
#include <driver/gpio.h>

#define TAG "InputM5Stick"

#define BTN_A_GPIO  GPIO_NUM_37
#define BTN_B_GPIO  GPIO_NUM_39
#define BTN_PWR_GPIO GPIO_NUM_35

#define DEBOUNCE_MS 20
#define LONG_PRESS_MS 500

typedef struct {
    uint32_t last_change_tick;
    bool pressed;
    bool long_press_reported;
} ButtonState;

static ButtonState btn_a = {0};
static ButtonState btn_b = {0};
static ButtonState btn_pwr = {0};

static void button_init(gpio_num_t gpio) {
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << gpio),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
}

static bool button_read(gpio_num_t gpio) {
    return gpio_get_level(gpio) == 0;
}

static InputEvent make_event(InputKey key, InputType type, uint32_t seq) {
    InputEvent event = {
        .sequence_source = INPUT_SEQUENCE_SOURCE_HARDWARE,
        .sequence_counter = seq,
        .key = key,
        .type = type,
    };
    return event;
}

static void event_publish(FuriPubSub* pubsub, uint32_t* seq, InputKey key, InputType type) {
    InputEvent event = make_event(key, type, *seq);
    furi_pubsub_publish(pubsub, &event);
    (*seq)++;
}

static void handle_button(FuriPubSub* pubsub, uint32_t* seq, ButtonState* btn, gpio_num_t gpio, InputKey key, InputKey long_key) {
    bool now = button_read(gpio);
    uint32_t tick = xTaskGetTickCount() * portTICK_PERIOD_MS;

    if(now != btn->pressed) {
        btn->pressed = now;

        if(now) {
            btn->last_change_tick = tick;
            btn->long_press_reported = false;
            event_publish(pubsub, seq, key, InputTypePress);
        } else {
            if(!btn->long_press_reported) {
                event_publish(pubsub, seq, key, InputTypeShort);
            }
            InputKey released_key = btn->long_press_reported ? long_key : key;
            event_publish(pubsub, seq, released_key, InputTypeRelease);
        }
    } else if(now) {
        if(!btn->long_press_reported && (tick - btn->last_change_tick > LONG_PRESS_MS)) {
            btn->long_press_reported = true;

            if(long_key != key) {
                event_publish(pubsub, seq, key, InputTypeRelease);
                /* Press before Short — the view dispatcher discards
                 * non-complementary events (Short with no preceding Press). */
                event_publish(pubsub, seq, long_key, InputTypePress);
                event_publish(pubsub, seq, long_key, InputTypeShort);
            }

            event_publish(pubsub, seq, long_key, InputTypeLong);
            btn->last_change_tick = tick;
        } else if(btn->long_press_reported && (tick - btn->last_change_tick > 200)) {
            btn->last_change_tick = tick;
            event_publish(pubsub, seq, long_key, InputTypeRepeat);
        }
    }
}

void target_input_init(void) {
    button_init(BTN_A_GPIO);
    button_init(BTN_B_GPIO);
    button_init(BTN_PWR_GPIO);
}

void target_input_poll(FuriPubSub* pubsub, uint32_t* sequence_counter) {
    // Button A (Front, most accessible): Short = Ok, Long = Ok
    handle_button(pubsub, sequence_counter, &btn_a, BTN_A_GPIO, InputKeyOk, InputKeyOk);

    // Button B (Side Top): Short = Down, Long = Up
    handle_button(pubsub, sequence_counter, &btn_b, BTN_B_GPIO, InputKeyDown, InputKeyUp);

    // Power Button (Side Bottom): Short = Back
    handle_button(pubsub, sequence_counter, &btn_pwr, BTN_PWR_GPIO, InputKeyBack, InputKeyBack);
}
