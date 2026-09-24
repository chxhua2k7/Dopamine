#pragma once
// Minimal libdispatch stand-in for the Linux test harness: dispatch_after runs
// the block on a detached thread after sleeping.
#include <stdint.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <Block.h>
#define NSEC_PER_SEC 1000000000ull
#define NSEC_PER_MSEC 1000000ull
#define DISPATCH_TIME_NOW 0
#define QOS_CLASS_UTILITY 0x11
typedef uint64_t dispatch_time_t;
typedef void *dispatch_queue_t;
typedef void (^dispatch_block_t)(void);
static inline dispatch_time_t dispatch_time(dispatch_time_t when, int64_t delta) { (void)when; return (dispatch_time_t)delta; }
static inline dispatch_queue_t dispatch_get_global_queue(long q, unsigned long f) { (void)q; (void)f; return (void *)1; }
struct _da_ctx { dispatch_time_t delay; dispatch_block_t block; };
static void *_da_thread(void *arg) { struct _da_ctx *c = arg; usleep((useconds_t)(c->delay / 1000)); c->block(); Block_release(c->block); free(c); return NULL; }
static inline void dispatch_after(dispatch_time_t when, dispatch_queue_t q, dispatch_block_t block) {
	(void)q; struct _da_ctx *c = malloc(sizeof(*c)); c->delay = when; c->block = Block_copy(block);
	pthread_t t; pthread_create(&t, NULL, _da_thread, c); pthread_detach(t);
}
