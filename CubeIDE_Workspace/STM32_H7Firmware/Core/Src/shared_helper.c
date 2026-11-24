/*
 * shared_helper.c
 *
 * Portable ASCII utility functions shared across all modules.
 */

#include "shared_helper.h"
#include <stddef.h>

/* Return the uppercase equivalent of c for ASCII letters; other chars unchanged. */
char ascii_upper(char c)
{
    if ((c >= 'a') && (c <= 'z')) {
        return (char)(c - ('a' - 'A'));
    }
    return c;
}

/* Case-insensitive string comparison.
 * Returns 1 if a and b are equal (ignoring ASCII case), 0 otherwise.
 * NULL pointers are never considered equal. */
int ascii_equal(const char *a, const char *b)
{
    if ((a == NULL) || (b == NULL)) { return 0; }
    while ((*a != '\0') && (*b != '\0')) {
        if (ascii_upper(*a) != ascii_upper(*b)) { return 0; }
        ++a;
        ++b;
    }
    return (*a == '\0') && (*b == '\0');
}
