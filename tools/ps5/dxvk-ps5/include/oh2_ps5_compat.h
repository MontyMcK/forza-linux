// Force-included into every DXVK source built for the PS5 (tools/ps5/build_dxvk.sh).
#pragma once

// FreeBSD's <sys/types.h> defines major()/minor() macros, which break DXVK's
// Version::major()/minor(); its include guard keeps them away once undefined.
#include <sys/types.h>
#undef major
#undef minor
// <machine/param.h>'s CACHE_LINE_SIZE macro breaks DXVK's constant of that name.
#include <sys/param.h>
#undef CACHE_LINE_SIZE

#ifdef __cplusplus
#include <pthread.h>
#include <pthread_np.h>
// glibc's name for FreeBSD's pthread_set_name_np (DXVK names its threads).
static inline int pthread_setname_np(pthread_t thread, const char* name) {
  pthread_set_name_np(thread, name);
  return 0;
}
#endif
