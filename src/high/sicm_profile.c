#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/time.h>
#include <sys/syscall.h>
#include <signal.h>

#define SICM_RUNTIME 1
#include "sicm_profile.h"
#include "sicm_parsing.h"

profiler prof;
static int global_signal;

/* Returns 0 if "a" is bigger, 1 if "b" is bigger */
char timespec_cmp(struct timespec *a, struct timespec *b) {
  if (a->tv_sec == b->tv_sec) {
    if(a->tv_nsec > b->tv_nsec) {
      return 0;
    } else {
      return 1;
    }
  } else if(a->tv_sec > b->tv_sec) {
    return 0;
  } else {
    return 1;
  }
}

/* Subtracts two timespec structs from each other. Assumes stop is
 * larger than start.
 */
void timespec_diff(struct timespec *start, struct timespec *stop,
                   struct timespec *result) {
  if ((stop->tv_nsec - start->tv_nsec) < 0) {
    result->tv_sec = stop->tv_sec - start->tv_sec - 1;
    result->tv_nsec = stop->tv_nsec - start->tv_nsec + 1000000000;
  } else {
    result->tv_sec = stop->tv_sec - start->tv_sec;
    result->tv_nsec = stop->tv_nsec - start->tv_nsec;
  }
}

/* Runs when an arena has already been created, but the runtime library
   has added an allocation site to the arena. */
void add_site_profile(int index, int site_id) {
  arena_profile *aprof;
  pthread_rwlock_wrlock(&prof.profile_lock);

  aprof = get_arena_prof(index);
  aprof->alloc_sites[aprof->num_alloc_sites] = site_id;
  aprof->num_alloc_sites++;

  pthread_rwlock_unlock(&prof.profile_lock);
}

/* Runs when a new arena is created. Allocates room to store
   profiling information about this arena. */
void create_arena_profile(int index, int site_id) {
  arena_profile *aprof;

  pthread_rwlock_wrlock(&prof.profile_lock);

  aprof = orig_calloc(1, sizeof(arena_profile));

  if(profopts.should_profile_all) {
    profile_all_arena_init(&(aprof->profile_all));
  }
  if(profopts.should_profile_rss) {
    profile_rss_arena_init(&(aprof->profile_rss));
  }
  if(profopts.should_profile_extent_size) {
    profile_extent_size_arena_init(&(aprof->profile_extent_size));
  }
  if(profopts.should_profile_allocs) {
    profile_allocs_arena_init(&(aprof->profile_allocs));
  }
  if(profopts.should_profile_online) {
    profile_online_arena_init(&(aprof->profile_online));
  }

  /* Creates a profile for this arena at the current interval */
  aprof->index = index;
  aprof->num_alloc_sites = 1;
  aprof->alloc_sites = orig_malloc(sizeof(int) * tracker.max_sites_per_arena);
  aprof->alloc_sites[0] = site_id;
  prof.profile->this_interval.arenas[index] = aprof;
  prof.profile->this_interval.num_arenas++;

  pthread_rwlock_unlock(&prof.profile_lock);
}

void end_interval() {
  /* Signal the master thread that we're done. Should only get called
   * by separated profiling threads. */
  if(profopts.should_profile_separate_threads) {
    pthread_mutex_lock(&prof.mtx);
    prof.threads_finished++;
    pthread_cond_signal(&prof.cond);
    pthread_mutex_unlock(&prof.mtx);
  }
}

/* This is the signal handler for the Master thread, so
 * it does this on every interval.
 */
void profile_master_interval(int s) {
  struct timespec actual;
  size_t i, n, x;
  char copy;
  double elapsed_time;

  /* Convenience pointers */
  arena_profile *aprof;
  arena_info *arena;
  profile_thread *profthread;
  
  /* Here, we're checking to see if the time between this interval and
     the previous one is too short. If it is, this is likely a queued-up
     signal caused by an interval that took too long. In some cases,
     profiling threads can take up to 10 seconds to complete, and in that
     span of time, hundreds or thousands of timer signals could have been
     queued up. We want to prevent that from happening, so ignore a signal
     that occurs quicker than it should. */
  clock_gettime(CLOCK_MONOTONIC, &(prof.start));
  timespec_diff(&(prof.end), &(prof.start), &actual);
  elapsed_time = actual.tv_sec + (((double) actual.tv_nsec) / 1000000000);
  if(elapsed_time < (prof.target - (prof.target * 10 / 100))) {
    /* It's too soon since the last interval. */
    clock_gettime(CLOCK_MONOTONIC, &(prof.end));
    return;
  }

  pthread_rwlock_wrlock(&(prof.profile_lock));

  if(profopts.should_profile_separate_threads) {
    /* If we're separating the profiling threads, notify them that an interval has started. */
    for(i = 0; i < prof.num_profile_threads; i++) {
      profthread = &prof.profile_threads[i];
      if(profthread->skipped_intervals == (profthread->skip_intervals - 1)) {
        /* This thread doesn't get skipped */
        pthread_kill(prof.profile_threads[i].id, prof.profile_threads[i].signal);
        profthread->skipped_intervals = 0;
      } else {
        /* This thread gets skipped */
        pthread_kill(prof.profile_threads[i].id, prof.profile_threads[i].skip_signal);
        profthread->skipped_intervals++;
      }
    }

    /* Wait for the threads to do their bit */
    pthread_mutex_lock(&prof.mtx);
    while(1) {
      if(prof.threads_finished) {
        /* At least one thread is finished, check if it's all of them */
        copy = prof.threads_finished;
        pthread_mutex_unlock(&prof.mtx);
        if(copy == prof.num_profile_threads) {
          /* They're all done. */
          pthread_mutex_lock(&prof.mtx);
          prof.threads_finished = 0;
          break;
        }
        /* At least one was done, but not all of them. Continue waiting. */
        pthread_mutex_lock(&prof.mtx);
      } else {
        /* Wait for at least one thread to signal us */
        pthread_cond_wait(&prof.cond, &prof.mtx);
      }
    }
    pthread_mutex_unlock(&prof.mtx);
  } else {
    /* If we're not separating the profiling threads, just call these functions
     * from the current thread. */
    for(i = 0; i < prof.num_profile_threads; i++) {
      profthread = &prof.profile_threads[i];
      if(profthread->skipped_intervals == (profthread->skip_intervals - 1)) {
        /* This thread doesn't get skipped */
        (*profthread->interval_func)(0);
        profthread->skipped_intervals = 0;
      } else {
        /* This thread gets skipped */
        (*profthread->skip_interval_func)(0);
        profthread->skipped_intervals++;
      }
    }
  }

  arena_arr_for(i) {
    prof_check_good(arena, aprof, i);

    if(profopts.should_profile_all) {
      profile_all_post_interval(aprof);
    }
    if(profopts.should_profile_rss) {
      profile_rss_post_interval(aprof);
    }
    if(profopts.should_profile_extent_size) {
      profile_extent_size_post_interval(aprof);
    }
    if(profopts.should_profile_allocs) {
      profile_allocs_post_interval(aprof);
    }
    if(profopts.should_profile_online) {
      profile_online_post_interval(aprof);
    }
  }
  if(profopts.should_profile_bw) {
    profile_bw_post_interval();
  }
  if(profopts.should_profile_latency) {
    profile_latency_post_interval();
  }
  
  /* Store the time that this interval took */
  clock_gettime(CLOCK_MONOTONIC, &(prof.end));
  timespec_diff(&(prof.start), &(prof.end), &actual);
  prof.profile->this_interval.time = actual.tv_sec + (((double) actual.tv_nsec) / 1000000000);
  
  /* End the interval */
  if(prof.profile->num_intervals) {
    /* If we've had at least one interval of profiling already,
       store that pointer in `prev_interval` */
    prof.prev_interval = &(prof.profile->intervals[prof.profile->num_intervals - 1]);
  }
  prof.profile->num_intervals++;
  copy_interval_profile(prof.profile->num_intervals - 1);
  prof.cur_interval = &(prof.profile->intervals[prof.profile->num_intervals - 1]);

  pthread_rwlock_unlock(&(prof.profile_lock));
  
  /* We need this timer to actually end outside out of the lock */
  clock_gettime(CLOCK_MONOTONIC, &(prof.end));
}

/* Stops the master thread */
void profile_master_stop(int s) {
  size_t i;
  profile_thread *profthread;

  timer_delete(prof.timerid);

  if(profopts.should_profile_separate_threads) {
    /* Cancel all threads. Since threads can only be running while the master
     * thread is in a different signal handler from this one, it's impossible that they're
     * in an interval currently.
     */
    for(i = 0; i < prof.num_profile_threads; i++) {
      profthread = &prof.profile_threads[i];
      pthread_cancel(profthread->id);
      pthread_join(profthread->id, NULL);
    }
  }

  pthread_exit(NULL);
}

void setup_profile_thread(void *(*main)(void *), /* Spinning loop function */
                          void (*interval)(int), /* Per-interval function */
                          void (*skip_interval)(int), /* Per-interval skip function */
                          unsigned long skip_intervals) {
  struct sigaction sa;
  profile_thread *profthread;

  /* Add a new profile_thread struct for it */
  prof.num_profile_threads++;
  prof.profile_threads = orig_realloc(prof.profile_threads, sizeof(profile_thread) * prof.num_profile_threads);
  profthread = &(prof.profile_threads[prof.num_profile_threads - 1]);

  if(profopts.should_profile_separate_threads) {
    /* Start the thread */
    pthread_create(&(profthread->id), NULL, main, NULL);

    /* Set up the signal handler */
    profthread->signal = global_signal;
    sa.sa_flags = 0;
    sa.sa_handler = interval;
    sigemptyset(&sa.sa_mask);
    if(sigaction(profthread->signal, &sa, NULL) == -1) {
      fprintf(stderr, "Error creating signal handler for signal %d. Aborting: %s\n", profthread->signal, strerror(errno));
      exit(1);
    }
    global_signal++;

    /* Set up the signal handler for what gets called if we're skipping this interval */
    profthread->skip_signal = global_signal;
    sa.sa_flags = 0;
    sa.sa_handler = skip_interval;
    sigemptyset(&sa.sa_mask);
    if(sigaction(profthread->skip_signal, &sa, NULL) == -1) {
      fprintf(stderr, "Error creating signal handler for signal %d. Aborting: %s\n", profthread->skip_signal, strerror(errno));
      exit(1);
    }
    global_signal++;
  }

  profthread->interval_func      = interval;
  profthread->skip_interval_func = skip_interval;
  profthread->skipped_intervals  = 0;
  profthread->skip_intervals     = skip_intervals;
}

/* This is the Master thread, it keeps track of intervals
 * and starts/stops the profiling threads. It has a timer
 * which signals it at a certain interval. Each time this
 * happens, it notifies the profiling threads.
 */
void *profile_master(void *a) {
  struct sigevent sev;
  struct sigaction sa;
  struct itimerspec its;
  long long frequency;
  sigset_t mask;
  pid_t tid;

  if(profopts.should_profile_all) {
    setup_profile_thread(&profile_all,
                         &profile_all_interval,
                         &profile_all_skip_interval,
                         profopts.profile_all_skip_intervals);
  }
  if(profopts.should_profile_bw) {
    setup_profile_thread(&profile_bw,
                         &profile_bw_interval,
                         &profile_bw_skip_interval,
                         profopts.profile_bw_skip_intervals);
  }
  if(profopts.should_profile_latency) {
    setup_profile_thread(&profile_latency,
                         &profile_latency_interval,
                         &profile_latency_skip_interval,
                         profopts.profile_latency_skip_intervals);
  }
  if(profopts.should_profile_rss) {
    setup_profile_thread(&profile_rss,
                         &profile_rss_interval,
                         &profile_rss_skip_interval,
                         profopts.profile_rss_skip_intervals);
  }
  if(profopts.should_profile_extent_size) {
    setup_profile_thread(&profile_extent_size,
                         &profile_extent_size_interval,
                         &profile_extent_size_skip_interval,
                         profopts.profile_extent_size_skip_intervals);
  }
  if(profopts.should_profile_allocs) {
    setup_profile_thread(&profile_allocs,
                         &profile_allocs_interval,
                         &profile_allocs_skip_interval,
                         profopts.profile_allocs_skip_intervals);
  }
  if(profopts.should_profile_online) {
    setup_profile_thread(&profile_online,
                         &profile_online_interval,
                         &profile_online_skip_interval,
                         profopts.profile_online_skip_intervals);
  }

  /* Initialize synchronization primitives */
  pthread_mutex_init(&prof.mtx, NULL);
  pthread_cond_init(&prof.cond, NULL);

  /* Set up a signal handler for the master */
  sa.sa_flags = 0;
  sa.sa_handler = profile_master_interval;
  sigemptyset(&sa.sa_mask);
  sigaddset(&sa.sa_mask, prof.stop_signal); /* Stop signal should block until an interval is finished */
  if(sigaction(prof.master_signal, &sa, NULL) == -1) {
    fprintf(stderr, "Error creating signal handler. Aborting.\n");
    exit(1);
  }

  /* Block the signal for a bit */
  sigemptyset(&mask);
  sigaddset(&mask, prof.master_signal);
  if(sigprocmask(SIG_SETMASK, &mask, NULL) == -1) {
    fprintf(stderr, "Error blocking signal. Aborting.\n");
    exit(1);
  }

  /* Create the timer */
  tid = syscall(SYS_gettid);
  sev.sigev_notify = SIGEV_THREAD_ID;
  sev.sigev_signo = prof.master_signal;
  sev.sigev_value.sival_ptr = &prof.timerid;
  sev._sigev_un._tid = tid;
  if(timer_create(CLOCK_REALTIME, &sev, &prof.timerid) == -1) {
    fprintf(stderr, "Error creating timer. Aborting.\n");
    exit(1);
  }
  
  /* Store how long the interval should take */
  prof.target = ((double) profopts.profile_rate_nseconds) / 1000000000;

  /* Set the timer */
  its.it_value.tv_sec     = profopts.profile_rate_nseconds / 1000000000;
  its.it_value.tv_nsec    = profopts.profile_rate_nseconds % 1000000000;
  its.it_interval.tv_sec  = its.it_value.tv_sec;
  its.it_interval.tv_nsec = its.it_value.tv_nsec;
  if(timer_settime(prof.timerid, 0, &its, NULL) == -1) {
    fprintf(stderr, "Error setting the timer. Aborting.\n");
    exit(1);
  }
  
  /* Initialize this time */
  clock_gettime(CLOCK_MONOTONIC, &(prof.end));

  /* Unblock the signal */
  if(sigprocmask(SIG_UNBLOCK, &mask, NULL) == -1) {
    fprintf(stderr, "Error unblocking signal. Aborting.\n");
    exit(1);
  }

  /* Wait for either the timer to signal us to start a new interval,
   * or for the main thread to signal us to stop.
   */
  while(1) {}
}

void initialize_profiling() {
  size_t i;

  pthread_rwlock_init(&(prof.profile_lock), NULL);

  /* Initialize the structs that store the profiling information */
  prof.profile = orig_calloc(1, sizeof(application_profile));

  /* We'll add profiling to this array when an interval happens */
  prof.profile->num_intervals = 0;
  prof.profile->intervals = NULL;
  prof.cur_interval = NULL;
  prof.prev_interval = NULL;
  
  /* Set flags for what type of profiling we'll store */
  if(profopts.should_profile_all) {
    prof.profile->has_profile_all = 1;
  }
  if(profopts.should_profile_allocs) {
    prof.profile->has_profile_allocs = 1;
  }
  if(profopts.should_profile_extent_size) {
    prof.profile->has_profile_extent_size = 1;
  }
  if(profopts.should_profile_rss) {
    prof.profile->has_profile_rss = 1;
  }
  if(profopts.should_profile_online) {
    prof.profile->has_profile_online = 1;
  }
  if(profopts.should_profile_bw) {
    prof.profile->has_profile_bw = 1;
  }
  if(profopts.should_profile_latency) {
    prof.profile->has_profile_latency = 1;
  }
  if(profopts.profile_bw_relative) {
    prof.profile->has_profile_bw_relative = 1;
  }

  /* Stores the current interval's profiling */
  prof.profile->this_interval.num_arenas = 0;
  prof.profile->this_interval.arenas = orig_calloc(tracker.max_arenas, sizeof(arena_profile *));

  /* Store the profile_all event strings */
  prof.profile->num_profile_all_events = profopts.num_profile_all_events;
  prof.profile->profile_all_events = orig_calloc(prof.profile->num_profile_all_events, sizeof(char *));
  for(i = 0; i < profopts.num_profile_all_events; i++) {
    prof.profile->profile_all_events[i] = orig_malloc((strlen(profopts.profile_all_events[i]) + 1) * sizeof(char));
    strcpy(prof.profile->profile_all_events[i], profopts.profile_all_events[i]);
  }
  
  /* Store which sockets we profiled */
  prof.profile->num_profile_skts = profopts.num_profile_skt_cpus;
  prof.profile->profile_skts = orig_calloc(prof.profile->num_profile_skts, sizeof(int));
  for(i = 0; i < profopts.num_profile_skt_cpus; i++) {
    prof.profile->profile_skts[i] = profopts.profile_skts[i];
  }
  
  prof.threads_finished = 0;

  /* The signal that will stop the master thread */
  global_signal = SIGRTMIN;
  prof.stop_signal = global_signal;
  global_signal++;

  /* The signal that the master thread will use to tell itself
   * (via a timer) when the next interval should start */
  prof.master_signal = global_signal;
  global_signal++;

  /* All of this initialization HAS to happen in the main SICM thread.
   * If it's not, the `perf_event_open` system call won't profile
   * the current thread, but instead will only profile the thread that
   * it was run in.
   */
  if(profopts.should_profile_all) {
    profile_all_init();
  }
  if(profopts.should_profile_bw) {
    profile_bw_init();
  }
  if(profopts.should_profile_latency) {
    profile_latency_init();
  }
  if(profopts.should_profile_rss) {
    profile_rss_init();
  }
  if(profopts.should_profile_extent_size) {
    profile_extent_size_init();
  }
  if(profopts.should_profile_allocs) {
    profile_allocs_init();
  }
  if(profopts.should_profile_online) {
    profile_online_init();
  }
}

void sh_start_profile_master_thread() {
  struct sigaction sa;

  /* This initializes the values that the threads will need to do their profiling,
   * including perf events, file descriptors, etc.
   */
  initialize_profiling();

  /* Set up the signal that we'll use to stop the master thread */
  sa.sa_flags = 0;
  sa.sa_handler = profile_master_stop;
  sigemptyset(&sa.sa_mask);
  sigaddset(&sa.sa_mask, prof.master_signal); /* Block the interval signal while the stop signal handler is running */
  if(sigaction(prof.stop_signal, &sa, NULL) == -1) {
    fprintf(stderr, "Error creating master stop signal handler. Aborting.\n");
    exit(1);
  }

  /* Start the master thread */
  pthread_create(&prof.master_id, NULL, &profile_master, NULL);
}

void deinitialize_profiling() {
  if(profopts.should_profile_all) {
    profile_all_deinit();
  }
  if(profopts.should_profile_bw) {
    profile_bw_deinit();
  }
  if(profopts.should_profile_latency) {
    profile_latency_deinit();
  }
  if(profopts.should_profile_rss) {
    profile_rss_deinit();
  }
  if(profopts.should_profile_extent_size) {
    profile_extent_size_deinit();
  }
  if(profopts.should_profile_allocs) {
    profile_allocs_deinit();
  }
  if(profopts.should_profile_online) {
    profile_online_deinit();
  }
}

void sh_stop_profile_master_thread() {
  /* Tell the master thread to stop */
  pthread_kill(prof.master_id, prof.stop_signal);
  pthread_join(prof.master_id, NULL);

  if(profopts.profile_output_file) {
    sh_print_profiling(prof.profile, profopts.profile_output_file);
  }
  deinitialize_profiling();
}
