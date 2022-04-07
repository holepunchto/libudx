#ifndef UDX_DEBUG_H
#define UDX_DEBUG_H

#ifndef NDEBUG
#define DEBUG 1
#else
#define DEBUG 0
#endif

#define debug_printf(...) do { if (DEBUG) fprintf(stderr, __VA_ARGS__); } while (0)

#ifdef DEBUG_MEMORY

#include <dlfcn.h>

static int mallocs_active = 0;

static void * (*real_malloc)(size_t) = NULL;
static void (*real_free)(void *ptr) = NULL;

void *malloc(size_t size) {
  if (real_malloc == NULL) real_malloc = dlsym(RTLD_NEXT, "malloc");

  mallocs_active++;
  void *ptr = real_malloc(size);
  fprintf(stderr, "DEBUG_MEMORY: malloc(%zu) = %p, pointers=%i\n", size, ptr, mallocs_active);

  return ptr;
}

void free(void *ptr) {
  if (real_free == NULL) real_free = dlsym(RTLD_NEXT, "free");

  mallocs_active--;
  fprintf(stderr, "DEBUG_MEMORY: free(%p), pointers=%i\n", ptr, mallocs_active);

  real_free(ptr);
}

#endif

#endif // UDX_DEBUG_H
