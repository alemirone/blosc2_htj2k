/*********************************************************************
 * Temporary JPEG2000 codec registration bootstrap for Blosc2.
 *
 * This library is meant to be used through LD_PRELOAD during the transition
 * phase before j2k/htj2k receive official c-blosc2 codec ids.
 *********************************************************************/

#include "blosc2.h"

#include <stdbool.h>
#include <stdint.h>

#if !defined(_WIN32)
#include <dlfcn.h>
#endif

#define BLOSC2_JPEG2000_J2K_ID 160
#define BLOSC2_JPEG2000_HTJ2K_ID 161

static bool blosc2_jpeg2000_registering = false;

static int register_dynamic_codec(uint8_t id, const char *name) {
  int existing = blosc2_compname_to_compcode(name);
  if (existing == id) {
    return 0;
  }
  if (existing >= 0) {
    return -1;
  }

  blosc2_codec codec = {0};
  codec.compcode = id;
  codec.compname = (char *)name;
  codec.complib = id;
  codec.version = 1;
  codec.encoder = NULL;
  codec.decoder = NULL;
  return blosc2_register_codec(&codec);
}

int blosc2_jpeg2000_register_codecs(void) {
  if (blosc2_jpeg2000_registering) {
    return 0;
  }
  blosc2_jpeg2000_registering = true;
  int rc_j2k = register_dynamic_codec(BLOSC2_JPEG2000_J2K_ID, "j2k");
  int rc_htj2k = register_dynamic_codec(BLOSC2_JPEG2000_HTJ2K_ID, "htj2k");
  blosc2_jpeg2000_registering = false;
  return rc_j2k < 0 ? rc_j2k : rc_htj2k;
}

#if !defined(_WIN32)
typedef void (*blosc2_init_fn)(void);

static void call_real_blosc2_init(void) {
  blosc2_init_fn real_init = (blosc2_init_fn)dlsym(RTLD_NEXT, "blosc2_init");
  if (real_init != NULL) {
    real_init();
  }
}

void blosc2_init(void) {
  call_real_blosc2_init();
  blosc2_jpeg2000_register_codecs();
}

__attribute__((constructor))
static void init_jpeg2000_codecs(void) {
  blosc2_init();
}
#endif

