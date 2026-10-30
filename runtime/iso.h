/* The game's CD image (iso.cpp), read-only. */
#pragma once
#include <cstdint>

struct iso_entry {
	uint64_t offset; /* of its data in the image */
	uint32_t size;
	int dir;
	uint8_t date[6]; /* years since 1900, month, day, hour, minute, second */
	char name[64];   /* upper case, without the ";1" version */
};

/* The image's file descriptor (opened on the first call), -1 if there is none. */
int iso_fd(const char *game_dir);
/* The entry at a path (no drive, backslashes or slashes, "" for the root): 1 if found. */
int iso_lookup(const char *path, struct iso_entry *e);
/* The index-th entry of a directory, without "." and "..": 1 if there is one. */
int iso_entry_at(const struct iso_entry *dir, uint32_t index, struct iso_entry *e);
