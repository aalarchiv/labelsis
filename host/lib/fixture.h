#ifndef FIXTURE_H
#define FIXTURE_H

/* Tiny helper for host tests: load a captured oracle fixture from
 * test/fixtures/ into memory. */

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *data;
    size_t   len;
} fixture_t;

/* Load the file. Returns 0 on success, -1 on any I/O failure (errno set). */
int  fixture_load(const char *path, fixture_t *out);
void fixture_free(fixture_t *f);

/* Absolute path to test/fixtures/ baked in at compile time. */
const char *fixture_dir(void);

#endif
