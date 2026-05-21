#define _GNU_SOURCE

/*********************************************************************
 * JPEG2000 bootstrap for Blosc2.
 *
 * This library can be used through LD_PRELOAD to force early Blosc2
 * initialization before HDF5 callbacks run.  The official j2k/htj2k ids
 * themselves must be present in the c-blosc2 codec registry.
 *********************************************************************/

#include "blosc2.h"

#if !defined(_WIN32)
#include <dlfcn.h>

typedef void (*blosc2_init_fn)(void);

static void call_real_blosc2_init(void) {
  blosc2_init_fn real_init = (blosc2_init_fn)dlsym(RTLD_NEXT, "blosc2_init");
  if (real_init != NULL) {
    real_init();
  }
}

void blosc2_init(void) {
  call_real_blosc2_init();
}

__attribute__((constructor))
static void init_blosc2_early(void) {
  blosc2_init();
}
#endif
