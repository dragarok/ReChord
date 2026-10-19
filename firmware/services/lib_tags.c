/*
 * lib_tags.c -- the library's tag reader: services/tags.c built again with TAGS_EXT (album artist,
 * genre, disc, year, duration; WMA, DSF, AIFF; 255-byte strings), into the library overlay. The
 * resident build of tags.c (the player's: title, artist, album, track) is unchanged.
 */
#define TAGS_EXT 1
#include "tags.c"
