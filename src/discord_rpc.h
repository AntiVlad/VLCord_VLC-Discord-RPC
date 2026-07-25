#ifndef DISCORD_RPC_H
#define DISCORD_RPC_H

#include <stdbool.h>
#include <stdint.h>
#include <windows.h>

typedef struct {
    int type;                /* 0=Playing, 2=Listening, 3=Watching */
    char details[128];       
    char state[128];         
    char large_image[512];   
    char large_text[128];   
    char small_image[64];    
    char small_text[64];     
    int64_t start_timestamp; 
    int64_t end_timestamp;   
} discord_activity_t;

typedef struct {
    HANDLE pipe;
    bool connected;
    char client_id[64];
    uint32_t nonce;
    DWORD last_update_time;  
} discord_ipc_t;

bool discord_ipc_init(discord_ipc_t *ipc, const char *client_id);
bool discord_ipc_connect(discord_ipc_t *ipc);
void discord_ipc_disconnect(discord_ipc_t *ipc);
bool discord_ipc_set_activity(discord_ipc_t *ipc, const discord_activity_t *activity);
bool discord_ipc_clear_activity(discord_ipc_t *ipc);

#endif 
