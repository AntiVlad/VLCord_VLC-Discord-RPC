#include "presence.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

void presence_update(discord_ipc_t *ipc, const presence_state_t *state)
{
    if (!ipc || !state) return;

    if (state->state == PLAYSTATE_STOPPED) {
        discord_ipc_clear_activity(ipc);
        return;
    }

    discord_activity_t act;
    memset(&act, 0, sizeof(act));

    if (state->media.type == MEDIA_TYPE_MUSIC) {
        act.type = 2; // Listening
        snprintf(act.details, sizeof(act.details), "%s", state->media.title);
        snprintf(act.state, sizeof(act.state), "%s — %s%s", state->media.artist, state->media.album,
                 state->state == PLAYSTATE_PAUSED ? " (Paused)" : "");
    } else if (state->media.type == MEDIA_TYPE_TV_SHOW) {
        act.type = 3; // Watching
        snprintf(act.details, sizeof(act.details), "%s", state->media.show_name);
        snprintf(act.state, sizeof(act.state), "%s%s", state->media.title,
                 state->state == PLAYSTATE_PAUSED ? " (Paused)" : "");
    } else if (state->media.type == MEDIA_TYPE_ANIME) {
        act.type = 3; // Watching
        const char *anime_name = state->media.show_name[0]
                                 ? state->media.show_name
                                 : state->media.title;
        snprintf(act.details, sizeof(act.details), "%s", anime_name);
        snprintf(act.state, sizeof(act.state), "Watching Anime%s",
                 state->state == PLAYSTATE_PAUSED ? " (Paused)" : "");
    } else {
        act.type = 3; // Watching
        snprintf(act.details, sizeof(act.details), "%s", state->media.title);
        snprintf(act.state, sizeof(act.state), "Watching%s",
                 state->state == PLAYSTATE_PAUSED ? " (Paused)" : "");
    }

    if (state->artwork_url[0] != '\0') {
        snprintf(act.large_image, sizeof(act.large_image), "%s", state->artwork_url);
    } else {
        snprintf(act.large_image, sizeof(act.large_image), "vlc_logo");
    }

    if (state->media.type == MEDIA_TYPE_TV_SHOW || state->media.type == MEDIA_TYPE_ANIME) {
        snprintf(act.large_text, sizeof(act.large_text), "%s",
                 state->media.show_name[0] ? state->media.show_name : state->media.clean_name);
    } else if (state->media.type == MEDIA_TYPE_MUSIC) {
        snprintf(act.large_text, sizeof(act.large_text), "%s - %s",
                 state->media.artist, state->media.album);
    } else {
        snprintf(act.large_text, sizeof(act.large_text), "%s",
                 state->media.title[0] ? state->media.title : "VLC Media Player");
    }

    if (state->state == PLAYSTATE_PLAYING) {
        snprintf(act.small_image, sizeof(act.small_image), "playing");
        if (state->media.type == MEDIA_TYPE_MUSIC) {
            snprintf(act.small_text, sizeof(act.small_text), "Listening");
        } else {
            snprintf(act.small_text, sizeof(act.small_text), "Watching");
        }
    } else {
        snprintf(act.small_image, sizeof(act.small_image), "paused");
        snprintf(act.small_text, sizeof(act.small_text), "Paused");
    }

    if (state->state == PLAYSTATE_PLAYING && state->duration_ms > 0) {
        int64_t now_sec = (int64_t)time(NULL);
        int64_t elapsed_sec = state->position_ms / 1000;
        int64_t remaining_sec = (state->duration_ms / 1000) - elapsed_sec;

        act.start_timestamp = now_sec - elapsed_sec;
        if (remaining_sec > 0) {
            act.end_timestamp = now_sec + remaining_sec;
        }
    }

    /* Activity button linking to the repository */
    snprintf(act.buttons[0].label, sizeof(act.buttons[0].label), "Get Vlcord");
    snprintf(act.buttons[0].url, sizeof(act.buttons[0].url),
             "https://github.com/AntiVlad/Vlcord_VLC-Discord-RPC");
    act.button_count = 1;

    discord_ipc_set_activity(ipc, &act);
}

void presence_clear(discord_ipc_t *ipc)
{
    if (ipc) {
        discord_ipc_clear_activity(ipc);
    }
}
