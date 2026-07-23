#ifndef PRESENCE_H
#define PRESENCE_H

#include "discord_rpc.h"
#include "artwork.h"
#include <stdbool.h>

typedef enum {
    PLAYSTATE_STOPPED,
    PLAYSTATE_PLAYING,
    PLAYSTATE_PAUSED
} playstate_t;

typedef struct {
    playstate_t state;
    media_info_t media;
    int64_t position_ms;
    int64_t duration_ms;
    char artwork_url[512];
} presence_state_t;

void presence_update(discord_ipc_t *ipc, const presence_state_t *state);
void presence_clear(discord_ipc_t *ipc);

#endif 
