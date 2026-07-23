#ifndef ARTWORK_H
#define ARTWORK_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    MEDIA_TYPE_UNKNOWN,
    MEDIA_TYPE_TV_SHOW,
    MEDIA_TYPE_MOVIE,
    MEDIA_TYPE_MUSIC
} media_type_t;

typedef struct {
    media_type_t type;
    char title[256];       
    char show_name[256];   
    int season;             
    int episode;            
    char artist[256];       
    char album[256];       
    char clean_name[256];   
} media_info_t;

void artwork_init(void);
void artwork_cleanup(void);

void artwork_detect_media(const char *title_meta, const char *artist_meta,
                          const char *album_meta, const char *filename,
                          media_info_t *out_info);

b
bool artwork_get_url(const media_info_t *info, const char *tvdb_api_key,
                     char *out_url, size_t out_max);

#endif 
