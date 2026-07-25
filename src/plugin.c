#include "vlc_compat.h"
#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_interface.h>
#include <vlc_playlist.h>
#include <vlc_input.h>

#include "discord_rpc.h"
#include "artwork.h"
#include "presence.h"

#include <windows.h>

struct intf_sys_t {
    vlc_mutex_t lock;
    vlc_thread_t thread;
    bool b_die;

    discord_ipc_t ipc;
    presence_state_t presence;

    char app_id[64];
    char tvdb_key[128];
};

static int Open(vlc_object_t *p_this);
static void Close(vlc_object_t *p_this);
static void *Run(void *data);

vlc_module_begin()
    set_shortname(N_("Discord RPC"))
    set_description(N_("Discord Rich Presence Integration with Dynamic Cover Art"))
    set_category(CAT_INTERFACE)
    set_subcategory(SUBCAT_INTERFACE_CONTROL)
    set_capability("interface", 0)
    set_callbacks(Open, Close)

    add_string("discord-rpc-appid", "1529779460042395799", N_("Discord App ID"), N_("Discord Application ID from Developer Portal"), false)
    add_string("discord-rpc-tvdb-key", "ae249700-e895-47c6-9f9d-c5ed23e1619f", N_("TVDB API Key"), N_("TheTVDB API key for TV and Movie poster lookup"), false)
vlc_module_end()

VLC_META_EXPORT(copyright, VLC_COPYRIGHT_VIDEOLAN)
VLC_META_EXPORT(license, VLC_LICENSE_GPL_2_PLUS)

static int Open(vlc_object_t *p_this)
{
    intf_thread_t *p_intf = (intf_thread_t *)p_this;
    intf_sys_t *p_sys = (intf_sys_t *)malloc(sizeof(intf_sys_t));
    if (!p_sys) return VLC_ENOMEM;

    memset(p_sys, 0, sizeof(*p_sys));
    p_intf->p_sys = p_sys;

    vlc_mutex_init(&p_sys->lock);
    artwork_init();

    char *psz_appid = var_InheritString(p_intf, "discord-rpc-appid");
    char *psz_tvdb = var_InheritString(p_intf, "discord-rpc-tvdb-key");

    snprintf(p_sys->app_id, sizeof(p_sys->app_id), "%s", (psz_appid && strlen(psz_appid) > 0) ? psz_appid : "383226320970055681");
    snprintf(p_sys->tvdb_key, sizeof(p_sys->tvdb_key), "%s", psz_tvdb ? psz_tvdb : "");

    vlc_free_string(psz_appid);
    vlc_free_string(psz_tvdb);

    discord_ipc_init(&p_sys->ipc, p_sys->app_id);

    p_sys->b_die = false;
    if (vlc_clone(&p_sys->thread, Run, p_intf, VLC_THREAD_PRIORITY_LOW)) {
        vlc_mutex_destroy(&p_sys->lock);
        artwork_cleanup();
        free(p_sys);
        return VLC_EGENERIC;
    }

    return VLC_SUCCESS;
}

static void Close(vlc_object_t *p_this)
{
    intf_thread_t *p_intf = (intf_thread_t *)p_this;
    intf_sys_t *p_sys = p_intf->p_sys;

    if (!p_sys) return;

    vlc_mutex_lock(&p_sys->lock);
    p_sys->b_die = true;
    vlc_mutex_unlock(&p_sys->lock);

    vlc_join(p_sys->thread, NULL);

    discord_ipc_clear_activity(&p_sys->ipc);
    discord_ipc_disconnect(&p_sys->ipc);

    vlc_mutex_destroy(&p_sys->lock);
    artwork_cleanup();
    free(p_sys);
    p_intf->p_sys = NULL;
}

static void *Run(void *data)
{
    intf_thread_t *p_intf = (intf_thread_t *)data;
    intf_sys_t *p_sys = p_intf->p_sys;

    while (1) {
        vlc_mutex_lock(&p_sys->lock);
        if (p_sys->b_die) {
            vlc_mutex_unlock(&p_sys->lock);
            break;
        }

        playlist_t *p_playlist = pl_Get(p_intf);
        input_thread_t *p_input = p_playlist ? playlist_CurrentInput(p_playlist) : NULL;

        if (p_input) {
            input_item_t *p_item = input_GetItem(p_input);
            if (p_item) {
                char *psz_title = input_item_GetMeta(p_item, vlc_meta_Title);
                char *psz_artist = input_item_GetMeta(p_item, vlc_meta_Artist);
                char *psz_album = input_item_GetMeta(p_item, vlc_meta_Album);
                char *psz_nowplaying = input_item_GetMeta(p_item, vlc_meta_NowPlaying);
                char *psz_name = input_item_GetName(p_item);

                const char *title_use = psz_nowplaying ? psz_nowplaying : (psz_title ? psz_title : psz_name);

                media_info_t info;
                artwork_detect_media(title_use, psz_artist, psz_album, psz_name, &info);

                int i_state = var_GetInteger(p_input, "state");
                p_sys->presence.state = (i_state == PAUSE_S) ? PLAYSTATE_PAUSED : PLAYSTATE_PLAYING;
                p_sys->presence.media = info;

                // Time and duration
                mtime_t i_time = var_GetInteger(p_input, "time");
                mtime_t i_length = input_item_GetDuration(p_item);
                p_sys->presence.position_ms = i_time / 1000;
                p_sys->presence.duration_ms = i_length / 1000;

                // Fetch Artwork
                artwork_get_url(&info, p_sys->tvdb_key, p_sys->presence.artwork_url, sizeof(p_sys->presence.artwork_url));

                vlc_free_string(psz_title);
                vlc_free_string(psz_artist);
                vlc_free_string(psz_album);
                vlc_free_string(psz_nowplaying);
                vlc_free_string(psz_name);
            }
            vlc_object_release(p_input);
        } else {
            p_sys->presence.state = PLAYSTATE_STOPPED;
        }

        presence_update(&p_sys->ipc, &p_sys->presence);
        vlc_mutex_unlock(&p_sys->lock);

        msleep(1500000);
    }

    return NULL;
}
