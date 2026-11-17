/* SPDX-License-Identifier: Apache-2.0 */
/* Reproduce fork cleanup after independent VCL/VLS worker pool reuse. The
 * private cleanup entry point is exercised without a VPP daemon or sockets. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <unistd.h>
#include "vcl_locked.c"

static unsigned int checks, cleaned;
static unsigned int flushed;

#define CHECK(expr)                                                                                \
  do                                                                                               \
    {                                                                                              \
      checks++;                                                                                    \
      if (!(expr))                                                                                 \
	{                                                                                          \
	  fprintf (stderr, "%s:%d: %s failed\n", __FILE__, __LINE__, #expr);                       \
	  exit (1);                                                                                \
	}                                                                                          \
    }                                                                                              \
  while (0)

/* Only transport teardown is replaced. Keep the real VLS cleanup and pool
 * ownership rules, including a VCL worker whose child already freed VLS. */
void
vcl_worker_cleanup (vcl_worker_t *wrk, u8 notify_vpp)
{
  CHECK (notify_vpp == 0);
  cleaned++;
  clib_memset (wrk, 0xab, sizeof (*wrk));
  pool_put (vcm->workers, wrk);
}

void
vcl_flush_mq_events (void)
{
  CHECK ((vlspt->locks_acq & (VLS_MT_LOCK_MQ | VLS_MT_WLOCK_SPOOL)) ==
	 (VLS_MT_LOCK_MQ | VLS_MT_WLOCK_SPOOL));
  CHECK (pthread_mutex_trylock (&vlsl->vls_mt_mq_mlock) == EBUSY);
  flushed++;
}

static vcl_worker_t *
new_vcl_worker (void)
{
  vcl_worker_t *wrk;
  pool_get_zero (vcm->workers, wrk);
  wrk->wrk_index = wrk - vcm->workers;
  wrk->forked_child = ~0;
  return wrk;
}

static void
reap_worker (u32 index)
{
  vls_worker_t *parent = vls_worker_get_current ();
  vec_add1 (parent->pending_vcl_wrk_cleanup, index);
  vls_handle_pending_wrk_cleanup ();
  CHECK (vec_len (parent->pending_vcl_wrk_cleanup) == 0);
  CHECK (vcl_worker_get_if_valid (index) == 0);
  CHECK (vls_worker_get_current () == parent);
}

int
main (void)
{
  vcl_worker_t *parent, *child, *other;
  vls_worker_t *child_vls;
  u32 child_index;
  struct rlimit no_core = { 0, 0 };

  CHECK (setrlimit (RLIMIT_CORE, &no_core) == 0);
  alarm (10);
  clib_mem_init (0, 32 << 20);
  pool_alloc (vcm->workers, 8);
  parent = new_vcl_worker ();
  vcl_set_worker_index (parent->wrk_index);
  vcm->cfg.vpp_app_socket_api = (u8 *) "/unused-fixture-socket";
  vlsm = clib_mem_alloc (sizeof (*vlsm));
  clib_memset (vlsm, 0, sizeof (*vlsm));
  pool_alloc (vlsm->workers, 8);
  vls_worker_alloc ();
  vlsl->vls_wrk_index = 0;

  /* The production core had a live VCL worker and no matching VLS worker. */
  child = new_vcl_worker ();
  reap_worker (child->wrk_index);
  CHECK (pool_elts (vlsm->workers) == 1);

  /* Child exit already released its VLS entry before parent SIGCHLD cleanup. */
  child = new_vcl_worker ();
  child_index = child->wrk_index;
  vcl_set_worker_index (child_index);
  child_vls = vls_worker_alloc ();
  vls_worker_free (child_vls);
  vcl_set_worker_index (0);
  reap_worker (child_index);
  CHECK (pool_elts (vlsm->workers) == 1);

  /* VLS slot 1 belongs to VCL worker 2. Reaping VCL worker 1 must keep it. */
  child = new_vcl_worker ();
  child_index = child->wrk_index;
  other = new_vcl_worker ();
  vcl_set_worker_index (other->wrk_index);
  child_vls = vls_worker_alloc ();
  vcl_set_worker_index (0);
  CHECK (child_vls - vlsm->workers == child_index);
  reap_worker (child_index);
  CHECK (pool_elts (vlsm->workers) == 2);
  CHECK (child_vls->vcl_wrk_index == other->wrk_index);
  reap_worker (other->wrk_index);
  CHECK (pool_elts (vlsm->workers) == 1);
  CHECK (cleaned == 4);
  vls_handle_pending_wrk_cleanup ();
  CHECK (cleaned == 4);
  /* fork used to flush a live network thread's MQ without its mutex.
   * Also cover a child that finishes before the parent handler runs. */
  vls_mt_locks_init ();
  clib_rwlock_init (&vlsl->vls_pool_lock);
  vlsl->vls_mt_needs_locks = 1;
  vls_app_pre_fork ();
  CHECK (flushed == 1 && vcm->forking == 1);
  __atomic_store_n (&vcm->forking, 0, __ATOMIC_RELEASE);
  vls_app_fork_parent_handler ();
  CHECK (vlspt->locks_acq == 0);
  CHECK (pthread_mutex_trylock (&vlsl->vls_mt_mq_mlock) == 0);
  CHECK (pthread_mutex_unlock (&vlsl->vls_mt_mq_mlock) == 0);
  printf ("PASS: %u VLS fork cleanup checks\n", checks);
  return 0;
}
