#include "../momentum_app.h"
#include <furi_hal_infrared.h>
#include <furi_hal_nfc.h>
#include <furi_hal_subghz.h>

enum VarItemListIndex {
    VarItemListIndexSpiCc1101Handle,
    VarItemListIndexSpiNrf24Handle,
    VarItemListIndexUartEspChannel,
    VarItemListIndexUartNmeaChannel,
    VarItemListIndexUartGeneralChannel,
    VarItemListIndexIrTxPin,
    VarItemListIndexNfcPins,
};

#define SPI_DISABLED "Disabled"
#define SPI_BRUCE    "Bruce (default)"
#define SPI_DEFAULT  "Default 4"
#define SPI_EXTRA    "Extra 7"
#define UART_DISABLED "Disabled"
#define UART_BRUCE    "Bruce (default)"
#define UART_USART    "Usart 13,14"
#define UART_LPUART   "Lpuart 15,16"
#define IR_G19       "G19 (default)"
#define IR_G26       "G26"
#define NFC_G2625    "G26/G25 (default)"
#define NFC_G3233    "G32/G33"
#define NFC_DISABLED "Disabled"

static const char* const spi_text[SpiCount] = {
    [SpiDisabled] = SPI_DISABLED,
    [SpiBruce] = SPI_BRUCE,
    [SpiDefault] = SPI_DEFAULT,
    [SpiExtra] = SPI_EXTRA,
};

static const char* const uart_text[UartCount] = {
    [UartDisabled] = UART_DISABLED,
    [UartBruce] = UART_BRUCE,
    [UartUsart] = UART_USART,
    [UartLpuart] = UART_LPUART,
};

// The Bruce CC1101 pinout (MOSI=32, MISO=33, SCK=0, CS=26, GDO0=25) overlaps
// every NFC pin option (G26/G25 and G32/G33), so the radio and NFC are mutually
// exclusive: enabling one forces the other off. These point at the live items so
// a change to one can update the other's displayed value.
static VariableItem* cc1101_item;
static VariableItem* nfc_item;

static void nfc_item_set_text(VariableItem* item, NfcPins pins) {
    switch(pins) {
    case NfcPinsG26G25:
        variable_item_set_current_value_text(item, NFC_G2625);
        break;
    case NfcPinsG32G33:
        variable_item_set_current_value_text(item, NFC_G3233);
        break;
    default:
        variable_item_set_current_value_text(item, NFC_DISABLED);
        break;
    }
}

void momentum_app_scene_protocols_gpio_var_item_list_callback(void* context, uint32_t index) {
    MomentumApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

static void momentum_app_scene_protocols_gpio_cc1101_handle_changed(VariableItem* item) {
    MomentumApp* app = variable_item_get_context(item);
    momentum_settings.spi_cc1101_handle = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(
        item, spi_text[momentum_settings.spi_cc1101_handle]);

    /* Enabling the external radio conflicts with NFC on the shared pins: disable
     * NFC first (it resets G26/G25), then point the SPI bus at the Bruce pins. */
    if(momentum_settings.spi_cc1101_handle == SpiBruce &&
       momentum_settings.nfc_pins != NfcPinsDisabled) {
        momentum_settings.nfc_pins = NfcPinsDisabled;
        furi_hal_nfc_set_pins_config(NfcPinsDisabled);
        if(nfc_item) {
            variable_item_set_current_value_index(nfc_item, NfcPinsDisabled);
            nfc_item_set_text(nfc_item, NfcPinsDisabled);
        }
    }

    furi_hal_subghz_set_spi_config(momentum_settings.spi_cc1101_handle);
    app->save_settings = true;
}

static void momentum_app_scene_protocols_gpio_nrf24_handle_changed(VariableItem* item) {
    MomentumApp* app = variable_item_get_context(item);
    momentum_settings.spi_nrf24_handle = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(
        item, spi_text[momentum_settings.spi_nrf24_handle]);
    app->save_settings = true;
}

static void momentum_app_scene_protocols_gpio_esp32_channel_changed(VariableItem* item) {
    MomentumApp* app = variable_item_get_context(item);
    momentum_settings.uart_esp_channel = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(
        item, uart_text[momentum_settings.uart_esp_channel]);
    app->save_settings = true;
}

static void momentum_app_scene_protocols_gpio_nmea_channel_changed(VariableItem* item) {
    MomentumApp* app = variable_item_get_context(item);
    momentum_settings.uart_nmea_channel = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(
        item, uart_text[momentum_settings.uart_nmea_channel]);
    app->save_settings = true;
}

static void momentum_app_scene_protocols_gpio_ir_tx_pin_changed(VariableItem* item) {
    MomentumApp* app = variable_item_get_context(item);
    momentum_settings.ir_tx_pin = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(
        item, momentum_settings.ir_tx_pin == IrTxPinG19 ? IR_G19 : IR_G26);
    furi_hal_infrared_set_tx_output(
        momentum_settings.ir_tx_pin == IrTxPinG19 ? FuriHalInfraredTxPinInternal :
                                                     FuriHalInfraredTxPinExtPA7);
    app->save_settings = true;
}

static void momentum_app_scene_protocols_gpio_nfc_pins_changed(VariableItem* item) {
    MomentumApp* app = variable_item_get_context(item);
    momentum_settings.nfc_pins = variable_item_get_current_value_index(item);
    nfc_item_set_text(item, momentum_settings.nfc_pins);

    /* Enabling NFC conflicts with a Bruce CC1101 (shared pins): turn the radio
     * off first so its SPI bus releases the pins, then bring NFC up. */
    if(momentum_settings.nfc_pins != NfcPinsDisabled &&
       momentum_settings.spi_cc1101_handle == SpiBruce) {
        momentum_settings.spi_cc1101_handle = SpiDisabled;
        furi_hal_subghz_set_spi_config(SpiDisabled);
        if(cc1101_item) {
            variable_item_set_current_value_index(cc1101_item, SpiDisabled);
            variable_item_set_current_value_text(cc1101_item, spi_text[SpiDisabled]);
        }
    }

    furi_hal_nfc_set_pins_config(momentum_settings.nfc_pins);
    app->save_settings = true;
}

void momentum_app_scene_protocols_gpio_on_enter(void* context) {
    MomentumApp* app = context;
    VariableItemList* var_item_list = app->var_item_list;
    VariableItem* item;

    item = variable_item_list_add(
        var_item_list,
        "CC1101 SPI",
        SpiCount,
        momentum_app_scene_protocols_gpio_cc1101_handle_changed,
        app);
    variable_item_set_current_value_index(item, momentum_settings.spi_cc1101_handle);
    variable_item_set_current_value_text(
        item, spi_text[momentum_settings.spi_cc1101_handle]);
    cc1101_item = item;

    item = variable_item_list_add(
        var_item_list, "NRF24 SPI", SpiCount, momentum_app_scene_protocols_gpio_nrf24_handle_changed, app);
    variable_item_set_current_value_index(item, momentum_settings.spi_nrf24_handle);
    variable_item_set_current_value_text(
        item, spi_text[momentum_settings.spi_nrf24_handle]);

    item = variable_item_list_add(
        var_item_list,
        "ESP32/8266 UART",
        UartCount,
        momentum_app_scene_protocols_gpio_esp32_channel_changed,
        app);
    variable_item_set_current_value_index(item, momentum_settings.uart_esp_channel);
    variable_item_set_current_value_text(
        item, uart_text[momentum_settings.uart_esp_channel]);

    item = variable_item_list_add(
        var_item_list,
        "NMEA GPS UART",
        UartCount,
        momentum_app_scene_protocols_gpio_nmea_channel_changed,
        app);
    variable_item_set_current_value_index(item, momentum_settings.uart_nmea_channel);
    variable_item_set_current_value_text(
        item, uart_text[momentum_settings.uart_nmea_channel]);

    item = variable_item_list_add(
        var_item_list,
        "IR TX Pin",
        2,
        momentum_app_scene_protocols_gpio_ir_tx_pin_changed,
        app);
    variable_item_set_current_value_index(item, momentum_settings.ir_tx_pin);
    variable_item_set_current_value_text(
        item, momentum_settings.ir_tx_pin == IrTxPinG19 ? IR_G19 : IR_G26);

    item = variable_item_list_add(
        var_item_list,
        "NFC Pins",
        NfcPinsCount,
        momentum_app_scene_protocols_gpio_nfc_pins_changed,
        app);
    variable_item_set_current_value_index(item, momentum_settings.nfc_pins);
    nfc_item_set_text(item, momentum_settings.nfc_pins);
    nfc_item = item;

    variable_item_list_set_enter_callback(
        var_item_list, momentum_app_scene_protocols_gpio_var_item_list_callback, app);

    variable_item_list_set_selected_item(
        var_item_list,
        scene_manager_get_scene_state(app->scene_manager, MomentumAppSceneProtocolsGpio));

    view_dispatcher_switch_to_view(app->view_dispatcher, MomentumAppViewVarItemList);
}

bool momentum_app_scene_protocols_gpio_on_event(void* context, SceneManagerEvent event) {
    MomentumApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        scene_manager_set_scene_state(
            app->scene_manager, MomentumAppSceneProtocolsGpio, event.event);
        consumed = true;
        switch(event.event) {
        default:
            break;
        }
    }

    return consumed;
}

void momentum_app_scene_protocols_gpio_on_exit(void* context) {
    MomentumApp* app = context;
    variable_item_list_reset(app->var_item_list);
    cc1101_item = NULL;
    nfc_item = NULL;
}
