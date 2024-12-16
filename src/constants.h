#ifndef CONSTANTS_H_INCLUDED
#define CONSTANTS_H_INCLUDED

#include <stddef.h>

static const int DEVOTION_BADGE_NAME_LENGTH_MAX = 32;
static const int VICTORY_BADGE_NAME_LENGTH_MAX  = 32;
static const int TROPHIES_BADGE_NAME_LENGTH_MAX = 32;
static const int SHARK_BADGE_NAME_LENGTH_MAX    = 32;
static const int HERO_BADGE_NAME_LENGTH_MAX     = 32;


// URLs can potentially be much longer than this but with Discord message character limits we want to keep things short and sweet.
static const size_t URL_LENGTH_MAX = 512;

// FIXME: Needs to be an option and point to the HTTP_API_Server document root
static const char* HTTP_SERVER_DOC_ROOT = "www-root"; // Relative to executable

static const int DISCORD_AUTOCOMPLETE_ENTRIES_MAX = 25;

static const int DISCORD_AUTOCOMPLETE_STRING_LENGTH_MAX = 100;

#endif // CONSTANTS_H_INCLUDED
