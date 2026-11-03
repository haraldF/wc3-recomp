/* Software interrupts: a small DOS (INT 21h) on top of the host file system, plus BIOS stubs.
 * Drive C: is the game folder, D: the game's CD (iso.cpp, read-only, and MSCDEX on INT 2Fh); the
 * program starts on D:, as GOG's DOSBox starts it. Anything not implemented stops the program with
 * the function number, so the next piece of work is obvious. */
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <system_error>

#include "host.h"
#include "iso.h"
#include "wc_rt.h"

void cpu_report(FILE *f);
void wc_dump_memory(const char *why);

const char *wc_game_dir = ".";
uint16_t wc_psp_seg;

namespace fs = std::filesystem;

/* DOS's file handles: the console's three (standard input, output, error; 3 and 4, AUX and PRN, are
 * closed), files in the game folder, and files on the CD (where they are in the image, their size
 * and position). A file's host path and whether it was changed are for host_file_saved() on close. */
enum { MAX_HANDLES = 64, FIRST_FILE = 5 };
enum handle_kind { FREE, CON_IN, CON_OUT, CON_ERR, HOST_FILE, CD_FILE };
static struct handle {
	handle_kind kind = FREE;
	FILE *f = NULL;
	std::string path = "";
	uint8_t changed = 0;
	uint64_t start = 0;
	uint32_t size = 0, pos = 0;
} handles[MAX_HANDLES] = { { .kind = CON_IN }, { .kind = CON_OUT }, { .kind = CON_ERR } };
static uint32_t dta;
enum { DRIVE_C = 2, DRIVE_CD = 3 };
static int cur_drive = DRIVE_CD;
static char cwds[26][128]; /* each drive's current directory, DOS style, no leading backslash */

static int cd_present(void)
{
	return iso_present(wc_game_dir);
}

/* Names compared without case (ASCII). */
static int same_name(const char *a, const char *b)
{
	for (; *a && *b; a++, b++) {
		if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return 0;
	}
	return *a == *b;
}

static void fail_dos(int code)
{
	wr(R_AX, 2, code);
	set_cf(1);
}

static void ok(void)
{
	set_cf(0);
}

/* flat32: the program calls DOS in protected mode (flat32.cpp), where pointers are a selector and a
 * 32-bit offset (DS:EDX), counts and results 32 bits (ECX, EAX) */
int dos_pm;

static uint32_t ds_dx(void)
{
	return dos_pm ? wc_sb[3] + (uint32_t)rd(R_DX, 4) : (uint32_t)lin(DS, DX);
}

static uint32_t ds_si(void)
{
	return dos_pm ? wc_sb[3] + (uint32_t)rd(R_SI, 4) : (uint32_t)lin(DS, rd(R_SI, 2));
}

static uint32_t count_cx(void)
{
	return dos_pm ? (uint32_t)rd(R_CX, 4) : (uint32_t)CX;
}

static void set_ax(uint64_t v)
{
	wr(R_AX, dos_pm ? 4 : 2, v);
}

/* a value at a linear address, without the 1 MB wrap of mwr (the DTA can be above it in flat32) */
static void put(uint32_t a, int size, uint32_t v)
{
	memcpy(M + a, &v, size);
}

/* Read a NUL-terminated string at a linear address (DOS's DS:DX, in protected mode too). */
static void get_str(uint32_t addr, char *buf, size_t size)
{
	size_t i = 0;
	for (; i + 1 < size; i++) {
		buf[i] = (char)M[addr + i];
		if (!buf[i]) {
			break;
		}
	}
	buf[i] = 0;
}

/* Absolute DOS path (no drive, no leading backslash, no "." or "..") from a path relative to its
 * drive's current directory; returns the drive (0 = A:). */
static int dos_absolute(const char *dos, char *out, size_t size)
{
	char joined[256];
	int drive = cur_drive;
	if (dos[0] && dos[1] == ':') {
		drive = toupper((unsigned char)dos[0]) - 'A';
		if (drive < 0 || drive >= 26) drive = cur_drive;
		dos += 2;
	}
	if (dos[0] == '\\' || dos[0] == '/') {
		snprintf(joined, sizeof joined, "%s", dos);
	}
	else {
		snprintf(joined, sizeof joined, "%s\\%s", cwds[drive], dos);
	}
	out[0] = 0;
	for (char *part = strtok(joined, "\\/"); part; part = strtok(NULL, "\\/")) {
		if (!strcmp(part, ".")) {
			continue;
		}
		if (!strcmp(part, "..")) {
			char *slash = strrchr(out, '\\');
			*(slash ? slash : out) = 0;
			continue;
		}
		size_t len = strlen(out);
		snprintf(out + len, size - len, "%s%s", len ? "\\" : "", part);
	}
	return drive;
}

/* Whether a DOS path is on the CD; abs gets its path there. */
static int on_cd(const char *dos, char *abs, size_t size)
{
	return dos_absolute(dos, abs, size) == DRIVE_CD;
}

/* DOS path -> host path under the game folder, matching each component case-insensitively. */
static void host_path(const char *rel, char *out, size_t size)
{
	char dos[256];
	dos_absolute(rel, dos, sizeof dos);
	snprintf(out, size, "%s", wc_game_dir);
	const char *p = dos;
	while (*p) {
		while (*p == '\\' || *p == '/') {
			p++;
		}
		const char *start = p;
		while (*p && *p != '\\' && *p != '/') {
			p++;
		}
		if (p == start) {
			break;
		}
		char part[256];
		snprintf(part, sizeof part, "%.*s", (int)(p - start), start);
		if (!strcmp(part, ".")) {
			continue;
		}
		std::error_code ec;
		for (const auto &e : fs::directory_iterator(out, ec)) {
			std::string name = e.path().filename().string();
			if (same_name(name.c_str(), part)) {
				snprintf(part, sizeof part, "%s", name.c_str());
				break;
			}
		}
		size_t len = strlen(out);
		snprintf(out + len, size - len, "/%s", part);
	}
}

/* Whether the game folder has this file (a DOS path) */
int dos_file_exists(const char *dos)
{
	char path[1024];
	host_path(dos, path, sizeof path);
	std::error_code ec;
	return fs::exists(path, ec);
}

/* DOS's error code for a host error. */
static int dos_error(std::error_code ec)
{
	if (ec == std::errc::no_such_file_or_directory) return 2;
	if (ec == std::errc::not_a_directory) return 3;
	if (ec == std::errc::too_many_files_open) return 4;
	if (ec == std::errc::permission_denied || ec == std::errc::operation_not_permitted || ec == std::errc::file_exists ||
	    ec == std::errc::directory_not_empty || ec == std::errc::is_a_directory) {
		return 5;
	}
	return 0x1f;
}

static std::error_code error(std::errc e)
{
	return std::make_error_code(e);
}

/* A free handle of that kind, -1 if there is none. */
static int new_handle(handle_kind kind)
{
	for (int i = FIRST_FILE; i < MAX_HANDLES; i++) {
		if (handles[i].kind == FREE) {
			handles[i] = handle{ .kind = kind };
			return i;
		}
	}
	return -1;
}

static handle *get_handle(uint64_t h)
{
	return h < MAX_HANDLES && handles[h].kind != FREE ? &handles[h] : NULL;
}

/* How a file is opened: create (or empty) it, or open it to read, write or both. */
enum open_mode { OPEN_READ, OPEN_WRITE, OPEN_RW, CREATE };

/* A file on the CD: read-only, read from the image (iso.cpp). */
static void open_cd(const char *dos, const char *abs, open_mode mode, const char *what)
{
	struct iso_entry e;
	int found = cd_present() && iso_lookup(abs, &e);
	if (wc_trace) {
		fprintf(stderr, "wc: %s \"%s\" on the CD%s\n", what, dos, found ? "" : " (not found)");
	}
	if (!found) {
		fail_dos(2);
		return;
	}
	if (e.dir || mode != OPEN_READ) {
		fail_dos(5);
		return;
	}
	int h = new_handle(CD_FILE);
	if (h < 0) {
		fail_dos(4);
		return;
	}
	handles[h].start = e.offset;
	handles[h].size = e.size;
	set_ax(h);
	ok();
}

static void open_file(open_mode mode, const char *what)
{
	char dos[128], abs[256], path[1024];
	get_str(ds_dx(), dos, sizeof dos);
	if (on_cd(dos, abs, sizeof abs)) {
		open_cd(dos, abs, mode, what);
		return;
	}
	host_path(dos, path, sizeof path);
	std::error_code ec;
	FILE *f = NULL;
	if (!dos[0]) ec = error(std::errc::no_such_file_or_directory);
	else if (fs::is_directory(path, ec)) ec = error(std::errc::permission_denied); /* DOS can't open folders */
	else {
		/* "r+" opens to write without emptying the file, as DOS does */
		f = fopen(path, mode == CREATE ? "w+b" : mode == OPEN_READ ? "rb" : "r+b");
		ec = f ? std::error_code() : std::error_code(errno, std::generic_category());
	}
	if (wc_trace) {
		fprintf(stderr, "wc: %s \"%s\" -> %s%s", what, dos, path, !f ? " (failed)" : "");
		if (dos[0] && (unsigned char)dos[0] < 32) fprintf(stderr, " (first byte %02x)", (unsigned char)dos[0]);
		fprintf(stderr, "\n");
	}
	if (!f) {
		fail_dos(dos_error(ec));
		return;
	}
	int h = new_handle(HOST_FILE);
	if (h < 0) {
		fclose(f);
		fail_dos(4);
		return;
	}
	handles[h].f = f;
	handles[h].path = path;
	handles[h].changed = mode == CREATE;
	set_ax(h);
	ok();
}

int bios_key_peek(uint16_t *key);
uint16_t bios_key_get(uint32_t at);
void bios_key_flush(void);

/* Console input: the character of the next key; for keys without one (arrows, F1, ...) 0 first,
 * then the scancode on the next call. */
/* ---- find first/next: 8.3 names in one folder, matched like DOS against an 11-character pattern ---- */

static char find_dir[1024];
static int find_on_cd;
static struct iso_entry find_cd_dir;
static void to_fcb(const char *name, char fcb[11]);
static int fcb_match(const char *pattern, const char *fcb);

/* find_next on the CD */
static int find_next_cd(const char *pattern, uint8_t attr, uint32_t index)
{
	struct iso_entry e;
	for (uint32_t n = index; iso_entry_at(&find_cd_dir, n, &e); n++) {
		char fcb[11];
		to_fcb(e.name, fcb);
		if (!fcb_match(pattern, fcb) || (e.dir && !(attr & 0x10))) continue;
		M[dta + 0x15] = e.dir ? 0x11 : 0x21;
		put(dta + 0x16, 2, e.date[3] << 11 | e.date[4] << 5 | e.date[5] / 2);
		put(dta + 0x18, 2, (e.date[0] - 80) << 9 | e.date[1] << 5 | e.date[2]);
		put(dta + 0x1a, 4, e.size);
		int i = 0;
		for (const char *c = e.name; *c && i < 12; c++) M[dta + 0x1e + i++] = (uint8_t)*c;
		M[dta + 0x1e + i] = 0;
		put(dta + 12, 4, n + 1);
		ok();
		return 1;
	}
	fail_dos(0x12);
	return 0;
}

/* "SAVEGAME.WC2", "*.*" -> "SAVEGAMEWC2", "???????????" (upper case, '?' matches anything) */
static void to_fcb(const char *name, char fcb[11])
{
	memset(fcb, ' ', 11);
	for (int field = 0, i = 0; *name; name++) {
		if (*name == '.') {
			if (field) return;
			field = 1;
			i = 8;
			continue;
		}
		if (i >= (field ? 11 : 8)) continue;
		if (*name == '*') {
			while (i < (field ? 11 : 8)) fcb[i++] = '?';
			continue;
		}
		fcb[i++] = (char)toupper((unsigned char)*name);
	}
}

static int fcb_match(const char *pattern, const char *fcb)
{
	for (int i = 0; i < 11; i++) {
		if (pattern[i] != '?' && pattern[i] != fcb[i]) return 0;
	}
	return 1;
}

/* Fills the DTA with the next match from the index kept there (CF and error 12h when there is none). */
static void find_next(void)
{
	char pattern[11];
	memcpy(pattern, M + dta + 1, 11);
	uint8_t attr = M[dta];
	uint32_t index;
	memcpy(&index, M + dta + 12, 4);
	if (find_on_cd) {
		find_next_cd(pattern, attr, index);
		return;
	}
	std::error_code ec;
	uint32_t n = 0;
	for (auto it = fs::directory_iterator(find_dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec), n++) {
		std::string file = it->path().filename().string();
		const char *name = file.c_str();
		if (n < index || name[0] == '.') continue;
		const char *dot = strchr(name, '.');
		size_t base = dot ? (size_t)(dot - name) : strlen(name);
		if (base < 1 || base > 8 || (dot && (strlen(dot + 1) > 3 || strchr(dot + 1, '.')))) continue; /* not 8.3 */
		char fcb[11];
		to_fcb(name, fcb);
		std::error_code e2;
		bool dir = it->is_directory(e2);
		auto mtime = it->last_write_time(e2);
		if (!fcb_match(pattern, fcb) || e2) continue;
		if (dir && !(attr & 0x10)) continue;
		time_t t = std::chrono::system_clock::to_time_t(
			std::chrono::time_point_cast<std::chrono::system_clock::duration>(std::chrono::file_clock::to_sys(mtime)));
		struct tm tm = *localtime(&t);
		M[dta + 0x15] = dir ? 0x10 : 0x20;
		put(dta + 0x16, 2, tm.tm_hour << 11 | tm.tm_min << 5 | tm.tm_sec / 2);
		put(dta + 0x18, 2, (tm.tm_year - 80) << 9 | (tm.tm_mon + 1) << 5 | tm.tm_mday);
		put(dta + 0x1a, 4, dir ? 0 : (uint32_t)it->file_size(e2));
		int i = 0;
		for (const char *c = name; *c && i < 12; c++) M[dta + 0x1e + i++] = (uint8_t)toupper((unsigned char)*c);
		M[dta + 0x1e + i] = 0;
		put(dta + 12, 4, n + 1);
		ok();
		return;
	}
	fail_dos(0x12);
}

static int extended_scan = -1;

static uint8_t console_char(uint32_t at)
{
	if (extended_scan >= 0) {
		uint8_t scan = (uint8_t)extended_scan;
		extended_scan = -1;
		return scan;
	}
	uint16_t key = bios_key_get(at);
	if ((key & 0xff) == 0) {
		extended_scan = key >> 8;
	}
	return (uint8_t)key;
}

void vga_console(const uint8_t *s, size_t n);
void vga_print_text(FILE *f);

/* The console's output goes to the terminal only without a window: the window shows text mode,
 * and the game rewrites lines in place there (the joystick calibration's reading, after AH=02h),
 * which in a terminal became an endless stream. What is on the screen at the exit is printed
 * then, as DOS would leave it. */
int dos_echo = 1;

/* A character to the console: the screen in text mode, and the terminal without a window. */
static void console_out(uint8_t c)
{
	if (dos_echo) putchar(c);
	vga_console(&c, 1);
}

static void dos(uint32_t at)
{
	uint8_t ah = (uint8_t)AH;
	if (wc_trace) {
		fprintf(stderr, "wc: INT 21h AH=%02x AL=%02llx BX=%04llx CX=%04llx DX=%04llx DS=%04llx at %05x\n", ah, AL, BX,
			CX, DX, DS, at);
	}
	switch (ah) {
	case 0x02: /* write character */
		console_out((uint8_t)rd(R_DL, 1));
		break;
	case 0x09: { /* write $-terminated string */
		for (uint32_t a = ds_dx(); M[a] != '$'; a++) {
			console_out(M[a]);
		}
		break;
	}
	case 0x01: /* read a character with echo */
		wr(R_AL, 1, console_char(at));
		console_out((uint8_t)AL);
		fflush(stdout);
		break;
	case 0x06: /* direct console I/O: DL = FF reads without waiting (ZF set if none) */
		if (DL == 0xff) {
			uint16_t key;
			int have = extended_scan >= 0 || bios_key_peek(&key);
			wr(R_ZF, 1, !have);
			wr(R_AL, 1, have ? console_char(at) : 0);
		}
		else {
			console_out((uint8_t)DL);
		}
		break;
	case 0x07: case 0x08: /* read a character without echo */
		wr(R_AL, 1, console_char(at));
		break;
	case 0x0b: /* input status: FF if a key is waiting */
		{
			uint16_t key;
			wr(R_AL, 1, extended_scan >= 0 || bios_key_peek(&key) ? 0xff : 0);
		}
		break;
	case 0x0c: /* flush the input, then read with function AL */
		bios_key_flush();
		extended_scan = -1;
		if (AL == 0x01 || AL == 0x06 || AL == 0x07 || AL == 0x08) {
			wr(R_AH, 1, AL);
			dos(at);
			wr(R_AH, 1, 0x0c);
		}
		break;
	case 0x0e: /* select disk: C:, or D: with the CD */
		if (DL == DRIVE_C || (DL == DRIVE_CD && cd_present())) cur_drive = DL;
		wr(R_AL, 1, 26);
		break;
	case 0x19: /* current disk */
		wr(R_AL, 1, cur_drive);
		break;
	case 0x1a: /* set DTA */
		dta = ds_dx();
		break;
	case 0x25: /* set interrupt vector */
		mwr(AL * 4, 2, DX);
		mwr(AL * 4 + 2, 2, DS);
		break;
	case 0x2a: { /* get date */
		time_t t = time(NULL);
		struct tm *tm = localtime(&t);
		wr(R_CX, 2, tm->tm_year + 1900);
		wr(R_DH, 1, tm->tm_mon + 1);
		wr(R_DL, 1, tm->tm_mday);
		wr(R_AL, 1, tm->tm_wday);
		break;
	}
	case 0x2c: { /* get time */
		time_t t = time(NULL);
		struct tm *tm = localtime(&t);
		wr(R_CH, 1, tm->tm_hour);
		wr(R_CL, 1, tm->tm_min);
		wr(R_DH, 1, tm->tm_sec);
		wr(R_DL, 1, 0);
		break;
	}
	case 0x2f: /* get DTA */
		wr(R_ES, 2, dta >> 4);
		wr(R_BX, 2, dta & 0xf);
		break;
	case 0x30: /* DOS version: 5.0 */
		wr(R_AX, 2, 0x0005);
		wr(R_BX, 2, 0);
		wr(R_CX, 2, 0);
		break;
	case 0x33: /* Ctrl-Break checking: off */
		wr(R_DL, 1, 0);
		break;
	case 0x35: /* get interrupt vector */
		wr(R_BX, 2, mrd(AL * 4, 2));
		wr(R_ES, 2, mrd(AL * 4 + 2, 2));
		break;
	case 0x3b: { /* change directory */
		char dos[128], abs[256], path[1024];
		struct iso_entry e;
		std::error_code ec;
		get_str(ds_dx(), dos, sizeof dos);
		int drive = dos_absolute(dos, abs, sizeof abs);
		host_path(dos, path, sizeof path);
		if (drive == DRIVE_CD ? !cd_present() || !iso_lookup(abs, &e) || !e.dir : !fs::is_directory(path, ec)) {
			fail_dos(3);
			break;
		}
		snprintf(cwds[drive], sizeof cwds[drive], "%s", abs);
		if (wc_trace) {
			fprintf(stderr, "wc: chdir \"%s\" -> %c:\\%s\n", dos, 'A' + drive, cwds[drive]);
		}
		ok();
		break;
	}
	case 0x36: /* free disk space of drive DL (0: current): clusters of 8 sectors of 512 bytes */
		if ((DL ? DL - 1 : cur_drive) == DRIVE_CD) {
			wr(R_AX, 2, 4);
			wr(R_BX, 2, 0);
			wr(R_CX, 2, 512);
			wr(R_DX, 2, 0xffff);
		}
		else {
			wr(R_AX, 2, 8);
			wr(R_BX, 2, 0xfff0); /* 256 MB free */
			wr(R_CX, 2, 512);
			wr(R_DX, 2, 0xfff0);
		}
		break;
	case 0x39: case 0x3a: { /* make, remove a directory */
		char dos[128], abs[256], path[1024];
		get_str(ds_dx(), dos, sizeof dos);
		if (on_cd(dos, abs, sizeof abs)) {
			fail_dos(5);
			break;
		}
		host_path(dos, path, sizeof path);
		std::error_code ec;
		if (ah == 0x39) {
			if (!fs::create_directory(path, ec) && !ec) ec = error(std::errc::file_exists);
		}
		else {
			fs::file_status st = fs::status(path, ec);
			if (!fs::exists(st)) ec = error(std::errc::no_such_file_or_directory);
			else if (!fs::is_directory(st)) ec = error(std::errc::not_a_directory);
			else fs::remove(path, ec); /* only when empty */
		}
		if (wc_trace) {
			fprintf(stderr, "wc: %s \"%s\" -> %s%s\n", ah == 0x39 ? "mkdir" : "rmdir", dos, path, ec ? " (failed)" : "");
		}
		if (ec) {
			fail_dos(dos_error(ec));
		}
		else {
			ok();
		}
		break;
	}
	case 0x3c: /* create */
		open_file(CREATE, "create");
		break;
	case 0x3d: /* open */
		open_file((AL & 3) == 0 ? OPEN_READ : (AL & 3) == 1 ? OPEN_WRITE : OPEN_RW, "open");
		break;
	case 0x3e: { /* close */
		handle *h = get_handle(BX);
		if (!h) {
			fail_dos(6);
			break;
		}
		if (BX >= FIRST_FILE) {
			if (h->f) fclose(h->f);
			if (h->changed) host_file_saved(h->path.c_str());
			*h = handle{};
		}
		ok();
		break;
	}
	case 0x3f: { /* read */
		handle *h = get_handle(BX);
		if (!h) {
			fail_dos(6);
			break;
		}
		uint64_t n = count_cx(), base = ds_dx();
		size_t got = 0;
		if (h->kind == CD_FILE) {
			uint32_t left = h->pos < h->size ? h->size - h->pos : 0;
			got = iso_read(M + base, n < left ? n : left, h->start + h->pos);
			h->pos += (uint32_t)got;
		}
		else if (h->kind == HOST_FILE || h->kind == CON_IN) {
			FILE *f = h->kind == CON_IN ? stdin : h->f;
			fseek(f, 0, SEEK_CUR); /* between writing and reading */
			got = fread(M + base, 1, n, f);
			if (ferror(f)) {
				clearerr(f);
				fail_dos(5);
				break;
			}
		}
		else {
			fail_dos(5);
			break;
		}
		set_ax(got);
		ok();
		break;
	}
	case 0x40: { /* write */
		handle *h = get_handle(BX);
		if (!h) {
			fail_dos(6);
			break;
		}
		uint64_t n = count_cx(), base = ds_dx();
		size_t put = n;
		if (h->kind == CD_FILE) {
			fail_dos(5);
			break;
		}
		if (h->kind == HOST_FILE) {
			fseek(h->f, 0, SEEK_CUR); /* between reading and writing */
			put = fwrite(M + base, 1, n, h->f);
			fflush(h->f);
			if (put < n && ferror(h->f)) {
				clearerr(h->f);
				fail_dos(5);
				break;
			}
			h->changed = 1;
		}
		else { /* the console */
			FILE *out = h->kind == CON_ERR ? stderr : stdout;
			if (out == stderr || dos_echo) {
				fwrite(M + base, 1, n, out);
				fflush(out);
			}
			vga_console(M + base, n);
		}
		set_ax(put);
		ok();
		break;
	}
	case 0x41: { /* delete */
		char dos[128], abs[256], path[1024];
		get_str(ds_dx(), dos, sizeof dos);
		if (on_cd(dos, abs, sizeof abs)) {
			fail_dos(5);
			break;
		}
		host_path(dos, path, sizeof path);
		std::error_code ec;
		fs::file_status st = fs::symlink_status(path, ec);
		if (!fs::exists(st)) ec = error(std::errc::no_such_file_or_directory);
		else if (fs::is_directory(st)) ec = error(std::errc::permission_denied);
		else fs::remove(path, ec);
		if (ec) {
			fail_dos(dos_error(ec));
		}
		else {
			host_file_saved(path);
			ok();
		}
		break;
	}
	case 0x42: { /* seek */
		handle *h = get_handle(BX);
		if (!h) {
			fail_dos(6);
			break;
		}
		int64_t off = (int32_t)((CX << 16) | DX), pos = -1;
		if (h->kind == CD_FILE) {
			pos = off + (AL == 0 ? 0 : AL == 1 ? h->pos : h->size);
			if (pos >= 0) h->pos = (uint32_t)pos;
		}
		else if (h->kind == HOST_FILE) { /* DOS files stay below 2 GB: a long does */
			if (!fseek(h->f, (long)off, AL == 0 ? SEEK_SET : AL == 1 ? SEEK_CUR : SEEK_END)) pos = ftell(h->f);
		}
		if (pos < 0) {
			fail_dos(0x19);
			break;
		}
		set_ax(dos_pm ? (uint64_t)pos : (uint64_t)(pos & 0xffff));
		wr(R_DX, 2, (pos >> 16) & 0xffff);
		ok();
		break;
	}
	case 0x43: { /* get/set attributes */
		char dos[128], abs[256], path[1024];
		get_str(ds_dx(), dos, sizeof dos);
		if (on_cd(dos, abs, sizeof abs)) {
			struct iso_entry e;
			if (!cd_present() || !iso_lookup(abs, &e)) {
				fail_dos(2);
			}
			else if (AL) {
				fail_dos(5);
			}
			else {
				wr(R_CX, 2, e.dir ? 0x11 : 0x21); /* read-only */
				ok();
			}
			break;
		}
		host_path(dos, path, sizeof path);
		std::error_code ec;
		fs::file_status st = fs::status(path, ec);
		if (!fs::exists(st)) {
			fail_dos(ec ? dos_error(ec) : 2);
			break;
		}
		wr(R_CX, 2, fs::is_directory(st) ? 0x10 : 0x20);
		ok();
		break;
	}
	case 0x44: /* IOCTL */
		if (AL == 0) { /* device information */
			if (!get_handle(BX)) {
				fail_dos(6);
				break;
			}
			wr(R_DX, 2, BX < FIRST_FILE ? 0x80d3 : 0x0002);
			ok();
		}
		else if (AL == 1) {
			ok();
		}
		else {
			fail_dos(1);
		}
		break;
	case 0x47: /* current directory */
	{
		const char *dir = cwds[DL ? (DL - 1) % 26 : cur_drive];
		for (size_t i = 0; i <= strlen(dir); i++) {
			M[ds_si() + i] = (uint8_t)dir[i];
		}
	}
		ok();
		break;
	case 0x4a: /* resize memory block: always fits */
		ok();
		break;
	case 0x4c: /* exit */
		if (!dos_echo) vga_print_text(stdout);
		fflush(stdout);
		wc_dump_memory("the program exited");
		if (wc_trace) {
			fprintf(stderr, "wc: program exited with code %llu\n", AL);
			cpu_report(stderr);
		}
		host_exit((int)AL);
	case 0x4e: { /* find first: the search goes into the DTA's reserved bytes */
		char dos[128];
		get_str(ds_dx(), dos, sizeof dos);
		char *name = dos;
		for (char *p = dos; *p; p++) {
			if (*p == '\\' || *p == '/' || *p == ':') name = p + 1;
		}
		char fcb[11], abs[256];
		to_fcb(name, fcb);
		*name = 0;
		find_on_cd = on_cd(dos, abs, sizeof abs);
		if (find_on_cd) {
			if (!cd_present() || !iso_lookup(abs, &find_cd_dir) || !find_cd_dir.dir) {
				fail_dos(3);
				break;
			}
			snprintf(find_dir, sizeof find_dir, "D:\\%s", abs);
		}
		else {
			host_path(dos, find_dir, sizeof find_dir); /* one search folder at a time */
		}
		M[dta] = (uint8_t)CX;
		memcpy(M + dta + 1, fcb, 11);
		put(dta + 12, 4, 0);
		find_next();
		if (wc_trace) fprintf(stderr, "wc: find first \"%.11s\" in %s -> %s\n", fcb, find_dir, (rd(R_CF, 1) ? "nothing" : (char *)M + dta + 0x1e));
		break;
	}
	case 0x4f: /* find next */
		find_next();
		break;
	case 0x62: /* get PSP */
		wr(R_BX, 2, wc_psp_seg);
		break;
	default:
		fflush(stdout);
		fprintf(stderr, "wc: unimplemented DOS function INT 21h AH=%02x at %05x\n", ah, at);
		cpu_report(stderr);
		host_exit(2);
	}
}

static uint64_t bios_ticks(void)
{
	auto now = std::chrono::system_clock::now();
	time_t t = std::chrono::system_clock::to_time_t(now);
	struct tm *tm = localtime(&t);
	double secs = tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec +
		std::chrono::duration<double>(now - std::chrono::system_clock::from_time_t(t)).count();
	return (uint64_t)(secs * 1193180.0 / 65536.0);
}

/* INT 2Fh: MSCDEX (AX=15xxh) with the CD as drive D:, version 2.21; nothing else is installed
 * (AL stays as it was). */
static void multiplex(uint32_t at)
{
	uint16_t ax = (uint16_t)AX;
	if (wc_trace) {
		fprintf(stderr, "wc: INT 2Fh AX=%04x BX=%04llx CX=%04llx at %05x\n", ax, BX, CX, at);
	}
	switch (ax) {
	case 0x1500: /* installation check: BX drives, CX the first */
		wr(R_BX, 2, cd_present() ? 1 : 0);
		wr(R_CX, 2, DRIVE_CD);
		break;
	case 0x150b: /* is drive CX a CD drive */
		wr(R_AX, 2, CX == DRIVE_CD && cd_present() ? 0x5ad8 : 0);
		wr(R_BX, 2, 0xadad);
		break;
	case 0x150c: /* version */
		wr(R_BX, 2, 0x0215);
		break;
	case 0x150d: { /* the CD drives' letters into ES:BX */
		uint32_t a = dos_pm ? wc_sb[0] + (uint32_t)rd(R_BX, 4) : (uint32_t)lin(ES, BX);
		M[a] = DRIVE_CD;
		if (wc_trace) fprintf(stderr, "wc: MSCDEX drive letters at %05x\n", a);
		break;
	}
	default:
		if ((ax >> 8) == 0x15) {
			fflush(stdout);
			fprintf(stderr, "wc: unimplemented MSCDEX function INT 2Fh AX=%04x at %05x\n", ax, at);
			cpu_report(stderr);
			host_exit(2);
		}
		break;
	}
}

/* The BIOS and DOS services behind the vectors' initial F000:nn entries. */
void vga_bios(uint32_t at);
void mouse_int(uint32_t at);
void ems_int(uint32_t at);

void dos_int(uint64_t n, uint32_t at)
{
	switch (n) {
	case 0x21:
		dos(at);
		return;
	case 0x10:
		vga_bios(at);
		return;
	case 0x11: /* equipment list */
		wr(R_AX, 2, 0x0021);
		return;
	case 0x12: /* conventional memory in KB */
		wr(R_AX, 2, 640);
		return;
	case 0x15: /* system services: the configuration (pc.cpp), the keyboard intercept (the key goes on) */
		if (AH == 0xc0) {
			wr(R_ES, 2, 0xf000);
			wr(R_BX, 2, 0xe6f5);
			wr(R_AH, 1, 0);
			wr(R_CF, 1, 0);
		}
		else if (AH == 0x4f) {
			wr(R_CF, 1, 1);
		}
		else {
			wr(R_AH, 1, 0x86); /* not supported */
			wr(R_CF, 1, 1);
		}
		return;
	case 0x16: { /* keyboard */
		uint16_t key;
		if (wc_trace) {
			fprintf(stderr, "wc: INT 16h AH=%02llx (keyboard) at %05x\n", AH, at);
		}
		switch (AH) {
		case 0x00: case 0x10: /* read */
			wr(R_AX, 2, bios_key_get(at));
			break;
		case 0x01: case 0x11: /* status: ZF clear and the key if there is one */
			if (bios_key_peek(&key)) {
				wr(R_AX, 2, key);
				wr(R_ZF, 1, 0);
			}
			else {
				wr(R_ZF, 1, 1);
			}
			break;
		case 0x02: case 0x12: /* shift flags */
			wr(R_AL, 1, M[0x417]);
			break;
		}
		return;
	}
	case 0x1a: /* timer ticks */
		if (AH == 0) {
			uint64_t t = bios_ticks();
			wr(R_CX, 2, t >> 16);
			wr(R_DX, 2, t & 0xffff);
			wr(R_AL, 1, 0);
		}
		return;
	case 0x33:
		mouse_int(at);
		return;
	case 0x67:
		ems_int(at);
		return;
	case 0x2f:
		multiplex(at);
		return;
	default:
		fflush(stdout);
		fprintf(stderr, "wc: unimplemented interrupt INT %02llxh AX=%04llx at %05x\n", n, AX, at);
		cpu_report(stderr);
		host_exit(2);
	}
}
