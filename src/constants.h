#ifndef CONSTANTS_H_INCLUDED
#define CONSTANTS_H_INCLUDED

#include <stddef.h>

// URLs can potentially be much longer than this but with Discord message character limits we want to keep things short and sweet.
static const size_t URL_LENGTH_MAX = 512;

// FIXME: Needs to be an option and point to the HTTP_API_Server document root
static const char* HTTP_SERVER_DOC_ROOT = "www-root"; // Relative to executable

#endif // CONSTANTS_H_INCLUDED
