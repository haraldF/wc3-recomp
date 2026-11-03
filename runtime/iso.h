/* The game's CD image (iso.cpp), read-only. */
#pragma once
#include <cstddef>
#include <cstdint>

struct iso_entry {
	uint64_t offset; /* of its data in the image */
	uint32_t size;
	int dir;
	uint8_t date[6]; /* years since 1900, month, day, hour, minute, second */
	char name[64];   /* upper case, without the ";1" version */
};

/* Whether there is an image (opened on the first call). */
int iso_present(const char *game_dir);
/* Up to n bytes of the image at offset into buf: the count read. */
size_t iso_read(void *buf, size_t n, uint64_t offset);
/* The entry at a path (no drive, backslashes or slashes, "" for the root): 1 if found. */
int iso_lookup(const char *path, struct iso_entry *e);
/* The index-th entry of a directory, without "." and "..": 1 if there is one. */
int iso_entry_at(const struct iso_entry *dir, uint32_t index, struct iso_entry *e);
