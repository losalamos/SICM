#include <fcntl.h>
#include <numa.h>
#include <numaif.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <inttypes.h>
#include <pthread.h>
#include <jemalloc/jemalloc.h>

#include "sicm_high.h"
#include "sicm_low.h"
#include "sicm_impl.h"
#include "sicm_profile.h"
#include "sicm_rdspy.h"

/* Stores all machine devices and device
 * we should bind to by default */
static struct sicm_device_list device_list;
int num_numa_nodes;
struct sicm_device *default_device;

/* Allocation site ID -> device */
tree(int, deviceptr) site_nodes;
/* Stores arenas associated with a device,
 * for the per-device arena layouts only. */
tree(deviceptr, int) device_arenas;

/* Should we do profiling? */
int should_profile_online;
int should_profile_all;
int should_profile_one;
int should_profile_rss;
int should_run_rdspy;

/* How quickly to sample accesses/RSS */
float profile_all_rate;
float profile_rss_rate;
int sample_freq;
int max_sample_pages;

/* The device to profile bandwidth on */
struct sicm_device *profile_one_device;

/* Online profiling device and parameters */
struct sicm_device *online_device;
ssize_t online_device_cap, online_device_packed_size;

/* Sampling and bandwidth perf event, input to libpfm4 */
char *profile_one_event;
char *profile_all_event;

/* Array of strings of IMCs for the bandwidth profiling */
char **imcs;
int num_imcs, max_imc_len, max_event_len;

/* Keep track of all extents */
extent_arr *extents;
extent_arr *rss_extents; /* The extents that we want to get the RSS of */

/* Gets locked when we add a new extent */
pthread_rwlock_t extents_lock = PTHREAD_RWLOCK_INITIALIZER;

/* Keeps track of arenas */
arena_info **arenas;
static enum arena_layout layout;
static int max_arenas, arenas_per_thread, max_sites_per_arena;
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
static pthread_key_t thread_key;
static int *thread_indices, *orig_thread_indices, *max_thread_indices, max_threads;
static int num_static_sites;

/* Passes an arena index to the extent hooks */
static int *pending_indices;

/* Takes a string as input and outputs which arena layout it is */
enum arena_layout parse_layout(char *env) {
	size_t max_chars;

	max_chars = 32;

	if(strncmp(env, "SHARED_ONE_ARENA", max_chars) == 0) {
		return SHARED_ONE_ARENA;
	} else if(strncmp(env, "EXCLUSIVE_ONE_ARENA", max_chars) == 0) {
		return EXCLUSIVE_ONE_ARENA;
	} else if(strncmp(env, "SHARED_DEVICE_ARENAS", max_chars) == 0) {
		return SHARED_DEVICE_ARENAS;
	} else if(strncmp(env, "EXCLUSIVE_DEVICE_ARENAS", max_chars) == 0) {
		return EXCLUSIVE_DEVICE_ARENAS;
	} else if(strncmp(env, "SHARED_SITE_ARENAS", max_chars) == 0) {
		return SHARED_SITE_ARENAS;
	} else if(strncmp(env, "EXCLUSIVE_SITE_ARENAS", max_chars) == 0) {
		return EXCLUSIVE_SITE_ARENAS;
	} else if(strncmp(env, "EXCLUSIVE_TWO_DEVICE_ARENAS", max_chars) == 0) {
		return EXCLUSIVE_TWO_DEVICE_ARENAS;
	} else if(strncmp(env, "EXCLUSIVE_FOUR_DEVICE_ARENAS", max_chars) == 0) {
		return EXCLUSIVE_FOUR_DEVICE_ARENAS;
	}

  return INVALID_LAYOUT;
}

/* Returns the index of an allocation site in an arena,
 * -1 if it's not there */
int get_alloc_site(arena_info *arena, int id) {
  int i;
  for(i = 0; i < arena->num_alloc_sites; i++) {
    if(arena->alloc_sites[i] == id) {
      return i;
    }
  }

  return -1;
}

/* Converts an arena_layout to a string */
char *layout_str(enum arena_layout layout) {
  switch(layout) {
    case SHARED_ONE_ARENA:
      return "SHARED_ONE_ARENA";
    case EXCLUSIVE_ONE_ARENA:
      return "EXCLUSIVE_ONE_ARENA";
    case SHARED_DEVICE_ARENAS:
      return "SHARED_DEVICE_ARENAS";
    case EXCLUSIVE_DEVICE_ARENAS:
      return "EXCLUSIVE_DEVICE_ARENAS";
    case SHARED_SITE_ARENAS:
      return "SHARED_SITE_ARENAS";
    case EXCLUSIVE_SITE_ARENAS:
      return "EXCLUSIVE_SITE_ARENAS";
    case EXCLUSIVE_TWO_DEVICE_ARENAS:
      return "EXCLUSIVE_TWO_DEVICE_ARENAS";
    case EXCLUSIVE_FOUR_DEVICE_ARENAS:
      return "EXCLUSIVE_FOUR_DEVICE_ARENAS";
    default:
      break;
  }

  return "INVALID_LAYOUT";
}

/* Gets the SICM low-level device that corresponds to a NUMA node ID */
sicm_device *get_device_from_numa_node(int id) {
  struct sicm_device *retval, *device;
  int i;

  retval = NULL;
  /* Figure out which device the NUMA node corresponds to */
  device = device_list.devices;
  for(i = 0; i < device_list.count; i++) {
    /* If the device has a NUMA node, and if that node is the node we're
     * looking for.
     */
    if((device->tag == SICM_DRAM ||
       device->tag == SICM_KNL_HBM || 
       device->tag == SICM_POWERPC_HBM) &&
       sicm_numa_id(device) == id) {
      retval = device;
      break;
    }
    device++;
  }
  /* If we don't find an appropriate device, it stays NULL
   * so that no allocation sites will be bound to it
   */
  if(!retval) {
    fprintf(stderr, "Couldn't find an appropriate device for NUMA node %d.\n", id);
  }

  return retval;
}


/* Gets environment variables and sets up globals */
void set_options() {
  char *env, *str, *line, guidance, found_guidance;
  long long tmp_val;
  struct sicm_device *device;
  int i, node, site;
  FILE *guidance_file;
  ssize_t len;
  tree_it(int, deviceptr) it;

  /* Do we want to use the online approach, moving arenas around devices automatically? */
  env = getenv("SH_ONLINE_PROFILING");
  should_profile_online = 0;
  if(env) {
    should_profile_online = 1;
    tmp_val = strtoimax(env, NULL, 10);
    online_device = get_device_from_numa_node((int) tmp_val);
    online_device_cap = sicm_avail(online_device) * 1024; /* sicm_avail() returns kilobytes */
    printf("Doing online profiling, packing onto NUMA node %lld with a capacity of %zd.\n", tmp_val, online_device_cap);
  }

  /* Get the arena layout */
  env = getenv("SH_ARENA_LAYOUT");
  if(env) {
    layout = parse_layout(env);
  } else {
    layout = DEFAULT_ARENA_LAYOUT;
  }
  if(should_profile_online) {
    layout = SHARED_SITE_ARENAS;
  }
  printf("Arena layout: %s\n", layout_str(layout));

  /* Get max_threads */
  max_threads = numa_num_possible_cpus();
  env = getenv("SH_MAX_THREADS");
  if(env) {
    tmp_val = strtoimax(env, NULL, 10);
    if((tmp_val == 0) || (tmp_val > INT_MAX)) {
      printf("Invalid thread number given. Defaulting to %d.\n", max_threads);
    } else {
      max_threads = (int) tmp_val;
    }
  }
  printf("Maximum threads: %d\n", max_threads);

  /* Get max_arenas.
   * Keep in mind that 4096 is the maximum number supported by jemalloc.
   * An error occurs if this limit is reached.
   */
  max_arenas = 4096;
  env = getenv("SH_MAX_ARENAS");
  if(env) {
    tmp_val = strtoimax(env, NULL, 10);
    if((tmp_val == 0) || (tmp_val > INT_MAX)) {
      printf("Invalid arena number given. Defaulting to %d.\n", max_arenas);
    } else {
      max_arenas = (int) tmp_val;
    }
  }
  printf("Maximum arenas: %d\n", max_arenas);

  /* Get max_sites_per_arena.
   * This is the maximum amount of allocation sites that a single arena can hold.
   */
  max_sites_per_arena = 1;
  env = getenv("SH_MAX_SITES_PER_ARENA");
  if(env) {
    tmp_val = strtoimax(env, NULL, 10);
    if((tmp_val == 0) || (tmp_val > INT_MAX)) {
      printf("Invalid arena number given. Defaulting to %d.\n", max_arenas);
    } else {
      max_sites_per_arena = (int) tmp_val;
    }
  }
  printf("Maximum allocation sites per arena: %d\n", max_sites_per_arena);

  /* Should we profile all allocation sites using sampling-based profiling? */
  env = getenv("SH_PROFILE_ALL");
  should_profile_all = 0;
  if(env) {
    should_profile_all = 1;
    printf("Profiling all arenas.\n");
  }
  if(should_profile_online) {
    should_profile_all = 1;
  }
  profile_all_rate = 1.0;
  if(should_profile_all) {
    env = getenv("SH_PROFILE_ALL_RATE");
    if(env) {
      profile_all_rate = strtof(env, NULL);
    }
  }

  /* Should we profile (by isolating) a single allocation site onto a NUMA node
   * and getting the memory bandwidth on that node?  Pass the allocation site
   * ID as the value of this environment variable.
   */
  env = getenv("SH_PROFILE_ONE");
  should_profile_one = -1;
  if(env) {
    tmp_val = strtoimax(env, NULL, 10);
    if((tmp_val == 0) || (tmp_val > INT_MAX)) {
      printf("Invalid allocation site ID given: %d.\n", tmp_val);
      exit(1);
    } else {
      should_profile_one = (int) tmp_val;
    }
  }

  if(should_profile_one != -1) {
    /* If the above is true, which NUMA node should we isolate the allocation site
     * onto? The user should also set SH_DEFAULT_DEVICE to another device to avoid
     * the two being the same, if the allocation site is to be isolated.
     */
    env = getenv("SH_PROFILE_ONE_NODE");
    if(env) {
      tmp_val = strtoimax(env, NULL, 10);
      profile_one_device = get_device_from_numa_node((int) tmp_val);
      printf("Isolating node: %s, node %d\n", sicm_device_tag_str(profile_one_device->tag), 
                                              sicm_numa_id(profile_one_device));
    }

    /* The user can also specify a comma-delimited list of IMCs to read the
     * bandwidth from. This will be passed to libpfm. For example, on an Ivy
     * Bridge server, this value is e.g. `ivbep_unc_imc0`, and on KNL it's
     * `knl_unc_imc0`.
     */
    env = getenv("SH_PROFILE_ONE_IMC");
    num_imcs = 0;
    max_imc_len = 0;
    imcs = NULL;
    if(env) {
      printf("Got IMC string: %s\n", env);
      /* Parse out the IMCs into an array */
      while((str = strtok(env, ",")) != NULL) {
        printf("%s\n", str);
        num_imcs++;
        imcs = realloc(imcs, sizeof(char *) * num_imcs);
        imcs[num_imcs - 1] = str;
        if(strlen(str) > max_imc_len) {
          max_imc_len = strlen(str);
        }
        env = NULL;
      }
    }
    if(num_imcs == 0) {
      fprintf(stderr, "No IMCs given. Can't measure bandwidth.\n");
      exit(1);
    }

    /* What event should be used to measure the bandwidth? Default
     * to the hardcoded list in profile.c if not specified.
     */
    env = getenv("SH_PROFILE_ONE_EVENT");
    profile_one_event = NULL;
    max_event_len = 64;
    if(env) {
      profile_one_event = env;
      max_event_len = strlen(env);
      printf("Using event: %s\n", profile_one_event);
    }
  }

  /* Should we get the RSS of each arena? */
  env = getenv("SH_PROFILE_RSS");
  should_profile_rss = 0;
  if(env) {
    if(layout == SHARED_SITE_ARENAS) {
      should_profile_rss = 1;
      printf("Profiling RSS of all arenas.\n");
    } else {
      printf("Can't profile RSS, because we're using the wrong arena layout.\n");
    }
  }
  if(should_profile_online) {
    should_profile_rss = 1;
  }
  profile_rss_rate = 1.0;
  if(should_profile_rss) {
    env = getenv("SH_PROFILE_RSS_RATE");
    if(env) {
      profile_rss_rate = strtof(env, NULL);
    }
  }


  /* What sample frequency should we use? Default is 2048. Higher
   * frequencies will fill up the sample pages (below) faster.
   */
  env = getenv("SH_SAMPLE_FREQ");
  sample_freq = 2048;
  if(env) {
    tmp_val = strtoimax(env, NULL, 10);
    if((tmp_val <= 0)) {
      printf("Invalid sample frequency given. Defaulting to %d.\n", sample_freq);
    } else {
      sample_freq = (int) tmp_val;
    }
  }
  printf("Sample frequency: %d\n", sample_freq);

  /* How many samples should be collected by perf, maximum?
   * Assuming we're only tracking addresses, this number is multiplied by 
   * the page size and divided by 16 to get the maximum number of samples.
   * 8 of those bytes are the header, and the other 8 are the address itself.
   * By default this is 64 pages, which yields 16k samples.
   */
  env = getenv("SH_MAX_SAMPLE_PAGES");
  max_sample_pages = 64;
  if(env) {
    tmp_val = strtoimax(env, NULL, 10);
    /* Value needs to be non-negative, less than or equal to 512, and a power of 2. */
    if((tmp_val <= 0) || (tmp_val > 512) || (tmp_val & (tmp_val - 1))) {
      printf("Invalid number of pages given (%d). Defaulting to %d.\n", tmp_val, max_sample_pages);
    } else {
      max_sample_pages = (int) tmp_val;
    }
  }
  printf("Maximum sample pages: %d\n", max_sample_pages);

  /* Get default_device_tag */
  env = getenv("SH_DEFAULT_NODE");
  default_device = NULL;
  if(env) {
    tmp_val = strtoimax(env, NULL, 10);
    default_device = get_device_from_numa_node((int) tmp_val);
  }
  if(!default_device) {
    /* This assumes that the normal page size is the first one that it'll find */
    default_device = get_device_from_numa_node(0);
  }
  printf("Default device: %s\n", sicm_device_tag_str(default_device->tag));

  /* Get arenas_per_thread */
  switch(layout) {
    case SHARED_ONE_ARENA:
    case EXCLUSIVE_ONE_ARENA:
      arenas_per_thread = 1;
      break;
    case SHARED_DEVICE_ARENAS:
    case EXCLUSIVE_DEVICE_ARENAS:
      arenas_per_thread = num_numa_nodes; //(int) device_list.count;
      break;
    case SHARED_SITE_ARENAS:
    case EXCLUSIVE_SITE_ARENAS:
      arenas_per_thread = max_arenas;
      break;
    case EXCLUSIVE_TWO_DEVICE_ARENAS:
      arenas_per_thread = 2 * num_numa_nodes; //((int) device_list.count);
      break;
    case EXCLUSIVE_FOUR_DEVICE_ARENAS:
      arenas_per_thread = 4 * num_numa_nodes; //((int) device_list.count);
      break;
    default:
      arenas_per_thread = 1;
      break;
  };
  printf("Arenas per thread: %d\n", arenas_per_thread);

  /* Get the guidance file that tells where each site goes */
  env = getenv("SH_GUIDANCE_FILE");
  if(env) {
    /* Open the file */
    guidance_file = fopen(env, "r");
    if(!guidance_file) {
      fprintf(stderr, "Failed to open guidance file. Aborting.\n");
      exit(1);
    }

    /* Read in the sites */
    guidance = 0;
    found_guidance = 0; /* Set if we find any site guidance at all */
    line = NULL;
    len = 0;
    while(getline(&line, &len, guidance_file) != -1) {
      str = strtok(line, " ");
      if(guidance) {
        if(!str) continue;

        /* Look to see if it's the end */
        if(str && (strcmp(str, "=====") == 0)) {
          str = strtok(NULL, " ");
          if(str && (strcmp(str, "END") == 0)) {
            guidance = 0;
          } else {
            fprintf(stderr, "In a guidance section, and found five equals signs, but not the end. Aborting.\n");
            exit(1);
          }
          continue;
        }

        /* Read in the actual guidance now that we're in a guidance section */
        sscanf(str, "%d", &site);
        str = strtok(NULL, " ");
        if(!str) {
          fprintf(stderr, "Read in a site number from the guidance file, but no node number. Aborting.\n");
          exit(1);
        }
        sscanf(str, "%d", &node);
        tree_insert(site_nodes, site, get_device_from_numa_node(node));
        printf("Adding site %d to NUMA node %d.\n", site, node);
      } else {
        if(!str) continue;
        /* Find the "===== GUIDANCE" tokens */
        if(strcmp(str, "=====") != 0) continue;
        str = strtok(NULL, " ");
        if(str && (strcmp(str, "GUIDANCE") == 0)) {
          /* Now we're in a guidance section */
          guidance = 1;
          found_guidance = 1;
          continue;
        }
      }
    }
    if(!found_guidance) {
      fprintf(stderr, "Didn't find any guidance in the file. Aborting.\n");
      exit(1);
    }
  }

  env = getenv("SH_NUM_STATIC_SITES");
  if (env) {
    tmp_val = strtoimax(env, NULL, 10);
    if((tmp_val == 0) || (tmp_val > INT_MAX)) {
      printf("Invalid number of static sites given.\n");
    } else {
      num_static_sites = (int) tmp_val;
    }
  }
  printf("Number of static sites: %d\n", num_static_sites);

  env = getenv("SH_RDSPY");
  should_run_rdspy = 0;
  if (env) {
      if (!num_static_sites) {
          printf("Invalid static sites -- not running rdspy.\n");
      }
      should_run_rdspy = 1 && num_static_sites;
      if (should_run_rdspy) {
          printf("Running with rdspy.\n");
      }
  }
}

int get_thread_index() {
  int *val;

  /* Get this thread's index */
  val = (int *) pthread_getspecific(thread_key);

  /* If nonexistent, increment the counter and set it */
  if(val == NULL) {
    if(thread_indices + 1 >= max_thread_indices) {
      fprintf(stderr, "Maximum number of threads reached. Aborting.\n");
      exit(1);
    }
    pthread_setspecific(thread_key, (void *) thread_indices);
    val = thread_indices;
    thread_indices++;
  }

  return *val;
}

/* Adds an arena to the `arenas` array. */
void sh_create_arena(int index, int id, sicm_device *device) {
  int i;

  if((arenas[index] != NULL) && (get_alloc_site(arenas[index], id) != -1)) {
    return;
  }

  /* Keep track of which arena we chose for this site */
  tree_insert(site_arenas, id, index);

  /* If we've already created this arena */
  if(arenas[index] != NULL) {

    /* Add the site to the arena */
    if(arenas[index]->num_alloc_sites == max_sites_per_arena) {
      fprintf(stderr, "Sites: ");
      for(i = 0; i < arenas[index]->num_alloc_sites; i++) {
        fprintf(stderr, "%d ", arenas[index]->alloc_sites[i]);
      }
      fprintf(stderr, "\n");
      fprintf(stderr, "Tried to allocate %d sites into an arena. Increase SH_MAX_SITES_PER_ARENA.\n", max_sites_per_arena + 1);
      exit(1);
    }
    arenas[index]->alloc_sites[arenas[index]->num_alloc_sites] = id;
    arenas[index]->num_alloc_sites++;

    return;
  }

  /* Put an upper bound on the indices that need to be searched */
  if(index > max_index) {
    max_index = index;
  }

  if(!device) {
    device = default_device;
  }

  /* Create the arena if it doesn't exist */
  arenas[index] = calloc(1, sizeof(arena_info));
  arenas[index]->index = index;
  arenas[index]->accesses = 0;
  arenas[index]->alloc_sites = malloc(sizeof(int) * max_sites_per_arena);
  arenas[index]->alloc_sites[0] = id;
  arenas[index]->num_alloc_sites = 1;
  arenas[index]->rss = 0;
  arenas[index]->peak_rss = 0;
  arenas[index]->avg_rss = 0;
  arenas[index]->arena = sicm_arena_create(0, device);
}

/* Adds an extent to the `extents` array. */
void sh_create_extent(void *start, void *end) {
  int thread_index, arena_index;

  /* Get this thread's current arena index from `pending_indices` */
  thread_index = get_thread_index();
  arena_index = pending_indices[thread_index];

  /* A extent allocation is happening without an sh_alloc... */
  if(arena_index == -1) {
    fprintf(stderr, "Unknown extent allocation. Aborting.\n");
    exit(1);
  }

  if(should_profile_rss && (should_profile_one != -1) && (get_alloc_site(arenas[arena_index], should_profile_one) != -1)) {
    /* If we're profiling RSS and this is the site that we're isolating */
    extent_arr_insert(rss_extents, start, end, arenas[arena_index]);
  }

  if(pthread_rwlock_wrlock(&extents_lock) != 0) {
    fprintf(stderr, "Failed to acquire read/write lock. Aborting.\n");
    exit(1);
  }
  extent_arr_insert(extents, start, end, arenas[arena_index]);
  if(pthread_rwlock_unlock(&extents_lock) != 0) {
    fprintf(stderr, "Failed to unlock read/write lock. Aborting.\n");
    exit(1);
  }
}

int get_site_arena(int id) {
  tree_it(int, int) it;
  int ret;

  it = tree_lookup(site_arenas, id);
  if(tree_it_good(it)) {
    /* We've already got an arena for this site, use it */
    ret = tree_it_val(it);
  } else {
    /* We need to create an arena for this site. Grab the next
     * available arena and increment.
     */
    ret = __sync_fetch_and_add(&arena_counter, 1);
  }

  return ret;
}

/* Gets the device that this site should go onto from the site_nodes tree */
sicm_device *get_site_device(int id) {
  sicm_device *device;
  tree_it(int, deviceptr) it;

  it = tree_lookup(site_nodes, id);
  if(tree_it_good(it)) {
    /* This site was found in the guidance file.  Use its device pointer to
     * find if this device has already got an arena.
     */
    device = tree_it_val(it);
  } else {
    /* Site's not in the guidance file. Use the default device. */
    device = default_device;
  }

  return device;
}

/* Chooses an arena for the per-device arena layouts. */
int get_device_arena(int id, sicm_device **device) {
  tree_it(deviceptr, int) devit;
  int ret;

  *device = get_site_device(id);
  devit = tree_lookup(device_arenas, *device);
  if(tree_it_good(devit)) {
    /* This device already has an arena associated with it. Return the
     * index of that arena.
     */
    ret = tree_it_val(devit);
  } else {
    /* Choose an arena index for this device.  We're going to assume here
     * that we never get a device that didn't exist on initialization.
     * Remember our choice.
     */
    ret = max_index + 1;
    tree_insert(device_arenas, *device, ret);
  }

  return ret;
}

/* Gets the index that the allocation site should go into */
int get_arena_index(int id) {
  int ret, thread_index;
  sicm_device *device;
  tree_it(int, int) it;

  thread_index = get_thread_index();

  ret = 0;
  device = NULL;
  switch(layout) {
    case SHARED_ONE_ARENA:
      ret = 0;
      break;
    case EXCLUSIVE_ONE_ARENA:
      ret = thread_index + 1;
      break;
    case SHARED_DEVICE_ARENAS:
      ret = get_device_arena(id, &device);
      break;
    case EXCLUSIVE_DEVICE_ARENAS:
      /* Same as SHARED_DEVICE_ARENAS, except per thread */
      ret = get_device_arena(id, &device);
      ret = (thread_index * arenas_per_thread) + ret;
      break;
    case SHARED_SITE_ARENAS:
      ret = get_site_arena(id);
      device = get_site_device(id);
      /* Special case for profiling */
      if(profile_one_device && (id == should_profile_one)) {
        /* If the site is the one we're profiling, isolate it */
        device = profile_one_device;
      }
      break;
    case EXCLUSIVE_SITE_ARENAS:
      ret = (thread_index * arenas_per_thread) + id;
      break;
    case EXCLUSIVE_TWO_DEVICE_ARENAS:
      ret = get_device_arena(id, &device);
      ret = (thread_index * arenas_per_thread) + ret;
    case EXCLUSIVE_FOUR_DEVICE_ARENAS:
      ret = get_device_arena(id, &device);
      ret = (thread_index * arenas_per_thread) + ret;
      break;
    default:
      fprintf(stderr, "Invalid arena layout. Aborting.\n");
      exit(1);
      break;
  };

  if(ret > max_arenas) {
    /* Fit the index to the maximum number of arenas */
    ret = ret % max_arenas;
  }

  pthread_mutex_lock(&arena_lock);
  pending_indices[thread_index] = ret;
  sh_create_arena(ret, id, device);
  pthread_mutex_unlock(&arena_lock);

  return ret;
}

void* sh_realloc(int id, void *ptr, size_t sz) {
  int   index;
  void *ret;

  if(layout == INVALID_LAYOUT) {
    ret = realloc(ptr, sz);
  } else {
    index = get_arena_index(id);
    ret = sicm_arena_realloc(arenas[index]->arena, ptr, sz);
  }

  if (should_run_rdspy) {
    sh_rdspy_realloc(ptr, ret, sz, id);
  }

  return ret;
}

/* Accepts an allocation site ID and a size, does the allocation */
void* sh_alloc(int id, size_t sz) {
  int index;
  void *ret;

  if((layout == INVALID_LAYOUT) || !sz) {
    ret = je_malloc(sz);
  } else {
    index = get_arena_index(id);
    ret = sicm_arena_alloc(arenas[index]->arena, sz);
  }

  if (should_run_rdspy) {
    sh_rdspy_alloc(ret, sz, id);
  }
  
  return ret;
}

/* Accepts an allocation site ID and a size, does the allocation */
void* sh_aligned_alloc(int id, size_t alignment, size_t sz) {
  int index;
  void *ret;

  if(!sz) {
    return NULL;
  }

  if((layout == INVALID_LAYOUT) || !sz) {
    ret = je_aligned_alloc(alignment, sz);
  } else {
    index = get_arena_index(id);
    ret = sicm_arena_alloc_aligned(arenas[index]->arena, sz, alignment);
  }

  if (should_run_rdspy) {
    sh_rdspy_alloc(ret, sz, id);
  }
  
  return ret;
}

int sh_posix_memalign(int id, void **ptr, size_t alignment, size_t sz) {
  *ptr = sh_aligned_alloc(id, alignment, sz);
  return 0;
}

void *sh_memalign(int id, size_t alignment, size_t sz) {
  return sh_aligned_alloc(id, alignment, sz);
}

void* sh_calloc(int id, size_t num, size_t sz) {
  void *ptr;
  size_t i;

  ptr = sh_alloc(id, num * sz);
  memset(ptr, 0, num * sz);
  return ptr;
}

void sh_free(void* ptr) {
  if (should_run_rdspy) {
      sh_rdspy_free(ptr);
  }

  if(!ptr) {
    return;
  }

  if(layout == INVALID_LAYOUT) {
    je_free(ptr);
  } else {
    sicm_free(ptr);
  }
}

__attribute__((constructor))
void sh_init() {
  int i;
  long size;

  device_list = sicm_init();

  /* Get the number of NUMA nodes with memory, since we ignore huge pages with
   * the DEVICE arena layouts */
  num_numa_nodes = 0;
  for(i = 0; i <= numa_max_node(); i++) {
    size = -1;
    if ((numa_node_size(i, &size) != -1) && size) {
      num_numa_nodes++;
    }
  }

  arena_counter = 0;
  site_arenas = tree_make(int, int);
  site_nodes = tree_make(int, deviceptr);
  device_arenas = tree_make(deviceptr, int);
  set_options();
  
  if(layout != INVALID_LAYOUT) {
    /* `arenas` is a pseudo-two-dimensional array, first dimension is per-thread.
     * Second dimension is one for each arena that each thread will have.
     * If the arena layout isn't per-thread (`EXCLUSIVE_`), arenas_per_thread is just
     * the total number of arenas.
     */
    switch(layout) {
      case SHARED_ONE_ARENA:
      case SHARED_DEVICE_ARENAS:
      case SHARED_SITE_ARENAS:
        arenas = (arena_info **) calloc(arenas_per_thread, sizeof(arena_info *));
        break;
      case EXCLUSIVE_SITE_ARENAS:
      case EXCLUSIVE_ONE_ARENA:
      case EXCLUSIVE_DEVICE_ARENAS:
      case EXCLUSIVE_TWO_DEVICE_ARENAS:
      case EXCLUSIVE_FOUR_DEVICE_ARENAS:
        arenas = (arena_info **) calloc(max_threads * arenas_per_thread, sizeof(arena_info *));
        break;
    }

    /* Initialize the extents array.
     * If we're just doing MBI on one site, initialize a new array that has extents from just that site.
     * If we're profiling all sites, rss_extents is just all extents.
     */
    extents = extent_arr_init();
    if(should_profile_rss) {
      rss_extents = extents;
      if(should_profile_one != -1) {
        rss_extents = extent_arr_init();
      }
    }

    /* Stores the index into the `arenas` array for each thread */
    pthread_key_create(&thread_key, NULL);
    thread_indices = (int *) malloc(max_threads * sizeof(int));
    orig_thread_indices = thread_indices;
    max_thread_indices = orig_thread_indices + max_threads;
    for(i = 0; i < max_threads; i++) {
      thread_indices[i] = i;
    }
    pthread_setspecific(thread_key, (void *) thread_indices);
    thread_indices++;

    /* Stores an index into `arenas` for the extent hooks */
    pending_indices = (int *) malloc(max_threads * sizeof(int));
    for(i = 0; i < max_threads; i++) {
      pending_indices[i] = -1;
    }

    /* Set the arena allocator's callback function */
    sicm_extent_alloc_callback = &sh_create_extent;

    sh_start_profile_thread();
  }
  
  if (should_run_rdspy) {
    sh_rdspy_init(max_threads, num_static_sites);
  }
}

__attribute__((destructor))
void sh_terminate() {
  size_t i;

  /* Clean up the low-level interface */
  sicm_fini(&device_list);

  if(layout != INVALID_LAYOUT) {

    /* Clean up the profiler */
    if(should_profile_all || (should_profile_one != -1) || should_profile_rss) {
      sh_stop_profile_thread();
    }

    /* Clean up the arenas */
    for(i = 0; i <= max_index; i++) {
      if(!arenas[i]) continue;
      sicm_arena_destroy(arenas[i]->arena);
      free(arenas[i]);
    }
    free(arenas);

    free(pending_indices);
    free(orig_thread_indices);
    extent_arr_free(extents);
  }

  if (should_run_rdspy) {
      sh_rdspy_terminate();
  }
}
