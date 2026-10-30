/* The game's CD: an ISO 9660 image (GOG's DATA.DAT, a single data track of 2048-byte sectors),
 * read-only, for drive D: in dos.cpp. WC_CD=file names another image; by default it is DATA.DAT in
 * the game folder. Only the primary volume descriptor's names (8.3, ";1" versions dropped). */
#include <cctype>
#include <fcntl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <unistd.h>

#include "iso.h"

enum { SECTOR = 2048 };

static int fd = -2; /* -2: not opened yet, -1: no image */
static struct iso_entry root;

/* A directory record into an entry (name without ";1", upper case) */
static void parse_record(const uint8_t *r, struct iso_entry *e)
{
	memcpy(&e->offset, r + 2, 4);
	e->offset = (uint64_t)(uint32_t)e->offset * SECTOR;
	memcpy(&e->size, r + 10, 4);
	e->dir = (r[25] & 2) != 0;
	memcpy(e->date, r + 18, 6);
	size_t n = r[32];
	if (n > sizeof e->name - 1) n = sizeof e->name - 1;
	memcpy(e->name, r + 33, n);
	e->name[n] = 0;
	char *v = strchr(e->name, ';');
	if (v) *v = 0;
	size_t len = strlen(e->name);
	if (len && e->name[len - 1] == '.') e->name[len - 1] = 0; /* "NAME." for names without extension */
	for (char *c = e->name; *c; c++) *c = (char)toupper((unsigned char)*c);
}

int iso_fd(const char *game_dir)
{
	if (fd != -2) return fd;
	fd = -1;
	char path[1024];
	const char *env = getenv("WC_CD");
	snprintf(path, sizeof path, "%s/DATA.DAT", game_dir);
	int f = open(env ? env : path, O_RDONLY);
	uint8_t pvd[SECTOR];
	if (f < 0 || pread(f, pvd, SECTOR, 16 * SECTOR) != SECTOR || pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5)) {
		fprintf(stderr, "wc: no CD image (%s is not ISO 9660)\n", env ? env : path);
		if (f >= 0) close(f);
		return fd;
	}
	parse_record(pvd + 156, &root);
	root.name[0] = 0;
	fd = f;
	return fd;
}

int iso_entry_at(const struct iso_entry *dir, uint32_t index, struct iso_entry *e)
{
	uint8_t sector[SECTOR];
	uint32_t n = 0;
	for (uint32_t s = 0; s * SECTOR < dir->size; s++) {
		if (pread(fd, sector, SECTOR, (off_t)(dir->offset + (uint64_t)s * SECTOR)) != SECTOR) return 0;
		for (uint32_t p = 0; p < SECTOR && sector[p];) { /* records don't cross sectors: 0 pads the rest */
			const uint8_t *r = sector + p;
			p += r[0];
			if (r[32] == 1 && r[33] <= 1) continue; /* "." and ".." */
			if (n++ == index) {
				parse_record(r, e);
				return 1;
			}
		}
	}
	return 0;
}

int iso_lookup(const char *path, struct iso_entry *e)
{
	if (fd < 0) return 0;
	*e = root;
	while (*path) {
		while (*path == '\\' || *path == '/') path++;
		const char *start = path;
		while (*path && *path != '\\' && *path != '/') path++;
		if (path == start) break;
		if (!e->dir) return 0;
		char part[64];
		snprintf(part, sizeof part, "%.*s", (int)(path - start), start);
		struct iso_entry dir = *e, c;
		int found = 0;
		for (uint32_t i = 0; iso_entry_at(&dir, i, &c); i++) {
			if (!strcasecmp(c.name, part)) {
				*e = c;
				found = 1;
				break;
			}
		}
		if (!found) return 0;
	}
	return 1;
}
