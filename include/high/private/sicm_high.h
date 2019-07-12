#pragma once

#include <inttypes.h>
#include "sicm_low.h"
#include "sicm_impl.h"
#include "sicm_tree.h"

enum arena_layout {
  SHARED_ONE_ARENA, /* One arena between all threads */
  EXCLUSIVE_ONE_ARENA, /* One arena per thread */
  SHARED_DEVICE_ARENAS, /* One arena per device */
  EXCLUSIVE_DEVICE_ARENAS, /* One arena per device per thread */
  SHARED_SITE_ARENAS, /* One arena per allocation site */
  EXCLUSIVE_SITE_ARENAS, /* One arena per allocation site per thread */
  EXCLUSIVE_TWO_DEVICE_ARENAS, /* Two arenas per device per thread */
  EXCLUSIVE_FOUR_DEVICE_ARENAS, /* Four arenas per device per thread */
  INVALID_LAYOUT
};

/* Keeps track of additional information about arenas */
typedef struct arena_info {
  int *alloc_sites, num_alloc_sites; /* Stores the allocation sites that are in this arena */
  unsigned index; /* Index into the arenas array */
  sicm_arena arena; /* SICM's low-level interface pointer */
  size_t accesses, rss, peak_rss, avg_rss, cur_accesses; /* Profiling info */
  double acc_per_sample;
} arena_info;

/* A tree associating site IDs with device pointers.
 * Sites should be bound to the device that they're associated with.
 * Filled with guidance from an offline profiling run or with
 * online profiling.
 */
typedef sicm_device * deviceptr;
use_tree(int, deviceptr);
use_tree(deviceptr, int);
use_tree(int, int);

/* Keeps track of arenas, extents, mutices, etc. */
typedef struct tracker_struct {
  /* Stores all machine devices and device
   * we should bind to by default */
  struct sicm_device_list device_list;
  int num_numa_nodes;
  deviceptr default_device;

  /* Allocation site ID -> device */
  tree(int, deviceptr) site_nodes;
  /* Stores arenas associated with a device,
   * for the per-device arena layouts only. */
  tree(deviceptr, int) device_arenas;

  /* Keep track of all extents */
  extent_arr *extents;
  extent_arr *rss_extents; /* The extents that we want to get the RSS of */

  /* Gets locked when we add a new extent */
  pthread_rwlock_t extents_lock = PTHREAD_RWLOCK_INITIALIZER;

  /* Keeps track of arenas */
  arena_info **arenas;
  enum arena_layout layout;
  int max_arenas, arenas_per_thread, max_sites_per_arena;
  int max_index;

  /* Stores which arena an allocation site goes into. Only for
   * the `*_SITE_ARENAS` layouts, where there is an arena for
   * each allocation site.
   */
  tree(int, int) site_arenas;
  int arena_counter;

  /* Gets locked when we add an arena */
  pthread_mutex_t arena_lock = PTHREAD_MUTEX_INITIALIZER;

  /* Associates a thread with an index (starting at 0) into the `arenas` array */
  pthread_key_t thread_key;
  int *thread_indices, *orig_thread_indices, *max_thread_indices, max_threads;
  int num_static_sites;

  /* Passes an arena index to the extent hooks */
  int *pending_indices;
} tracker_struct;

#define DEFAULT_ARENA_LAYOUT INVALID_LAYOUT

__attribute__((constructor))
void sh_init();

__attribute__((destructor))
void sh_terminate();

void sh_create_extent(void *begin, void *end);
int get_arena_index(int id);

/* Options for if/when/how to profile. Initialized in src/high/sicm_high_init.c,
 * used by src/high/sicm_profile.c.
 */
typedef struct profiling_options {
  /* Should we do profiling? */
  int should_profile_online;
  int should_profile_all;
  int should_profile_one;
  int profile_one_site;
  int should_profile_rss;
  int should_run_rdspy;

  /* How quickly to sample accesses/RSS */
  float profile_all_rate;
  float profile_rss_rate;
  int sample_freq;
  int max_sample_pages;

  /* The device to profile bandwidth on */
  deviceptr profile_one_device;

  /* Online profiling device and parameters */
  deviceptr online_device;
  ssize_t online_device_cap;

  /* Array of strings for profile_all events */
  size_t num_profile_all_events;
  char **profile_all_events;

  /* Array of strings for profile_one events */
  size_t num_profile_one_events;
  char **profile_one_events;

  /* Array of strings of IMCs for the bandwidth profiling */
  char **imcs;
  int num_imcs, max_imc_len, max_event_len;

  /* Set depending on which type of profiling we're doing */
  size_t num_events;
  char **events;
} profiling_options;

#ifdef __cplusplus
extern "C" {
#endif
  void* sh_alloc_exact(int id, size_t sz);
  void* sh_alloc(int id, size_t sz);
  void* sh_aligned_alloc(int id, size_t alignment, size_t sz);
  void* sh_memalign(int id, size_t alignment, size_t sz);
  int sh_posix_memalign(int id, void **ptr, size_t alignment, size_t sz);
  void* sh_calloc(int id, size_t num, size_t sz);
  void* sh_realloc(int id, void *ptr, size_t sz);
  void sh_free(void* ptr);
#ifdef __cplusplus
}
#endif
