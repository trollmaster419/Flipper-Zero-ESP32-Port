/**
 * @file rpc_system.c
 * RPC System commands — ESP32 port
 *
 * Power/reboot/factory_reset/update stubbed out.
 */

#include <flipper.pb.h>
#include <furi_hal.h>
#include <notification/notification_messages.h>
#include <protobuf_version.h>

#include "rpc_i.h"

#define TAG "RpcSystem"

typedef struct {
    RpcSession* session;
    PB_Main* response;
} RpcSystemContext;

static void rpc_system_system_ping_process(const PB_Main* request, void* context) {
    furi_assert(request);
    furi_assert(request->which_content == PB_Main_system_ping_request_tag);

    // FURI_LOG_D(TAG, "Ping"); // Disabled to prevent serial TX stream corruption

    RpcSession* session = (RpcSession*)context;
    furi_assert(session);

    if(request->has_next) {
        rpc_send_and_release_empty(
            session, request->command_id, PB_CommandStatus_ERROR_INVALID_PARAMETERS);
        return;
    }

    PB_Main response = PB_Main_init_default;
    response.command_status = PB_CommandStatus_OK;
    response.command_id = request->command_id;
    response.which_content = PB_Main_system_ping_response_tag;

    const PB_System_PingRequest* ping_request = &request->content.system_ping_request;
    PB_System_PingResponse* ping_response = &response.content.system_ping_response;
    if(ping_request->data && (ping_request->data->size > 0)) {
        ping_response->data = malloc(PB_BYTES_ARRAY_T_ALLOCSIZE(ping_request->data->size));
        memcpy(ping_response->data->bytes, ping_request->data->bytes, ping_request->data->size);
        ping_response->data->size = ping_request->data->size;
    }

    rpc_send_and_release(session, &response);
}

static void rpc_system_system_reboot_process(const PB_Main* request, void* context) {
    furi_assert(request);
    RpcSession* session = (RpcSession*)context;
    // FURI_LOG_W(TAG, "Reboot not implemented on ESP32"); // Disabled to prevent serial TX stream corruption
    rpc_send_and_release_empty(
        session, request->command_id, PB_CommandStatus_ERROR_NOT_IMPLEMENTED);
}

static void rpc_system_system_device_info_callback(
    const char* key,
    const char* value,
    bool last,
    void* context) {
    furi_assert(key);
    furi_assert(value);
    RpcSystemContext* ctx = context;
    furi_assert(ctx);

    char* str_key = strdup(key);
    char* str_value = strdup(value);

    ctx->response->has_next = !last;
    ctx->response->content.system_device_info_response.key = str_key;
    ctx->response->content.system_device_info_response.value = str_value;

    rpc_send_and_release(ctx->session, ctx->response);
}

static void rpc_system_system_device_info_process(const PB_Main* request, void* context) {
    furi_assert(request);
    furi_assert(request->which_content == PB_Main_system_device_info_request_tag);

    // FURI_LOG_D(TAG, "DeviceInfo"); // Disabled to prevent serial TX stream corruption

    RpcSession* session = (RpcSession*)context;
    furi_assert(session);

    PB_Main* response = malloc(sizeof(PB_Main));
    response->command_id = request->command_id;
    response->which_content = PB_Main_system_device_info_response_tag;
    response->command_status = PB_CommandStatus_OK;

    RpcSystemContext device_info_context = {
        .session = session,
        .response = response,
    };
    furi_hal_info_get(rpc_system_system_device_info_callback, '_', &device_info_context);

    free(response);
}

static void rpc_system_system_get_datetime_process(const PB_Main* request, void* context) {
    furi_assert(request);
    furi_assert(request->which_content == PB_Main_system_get_datetime_request_tag);

    // FURI_LOG_D(TAG, "GetDatetime"); // Disabled to prevent serial TX stream corruption

    RpcSession* session = (RpcSession*)context;
    furi_assert(session);

    DateTime datetime;
    furi_hal_rtc_get_datetime(&datetime);

    PB_Main* response = malloc(sizeof(PB_Main));
    response->command_id = request->command_id;
    response->which_content = PB_Main_system_get_datetime_response_tag;
    response->command_status = PB_CommandStatus_OK;
    response->content.system_get_datetime_response.has_datetime = true;
    response->content.system_get_datetime_response.datetime.hour = datetime.hour;
    response->content.system_get_datetime_response.datetime.minute = datetime.minute;
    response->content.system_get_datetime_response.datetime.second = datetime.second;
    response->content.system_get_datetime_response.datetime.day = datetime.day;
    response->content.system_get_datetime_response.datetime.month = datetime.month;
    response->content.system_get_datetime_response.datetime.year = datetime.year;
    response->content.system_get_datetime_response.datetime.weekday = datetime.weekday;

    rpc_send_and_release(session, response);
    free(response);
}

static void rpc_system_system_set_datetime_process(const PB_Main* request, void* context) {
    furi_assert(request);
    RpcSession* session = (RpcSession*)context;

    if(!request->content.system_set_datetime_request.has_datetime) {
        rpc_send_and_release_empty(
            session, request->command_id, PB_CommandStatus_ERROR_INVALID_PARAMETERS);
        return;
    }

    DateTime datetime;
    datetime.hour = request->content.system_set_datetime_request.datetime.hour;
    datetime.minute = request->content.system_set_datetime_request.datetime.minute;
    datetime.second = request->content.system_set_datetime_request.datetime.second;
    datetime.day = request->content.system_set_datetime_request.datetime.day;
    datetime.month = request->content.system_set_datetime_request.datetime.month;
    datetime.year = request->content.system_set_datetime_request.datetime.year;
    datetime.weekday = request->content.system_set_datetime_request.datetime.weekday;
    furi_hal_rtc_set_datetime(&datetime);

    rpc_send_and_release_empty(session, request->command_id, PB_CommandStatus_OK);
}

static void rpc_system_system_power_info_callback(
    const char* key,
    const char* value,
    bool last,
    void* context) {
    furi_assert(key);
    furi_assert(value);
    RpcSystemContext* ctx = context;
    furi_assert(ctx);

    char* str_key = strdup(key);
    char* str_value = strdup(value);

    ctx->response->has_next = !last;
    ctx->response->content.system_device_info_response.key = str_key;
    ctx->response->content.system_device_info_response.value = str_value;

    rpc_send_and_release(ctx->session, ctx->response);
}

static void rpc_system_system_get_power_info_process(const PB_Main* request, void* context) {
    furi_assert(request);
    furi_assert(request->which_content == PB_Main_system_power_info_request_tag);

    // FURI_LOG_D(TAG, "GetPowerInfo"); // Disabled to prevent serial TX stream corruption

    RpcSession* session = (RpcSession*)context;
    furi_assert(session);

    PB_Main* response = malloc(sizeof(PB_Main));
    response->command_id = request->command_id;
    response->which_content = PB_Main_system_power_info_response_tag;
    response->command_status = PB_CommandStatus_OK;

    RpcSystemContext power_info_context = {
        .session = session,
        .response = response,
    };
    furi_hal_power_info_get(rpc_system_system_power_info_callback, '_', &power_info_context);

    free(response);
}

static void rpc_system_system_protobuf_version_process(const PB_Main* request, void* context) {
    furi_assert(request);
    furi_assert(request->which_content == PB_Main_system_protobuf_version_request_tag);

    // FURI_LOG_D(TAG, "ProtobufVersion"); // Disabled to prevent serial TX stream corruption

    RpcSession* session = (RpcSession*)context;
    furi_assert(session);

    PB_Main* response = malloc(sizeof(PB_Main));
    response->command_id = request->command_id;
    response->has_next = false;
    response->command_status = PB_CommandStatus_OK;
    response->which_content = PB_Main_system_protobuf_version_response_tag;
    response->content.system_protobuf_version_response.major = PROTOBUF_MAJOR_VERSION;
    response->content.system_protobuf_version_response.minor = PROTOBUF_MINOR_VERSION;

    rpc_send_and_release(session, response);
    free(response);
}

static void rpc_system_property_get_callback(
    const char* key,
    const char* value,
    bool last,
    void* context) {
    furi_assert(key);
    furi_assert(value);
    RpcSystemContext* ctx = context;
    furi_assert(ctx);

    ctx->response->has_next = !last;
    ctx->response->content.property_get_response.key = strdup(key);
    ctx->response->content.property_get_response.value = strdup(value);

    rpc_send_and_release(ctx->session, ctx->response);
}

static void rpc_system_property_get_process(const PB_Main* request, void* context) {
    furi_assert(request);
    furi_assert(request->which_content == PB_Main_property_get_request_tag);

    RpcSession* session = (RpcSession*)context;
    furi_assert(session);

    /* The request key selects a property category (qFlipper sends "devinfo"),
     * not a literal key prefix — the emitted keys are dotted (hardware.name,
     * firmware.version, ...). Reuse the same providers as system_device_info /
     * system_power_info but with the '.' separator qFlipper expects. */
    const char* category = request->content.property_get_request.key;

    PB_Main* response = malloc(sizeof(PB_Main));
    response->command_id = request->command_id;
    response->which_content = PB_Main_property_get_response_tag;
    response->command_status = PB_CommandStatus_OK;

    RpcSystemContext property_context = {
        .session = session,
        .response = response,
    };

    if(category && (strcmp(category, "pwrinfo") == 0)) {
        furi_hal_power_info_get(rpc_system_property_get_callback, '.', &property_context);
    } else {
        /* Default (and "devinfo"): full device information. */
        furi_hal_info_get(rpc_system_property_get_callback, '.', &property_context);
    }

    free(response);
}

void* rpc_system_system_alloc(RpcSession* session) {
    furi_assert(session);

    RpcHandler rpc_handler = {
        .message_handler = NULL,
        .decode_submessage = NULL,
        .context = session,
    };

    rpc_handler.message_handler = rpc_system_system_ping_process;
    rpc_add_handler(session, PB_Main_system_ping_request_tag, &rpc_handler);

    rpc_handler.message_handler = rpc_system_system_reboot_process;
    rpc_add_handler(session, PB_Main_system_reboot_request_tag, &rpc_handler);

    rpc_handler.message_handler = rpc_system_system_device_info_process;
    rpc_add_handler(session, PB_Main_system_device_info_request_tag, &rpc_handler);

    rpc_handler.message_handler = rpc_system_system_get_datetime_process;
    rpc_add_handler(session, PB_Main_system_get_datetime_request_tag, &rpc_handler);

    rpc_handler.message_handler = rpc_system_system_set_datetime_process;
    rpc_add_handler(session, PB_Main_system_set_datetime_request_tag, &rpc_handler);

    rpc_handler.message_handler = rpc_system_system_get_power_info_process;
    rpc_add_handler(session, PB_Main_system_power_info_request_tag, &rpc_handler);

    rpc_handler.message_handler = rpc_system_system_protobuf_version_process;
    rpc_add_handler(session, PB_Main_system_protobuf_version_request_tag, &rpc_handler);

    rpc_handler.message_handler = rpc_system_property_get_process;
    rpc_add_handler(session, PB_Main_property_get_request_tag, &rpc_handler);

    return NULL;
}
