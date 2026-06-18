/*
 * shared_helper.h
 *
 * Portable ASCII utility functions shared across all modules.
 */

#ifndef INC_SHARED_HELPER_H_
#define INC_SHARED_HELPER_H_

#ifdef __cplusplus
extern "C" {
#endif

/* Return uppercase of c for ASCII letters; other characters are unchanged. */
char ascii_upper(char c);

/* Case-insensitive string equality check.
 * Returns 1 if both strings are equal (ignoring case), 0 otherwise.
 * NULL arguments are never equal. */
int ascii_equal(const char *a, const char *b);

#ifdef __cplusplus
}
#endif

#endif /* INC_SHARED_HELPER_H_ */
