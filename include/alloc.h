#ifndef __NK_ALLOC_H__
#define __NK_ALLOC_H__

#include "kernel.h"

#include <pthread.h>

typedef struct nk_freelist_node nk_freelist_node;
typedef struct nk_freelist_attrs nk_freelist_attrs;
typedef struct nk_freelist nk_freelist;

struct nk_freelist_node {
  nk_freelist_node *next;
};

typedef void *(*nk_freelist_alloc_func)(const nk_freelist_attrs *attrs,
                                        void *cookie);
typedef void (*nk_freelist_free_func)(const nk_freelist_attrs *attrs,
                                      void *cookie, void *p);
typedef void (*nk_freelist_zero_func)(const nk_freelist_attrs *attrs,
                                      void *cookie, void *p);

struct nk_freelist_attrs {
  size_t node_size; // used only by default alloc/free/zero funcs.
  size_t max_count;
  nk_freelist_alloc_func alloc_func;
  nk_freelist_free_func free_func;
  nk_freelist_zero_func zero_func;
};

struct nk_freelist {
  nk_freelist_attrs attrs;
  void *cookie;

  pthread_spinlock_t lock;
  size_t count;
  nk_freelist_node *freelist_head;
};

nk_status nk_freelist_init(nk_freelist *f, const nk_freelist_attrs *attrs,
                           void *cookie);
void nk_freelist_destroy(nk_freelist *f);
void *nk_freelist_alloc(nk_freelist *f);
void nk_freelist_free(nk_freelist *f, void *p);

#define DEFINE_SIMPLE_FREELIST_TYPE(type, count)                               \
  static nk_freelist_attrs type##_freelist_attrs = {                           \
      .node_size = sizeof(type),                                               \
      .max_count = count,                                                      \
      .alloc_func = NULL,                                                      \
      .free_func = NULL,                                                       \
      .zero_func = NULL,                                                       \
  }

#endif // __NK_ALLOC_H__
