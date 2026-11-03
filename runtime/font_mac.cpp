/* The text mode's font (code page 437, 8x16) for the window, made at startup: drawn from Menlo with
 * Core Text (other fonts where Menlo lacks a character), without the VGA BIOS's font. The line and
 * block characters (B0h-DFh) are drawn here, so that they join from cell to cell. */
#include <CoreText/CoreText.h>
#include <cstdint>
#include <cstring>

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

/* The 256 characters, 16 bytes each (a row a byte, bit 7 the leftmost pixel). */
void host_font(uint8_t glyphs[256 * 16])
{
	memset(glyphs, 0, 256 * 16);
	CTFontRef font = CTFontCreateWithName(CFSTR("Menlo-Regular"), 14, NULL);
	CGColorSpaceRef gray = CGColorSpaceCreateDeviceGray();
	uint8_t bits[16 * 8];
	CGContextRef cg = CGBitmapContextCreate(bits, 8, 16, 8, 8, gray, kCGImageAlphaNone);
	CGContextSetGrayFillColor(cg, 1, 1);
	CGFloat baseline = (CGFloat)(int)(CTFontGetDescent(font) + 0.5) + 1; /* from the cell's bottom */
	const void *keys[] = { kCTFontAttributeName, kCTForegroundColorFromContextAttributeName };
	const void *values[] = { font, kCFBooleanTrue };
	CFDictionaryRef attrs = CFDictionaryCreate(NULL, keys, values, 2, &kCFTypeDictionaryKeyCallBacks,
		&kCFTypeDictionaryValueCallBacks);
	const char *p = cp437;
	for (unsigned c = 0; c < 256; c++) {
		size_t n = 1;
		while ((p[n] & 0xc0) == 0x80) n++;
		uint8_t *g = glyphs + c * 16;
		if (c >= 0xb0 && c <= 0xdf) draw_box(g, c);
		else if (*p != ' ') {
			CFStringRef s = CFStringCreateWithBytes(NULL, (const UInt8 *)p, (CFIndex)n, kCFStringEncodingUTF8, false);
			CFAttributedStringRef text = CFAttributedStringCreate(NULL, s, attrs);
			CTLineRef line = CTLineCreateWithAttributedString(text);
			memset(bits, 0, sizeof bits);
			CGContextSetTextPosition(cg, (8 - CTLineGetTypographicBounds(line, NULL, NULL, NULL)) / 2, baseline);
			CTLineDraw(line, cg);
			for (int y = 0; y < 16; y++) {
				for (int x = 0; x < 8; x++) {
					if (bits[y * 8 + x] >= 100) set(g, x, y); /* from the smoothed picture */
				}
			}
			CFRelease(line);
			CFRelease(text);
			CFRelease(s);
		}
		p += n;
	}
	CFRelease(attrs);
	CGContextRelease(cg);
	CGColorSpaceRelease(gray);
	CFRelease(font);
}
