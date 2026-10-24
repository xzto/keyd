/*
 * keyd - A key remapping daemon.
 *
 * © 2019 Raheman Vaiya (see also: LICENSE).
 */
#include "string.h"

int utf8_read_char(const char *_s, uint32_t *code)
{
	const unsigned char *s = (const unsigned char*)_s;

	if (!s[0])
		return 0;

	if (s[0] < 0x80) {
		*code = s[0];
		return 1;
	}

	int size;
	uint32_t value, minimum;
	if (s[0] >= 0xC2 && s[0] <= 0xDF) {
		size = 2;
		value = s[0] & 0x1F;
		minimum = 0x80;
	} else if (s[0] >= 0xE0 && s[0] <= 0xEF) {
		size = 3;
		value = s[0] & 0x0F;
		minimum = 0x800;
	} else if (s[0] >= 0xF0 && s[0] <= 0xF4) {
		size = 4;
		value = s[0] & 0x07;
		minimum = 0x10000;
	} else {
		return -1;
	}

	for (int i = 1; i < size; i++) {
		if (!s[i] || (s[i] & 0xC0) != 0x80)
			return -1;
		value = (value << 6) | (s[i] & 0x3F);
	}
	if (value < minimum || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF))
		return -1;
	*code = value;
	return size;
}

int utf8_strlen(const char *s)
{
	uint32_t code;
	int csz;
	int n = 0;

	while ((csz = utf8_read_char(s, &code)) > 0) {
		n++;
		s+=csz;
	}

	return csz < 0 ? -1 : n;
}

int is_timeval(const char *s)
{
	if (s[0] < '0' || s[0] > '9')
		return 0;

	while(*s && *s >= '0' && *s <= '9')
		s++;

	return s[0] == 'm' && s[1] == 's' && (s[2] == 0 || s[2] == '\n');
}

/*
 * Returns the character size in bytes, or 0 in the case of the empty string.
 */
size_t str_escape(char *s)
{
	int i = 0;
	int n = 0;

	for (i = 0; s[i]; i++) {
		if (s[i] == '\\') {
			switch (s[i+1]) {
			case 'n':
				s[n++] = '\n';
				break;
			case 't':
				s[n++] = '\t';
				break;
			case '\\':
				s[n++] = '\\';
				break;
			case ')':
				s[n++] = ')';
				break;
			case '(':
				s[n++] = '(';
				break;
			case 0:
				s[n] = 0;
				return n;
			default:
				s[n++] = '\\';
				s[n++] = s[i+1];
				break;
			}

			i++;
		} else {
			s[n++] = s[i];
		}
	}

	s[n] = 0;

	return n;
}

