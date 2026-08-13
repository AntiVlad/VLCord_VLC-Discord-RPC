#include "discord_rpc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <process.h>

#define IPC_OPCODE_HANDSHAKE 0
#define IPC_OPCODE_FRAME     1
#define IPC_OPCODE_CLOSE     2

#pragma pack(push, 1)
typedef struct {
    uint32_t opcode;
    uint32_t length;
} ipc_header_t;
#pragma pack(pop)

bool discord_ipc_init(discord_ipc_t *ipc, const char *client_id)
{
    if (!ipc || !client_id) return false;
    memset(ipc, 0, sizeof(*ipc));
    ipc->pipe = INVALID_HANDLE_VALUE;
    ipc->connected = false;
    snprintf(ipc->client_id, sizeof(ipc->client_id), "%s", client_id);
    ipc->nonce = 1;
    ipc->last_update_time = 0;
    return true;
}

bool discord_ipc_connect(discord_ipc_t *ipc)
{
    if (!ipc) return false;
    if (ipc->connected && ipc->pipe != INVALID_HANDLE_VALUE) return true;

    char pipe_path[128];
    for (int i = 0; i < 10; i++) {
        snprintf(pipe_path, sizeof(pipe_path), "\\\\.\\pipe\\discord-ipc-%d", i);
        ipc->pipe = CreateFileA(pipe_path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (ipc->pipe != INVALID_HANDLE_VALUE) {
            break;
        }
    }

    if (ipc->pipe == INVALID_HANDLE_VALUE) {
        ipc->connected = false;
        return false;
    }

    char payload[256];
    int payload_len = snprintf(payload, sizeof(payload), "{\"v\":1,\"client_id\":\"%s\"}", ipc->client_id);

    ipc_header_t header;
    header.opcode = IPC_OPCODE_HANDSHAKE;
    header.length = (uint32_t)payload_len;

    DWORD bytes_written = 0;
    if (!WriteFile(ipc->pipe, &header, sizeof(header), &bytes_written, NULL) ||
        !WriteFile(ipc->pipe, payload, payload_len, &bytes_written, NULL)) {
        CloseHandle(ipc->pipe);
        ipc->pipe = INVALID_HANDLE_VALUE;
        ipc->connected = false;
        return false;
    }

    ipc_header_t resp_header;
    DWORD bytes_read = 0;
    if (ReadFile(ipc->pipe, &resp_header, sizeof(resp_header), &bytes_read, NULL) && bytes_read == sizeof(resp_header)) {
        if (resp_header.length > 0) {
            char *resp_buf = (char*)malloc(resp_header.length + 1);
            if (resp_buf) {
                ReadFile(ipc->pipe, resp_buf, resp_header.length, &bytes_read, NULL);
                resp_buf[resp_header.length] = '\0';
                free(resp_buf);
            }
        }
    }

    ipc->connected = true;
    return true;
}

void discord_ipc_disconnect(discord_ipc_t *ipc)
{
    if (!ipc) return;
    if (ipc->pipe != INVALID_HANDLE_VALUE) {
        ipc_header_t header = { IPC_OPCODE_CLOSE, 0 };
        DWORD written;
        WriteFile(ipc->pipe, &header, sizeof(header), &written, NULL);
        CloseHandle(ipc->pipe);
        ipc->pipe = INVALID_HANDLE_VALUE;
    }
    ipc->connected = false;
}

static void escape_json_str(const char *in, char *out, size_t out_max)
{
    size_t j = 0;
    for (size_t i = 0; in[i] != '\0' && j + 2 < out_max; i++) {
        if (in[i] == '"' || in[i] == '\\') {
            out[j++] = '\\';
            out[j++] = in[i];
        } else if (in[i] == '\n') {
            out[j++] = '\\';
            out[j++] = 'n';
        } else if (in[i] == '\r') {
            out[j++] = '\\';
            out[j++] = 'r';
        } else {
            out[j++] = in[i];
        }
    }
    out[j] = '\0';
}

bool discord_ipc_set_activity(discord_ipc_t *ipc, const discord_activity_t *activity)
{
    if (!ipc || !activity) return false;
    if (!ipc->connected) {
        if (!discord_ipc_connect(ipc)) return false;
    }

    DWORD now = GetTickCount();
    if (now - ipc->last_update_time < 1200) {
        return true;
    }
    ipc->last_update_time = now;

    char safe_details[256], safe_state[256], safe_large_img[512], safe_large_text[256], safe_small_img[128], safe_small_text[128];
    escape_json_str(activity->details, safe_details, sizeof(safe_details));
    escape_json_str(activity->state, safe_state, sizeof(safe_state));
    escape_json_str(activity->large_image, safe_large_img, sizeof(safe_large_img));
    escape_json_str(activity->large_text, safe_large_text, sizeof(safe_large_text));
    escape_json_str(activity->small_image, safe_small_img, sizeof(safe_small_img));
    escape_json_str(activity->small_text, safe_small_text, sizeof(safe_small_text));

    char timestamps_json[256] = "";
    if (activity->start_timestamp > 0 || activity->end_timestamp > 0) {
        if (activity->start_timestamp > 0 && activity->end_timestamp > 0) {
            snprintf(timestamps_json, sizeof(timestamps_json),
                ",\"timestamps\":{\"start\":%lld,\"end\":%lld}",
                (long long)activity->start_timestamp, (long long)activity->end_timestamp);
        } else if (activity->start_timestamp > 0) {
            snprintf(timestamps_json, sizeof(timestamps_json),
                ",\"timestamps\":{\"start\":%lld}", (long long)activity->start_timestamp);
        }
    }

    char buttons_json[1024] = "";
    if (activity->button_count > 0) {
        char tmp[1024];
        int bpos = 0;
        bpos += snprintf(tmp + bpos, sizeof(tmp) - bpos, ",\"buttons\":[");
        for (int i = 0; i < activity->button_count && i < 2; i++) {
            char safe_label[64], safe_url[512];
            escape_json_str(activity->buttons[i].label, safe_label, sizeof(safe_label));
            escape_json_str(activity->buttons[i].url, safe_url, sizeof(safe_url));
            if (i > 0) bpos += snprintf(tmp + bpos, sizeof(tmp) - bpos, ",");
            bpos += snprintf(tmp + bpos, sizeof(tmp) - bpos,
                "{\"label\":\"%s\",\"url\":\"%s\"}", safe_label, safe_url);
        }
        bpos += snprintf(tmp + bpos, sizeof(tmp) - bpos, "]");
        snprintf(buttons_json, sizeof(buttons_json), "%s", tmp);
    }

    char payload[4096];
    int len = snprintf(payload, sizeof(payload),
        "{"
            "\"cmd\":\"SET_ACTIVITY\","
            "\"args\":{"
                "\"pid\":%lu,"
                "\"activity\":{"
                    "\"type\":%d,"
                    "\"details\":\"%s\","
                    "\"state\":\"%s\""
                    "%s,"
                    "\"assets\":{"
                        "\"large_image\":\"%s\","
                        "\"large_text\":\"%s\","
                        "\"small_image\":\"%s\","
                        "\"small_text\":\"%s\""
                    "}"
                    "%s"
                "}"
            "},"
            "\"nonce\":\"%u\""
        "}",
        GetCurrentProcessId(),
        activity->type,
        safe_details,
        safe_state,
        timestamps_json,
        safe_large_img,
        safe_large_text,
        safe_small_img,
        safe_small_text,
        buttons_json,
        ipc->nonce++
    );

    ipc_header_t header = { IPC_OPCODE_FRAME, (uint32_t)len };
    DWORD written;
    if (!WriteFile(ipc->pipe, &header, sizeof(header), &written, NULL) ||
        !WriteFile(ipc->pipe, payload, len, &written, NULL)) {
        discord_ipc_disconnect(ipc);
        return false;
    }

    return true;
}

bool discord_ipc_clear_activity(discord_ipc_t *ipc)
{
    if (!ipc || !ipc->connected) return false;

    char payload[512];
    int len = snprintf(payload, sizeof(payload),
        "{"
            "\"cmd\":\"SET_ACTIVITY\","
            "\"args\":{"
                "\"pid\":%lu,"
                "\"activity\":null"
            "},"
            "\"nonce\":\"%u\""
        "}",
        GetCurrentProcessId(),
        ipc->nonce++
    );

    ipc_header_t header = { IPC_OPCODE_FRAME, (uint32_t)len };
    DWORD written;
    if (!WriteFile(ipc->pipe, &header, sizeof(header), &written, NULL) ||
        !WriteFile(ipc->pipe, payload, len, &written, NULL)) {
        discord_ipc_disconnect(ipc);
        return false;
    }

    return true;
}
