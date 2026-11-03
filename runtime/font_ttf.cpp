/* The text mode's font (code page 437, 8x16) for the window, made at startup from a TrueType font
 * with SDL_ttf: font.ttf next to the executable (CMake copies it from fonts/), or WC_FONT. There is
 * no VGA BIOS font here. The line and block characters (B0h-DFh) are drawn here, so that they join
 * from cell to cell whatever the font. */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

static const char *const cp437 =
	" ☺☻♥♦♣♠•◘○◙♂♀♪♫☼►◄↕‼¶§▬↨↑↓→←∟↔▲▼"
	" !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~⌂"
	"ÇüéâäàåçêëèïîìÄÅÉæÆôöòûùÿÖÜ¢£¥₧ƒáíóúñÑªº¿⌐¬½¼¡«»"
	"░▒▓│┤╡╢╖╕╣║╗╝╜╛┐└┴┬├─┼╞╟╚╔╩╦╠═╬╧╨╤╥╙╘╒╓╫╪┘┌█▄▌▐▀"
	"αßΓπΣσµτΦΘΩδ∞φε∩≡±≥≤⌠⌡÷≈°∙·√ⁿ²■ ";

/* B3h-DAh: the lines up, down, left and right of each (1 single, 2 double) */
static const char *const lines =
	"1100" "1110" "1120" "2210" "0210" "0120" "2220" "2200" "0220" "2020" "2010" "1020" "0110"
	"1001" "1011" "0111" "1101" "0011" "1111" "1102" "2201" "2002" "0202" "2022" "0222" "2202"
	"0022" "2222" "1022" "2011" "0122" "0211" "2001" "1002" "0102" "0201" "2211" "1122" "1010" "0101";

static void set(uint8_t *g, int x, int y)
{
	g[y] |= (uint8_t)(0x80 >> x);
}

static void draw_box(uint8_t *g, unsigned c)
{
	if (c <= 0xb2) { /* shades */
		for (int y = 0; y < 16; y++) {
			g[y] = c == 0xb0 ? (y % 4 == 0 ? 0x88 : y % 4 == 2 ? 0x22 : 0) : c == 0xb1 ? (y % 2 ? 0xaa : 0x55)
			                                                                  : (y % 2 ? 0x77 : 0xdd);
		}
		return;
	}
	if (c >= 0xdb) { /* blocks: full, lower, left, right, upper half */
		for (int y = 0; y < 16; y++) {
			g[y] = c == 0xdb ? 0xff : c == 0xdc ? (y >= 8 ? 0xff : 0) : c == 0xdd ? 0xf0 : c == 0xde ? 0x0f
			                                                                  : (y < 8 ? 0xff : 0);
		}
		return;
	}
	const char *l = lines + (c - 0xb3) * 4;
	int up = l[0] - '0', down = l[1] - '0', left = l[2] - '0', right = l[3] - '0';
	/* single lines in columns 3-4 and row 7, double ones in columns 2 and 5, rows 5 and 9 */
	auto vertical = [&](int style, int from, int to) {
		for (int y = from; y <= to; y++) {
			if (style == 1) { set(g, 3, y); set(g, 4, y); }
			if (style == 2) { set(g, 2, y); set(g, 5, y); }
		}
	};
	auto horizontal = [&](int style, int from, int to) {
		for (int x = from; x <= to; x++) {
			if (style == 1) set(g, x, 7);
			if (style == 2) { set(g, x, 5); set(g, x, 9); }
		}
	};
	/* each line reaches across the middle to the far side of the lines crossing it */
	int h = left > right ? left : right, v = up > down ? up : down;
	vertical(up, 0, h == 2 ? 9 : 7);
	vertical(down, h == 2 ? 5 : 7, 15);
	horizontal(left, 0, v == 2 ? 5 : 4);
	horizontal(right, v == 2 ? 2 : 3, 7);
}

/* The next character of a UTF-8 string. */
static uint32_t next_char(const char *&p)
{
	uint8_t c = (uint8_t)*p++;
	int more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
	uint32_t ch = more ? c & (0x3f >> more) : c;
	while (more--) ch = ch << 6 | ((uint8_t)*p++ & 0x3f);
	return ch;
}

/* The largest size whose lines fit 16 pixels and whose characters fit 8 (a font drawn for 8x16
 * cells, such as a VGA font's outline, fits at 16 exactly). */
static TTF_Font *open_fitting(const char *path)
{
	for (float size = 16; size >= 8; size -= 0.5f) {
		TTF_Font *font = TTF_OpenFont(path, size);
		if (!font) return NULL;
		int advance = 0;
		TTF_GetGlyphMetrics(font, 'M', NULL, NULL, NULL, NULL, &advance);
		if (TTF_GetFontHeight(font) <= 16 && advance <= 8) return font;
		TTF_CloseFont(font);
	}
	return NULL;
}

/* The 256 characters, 16 bytes each (a row a byte, bit 7 the leftmost pixel). Without a font only
 * the line and block characters are there. */
void host_font(uint8_t glyphs[256 * 16])
{
	memset(glyphs, 0, 256 * 16);
	for (unsigned c = 0xb0; c <= 0xdf; c++) draw_box(glyphs + c * 16, c);
	const char *env = getenv("WC_FONT");
	std::string path = env && *env ? env : std::string(SDL_GetBasePath()) + "font.ttf";
	if (!TTF_Init()) return;
	TTF_Font *font = open_fitting(path.c_str());
	if (!font) {
		fprintf(stderr, "wc: no font for text mode (%s: %s); put a TrueType font in fonts/ and build again\n",
			path.c_str(), SDL_GetError());
		TTF_Quit();
		return;
	}
	int top = (16 - TTF_GetFontHeight(font)) / 2; /* the line's place in the cell */
	const SDL_Color white = { 255, 255, 255, 255 };
	const char *p = cp437;
	for (unsigned c = 0; c < 256; c++) {
		uint32_t ch = next_char(p);
		if ((c >= 0xb0 && c <= 0xdf) || ch == ' ' || !TTF_FontHasGlyph(font, ch)) continue;
		SDL_Surface *s = TTF_RenderGlyph_Blended(font, ch, white);
		SDL_Surface *rgba = s ? SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA32) : NULL;
		if (rgba) {
			int left = (8 - rgba->w) / 2;
			for (int y = 0; y < rgba->h; y++) {
				const uint8_t *row = (const uint8_t *)rgba->pixels + y * rgba->pitch;
				for (int x = 0; x < rgba->w; x++) {
					int cx = left + x, cy = top + y;
					if (row[x * 4 + 3] >= 100 && cx >= 0 && cx < 8 && cy >= 0 && cy < 16) {
						set(glyphs + c * 16, cx, cy); /* from the smoothed glyph */
					}
				}
			}
		}
		SDL_DestroySurface(rgba);
		SDL_DestroySurface(s);
	}
	TTF_CloseFont(font);
	TTF_Quit();
}
