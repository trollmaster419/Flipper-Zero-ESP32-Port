/**
 * Direct UART0 <-> RPC bridge for qFlipper on the classic ESP32 (M5StickC Plus2).
 *
 * UART0 is the only serial path to the host (USB goes through a fixed USB-UART
 * bridge chip). When the qFlipper app enables this bridge, UART0 is taken over
 * for protobuf RPC: host bytes are fed straight into an RpcSession and the
 * session's output is written straight back to UART0. No CLI shell, no
 * start_rpc_session handshake — the (patched) qFlipper host speaks RPC
 * immediately. Modeled on applications/services/usb_rpc/usb_rpc.c, but over the
 * ESP-IDF uart driver instead of TinyUSB CDC.
 *
 * While the bridge is active both log sinks on UART0 are silenced: the native
 * ESP-IDF logger here, and furi_log's console sink via the qFlipper app
 * (furi_log_set_console_muted). When disabled, UART0 returns to logging.
 */

#include <furi.h>
#include <furi_hal.h>
#include <driver/uart.h>
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <esp_rom_uart.h>
#include <momentum_settings/momentum_settings.h>
#include <rpc/rpc.h>

#define TAG       "CliUartRpc"
#define UART_NUM  UART_NUM_0

/* qFlipper pipelines storage_write chunks over UART0 without waiting for an ack
 * (only periodic StatusPings get responses), and a USB-UART bridge chip gives
 * UART0 no way to backpressure the host the way real USB-CDC NAKs do. So the
 * host blasts the whole file at line rate while the consumer (RPC -> storage ->
 * littlefs) stalls for tens of ms on each flash erase/GC. If the driver's RX
 * ring overflows during such a stall, bytes are dropped, the protobuf stream
 * desyncs, and the decoder faults -> device crash.
 *
 * At 115200 baud (~11.5 KB/s) a 1 KB ring fills in <90 ms — easily overrun by a
 * single erase. A 16 KB ring buffers >1 s of input, far longer than any flash
 * stall, and since average flash throughput >> 11.5 KB/s the backlog always
 * drains. READ_CHUNK is the per-read drain size; bigger empties the ring faster
 * after a stall. */
#define RX_RING_SIZE (16 * 1024)
#define TX_RING_SIZE (4 * 1024)
#define READ_CHUNK   2048
#define RX_POLL_MS 10

static FuriThread* cli_uart_thread = NULL;

typedef struct {
    volatile bool terminated;
    volatile bool closed;
} CliUartRpcCtx;

/* RPC -> UART0. Runs on the RpcSessionWorker thread. */
static void cli_uart_rpc_send_bytes(void* context, uint8_t* bytes, size_t len) {
    UNUSED(context);
    while(len > 0) {
        int written = uart_write_bytes(UART_NUM, (const char*)bytes, len);
        if(written <= 0) break;
        bytes += written;
        len -= (size_t)written;
    }
}

/* Fired by the RPC worker when the session is poisoned by a decode error
 * (e.g. a stray byte when the host unplugs/reconnects). Once that happens the
 * decoder is stuck permanently — rpc.c only clears decode_error in
 * rpc_session_open — so the loop below recycles the session. USB/BLE transports
 * tear the session down on this same callback; the raw UART bridge must too. */
static void cli_uart_rpc_closed(void* context) {
    CliUartRpcCtx* ctx = context;
    ctx->closed = true;
}

/* Fired (on the FreeRTOS timer task, via rpc_session_thread_pending_callback)
 * once the RPC session worker has fully stopped. This MUST NOT signal through a
 * primitive the session task then frees: furi_semaphore_release() touches the
 * semaphore again after the give (furi_event_loop_link_notify), so if the woken
 * session task frees it in between, the timer task dereferences freed memory
 * (LoadProhibited at +0x14). A plain flag write into the still-alive stack `ctx`
 * has nothing to free — the session task only polls it. */
static void cli_uart_rpc_terminated(void* context) {
    CliUartRpcCtx* ctx = context;
    ctx->terminated = true;
}
static void cli_uart_dummy_putc(char c) {
    UNUSED(c);
}
static int32_t cli_uart_session_task(void* context) {
    UNUSED(context);

    /* Take over UART0 so nothing corrupts the protobuf stream:
     *  - esp_log_level_set NONE silences the native ESP-IDF logger (ESP_LOGx),
     *  - furi console is muted by the qFlipper app (FURI_LOG),
     *  - esp_rom_install_channel_putc(1, NULL) disconnects the low-level ROM
     *    console putc, which is the ONLY thing that catches esp_rom_printf().
     *    esp_rom_printf writes straight to the UART ROM channel, bypassing both
     *    log levels and the furi mute — it's used by scattered debug traces
     *    (loader, file browser, desktop input, low-stack diag, ...) that would
     *    otherwise inject text mid-frame and desync qFlipper. Killing the putc
     *    centrally suppresses all of them at once without touching each call. */
    esp_log_level_set("*", ESP_LOG_NONE);
    esp_rom_install_channel_putc(1, cli_uart_dummy_putc);

    if(!uart_is_driver_installed(UART_NUM)) {
        uart_driver_install(UART_NUM, RX_RING_SIZE, TX_RING_SIZE, 0, NULL, 0);
    }

    Rpc* rpc = furi_record_open(RECORD_RPC);
    uint8_t* rx = malloc(READ_CHUNK);

    /* Outer loop: own a fresh RPC session, pump it, and recycle it whenever it
     * gets poisoned (decode error -> cli_uart_rpc_closed). This makes the bridge
     * survive disconnect/reconnect cycles instead of wedging on the first stray
     * byte the way a single long-lived session does. */
    while(momentum_settings.qflipper_enabled) {
        /* Drop boot/console noise (or leftover bytes from a poisoned session)
         * sitting in the RX FIFO before this session starts decoding. */
        uart_flush_input(UART_NUM);

        CliUartRpcCtx ctx = {.terminated = false, .closed = false};
        RpcSession* session = rpc_session_open(rpc, RpcOwnerUart);
        if(session == NULL) {
            /* Transient (e.g. the previous session is still tearing down / heap
             * pressure). Back off and retry rather than killing the bridge for
             * good — otherwise one failed reopen wedges qFlipper permanently. */
            furi_delay_ms(100);
            continue;
        }

        rpc_session_set_context(session, &ctx);
        rpc_session_set_send_bytes_callback(session, cli_uart_rpc_send_bytes);
        rpc_session_set_close_callback(session, cli_uart_rpc_closed);
        rpc_session_set_terminated_callback(session, cli_uart_rpc_terminated);

        while(momentum_settings.qflipper_enabled && !ctx.closed) {
            int len = uart_read_bytes(UART_NUM, rx, READ_CHUNK, RX_POLL_MS / portTICK_PERIOD_MS);
            if(len <= 0) continue;

            /* Feed everything we read; rpc_session_feed applies backpressure (it
             * blocks until the consumer drains), so loop until all bytes land. */
            size_t fed = 0;
            while(fed < (size_t)len && momentum_settings.qflipper_enabled && !ctx.closed) {
                fed += rpc_session_feed(session, rx + fed, (size_t)len - fed, 1000);
            }
        }

        /* Tear this session down (poisoned or bridge shutting down) and wait for
         * the worker to fully terminate before opening the next one. */
        rpc_session_close(session);
        /* Wait for the worker to fully terminate (cli_uart_rpc_terminated sets
         * the flag from the timer task) before reusing `rx`/opening a new
         * session. Poll a flag rather than a freeable semaphore — see the note
         * on cli_uart_rpc_terminated for why. */
        while(!ctx.terminated) furi_delay_ms(1);
    }
    free(rx);

    furi_record_close(RECORD_RPC);
    /* Restore raw console output now that the bridge is releasing UART0. */
    esp_rom_install_channel_putc(1, esp_rom_output_putc);
    esp_log_level_set("*", ESP_LOG_INFO);
    return 0;
}

void cli_uart_bridge_start() {
    if(cli_uart_thread) return;
    cli_uart_thread = furi_thread_alloc_ex("CliUartRpc", 4 * 1024, cli_uart_session_task, NULL);
    furi_thread_start(cli_uart_thread);
}

void cli_uart_bridge_stop() {
    if(!cli_uart_thread) return;
    /* The task loop exits once momentum_settings.qflipper_enabled is false
     * (cleared by the qFlipper app before this call). Join and free. */
    furi_thread_join(cli_uart_thread);
    furi_thread_free(cli_uart_thread);
    cli_uart_thread = NULL;
}
