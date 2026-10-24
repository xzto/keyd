/*
 * keyd - A key remapping daemon.
 *
 * © 2019 Raheman Vaiya (see also: LICENSE).
 */
#ifndef STRING_H
#define STRING_H

#include <stdint.h>
#include <stdlib.h>

/* Return -1 on invalid UTF-8; read_char returns 0 at the terminator. */
int utf8_read_char(const char *_s, uint32_t *code);
int utf8_strlen(const char *s);

int is_timeval(const char *s);
size_t str_escape(char *s);
#endif
