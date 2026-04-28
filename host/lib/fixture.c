#include "fixture.h"

#include <stdio.h>
#include <stdlib.h>

int fixture_load(const char *path, fixture_t *out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return -1; }
    rewind(f);
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(f); return -1; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf); fclose(f); return -1;
    }
    fclose(f);
    out->data = buf;
    out->len = (size_t)sz;
    return 0;
}

void fixture_free(fixture_t *f)
{
    free(f->data);
    f->data = NULL;
    f->len = 0;
}

const char *fixture_dir(void)
{
    return PT_FIXTURE_DIR;
}
