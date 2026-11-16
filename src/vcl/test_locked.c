/* SPDX-License-Identifier: Apache-2.0 */
/* Exercise real VLS locking through its public API. Only the VCL transport
 * endpoints are fixtures; no VPP process or network socket is needed. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>
#include <vcl/vcl_locked.h>
#include <vcl/vcl_private.h>

static atomic_uint checks;
static atomic_int accept_waited, release_accept, reader_started, reader_entered;
static vls_handle_t listener, writer;
static svm_fifo_t tx_fifo;
static svm_fifo_shared_t tx_shared;

#define CHECK(expr)                                                                                \
  do                                                                                               \
    {                                                                                              \
      atomic_fetch_add (&checks, 1);                                                               \
      if (!(expr))                                                                                 \
	{                                                                                          \
	  fprintf (stderr, "%s:%d: %s failed\n", __FILE__, __LINE__, #expr);                       \
	  abort ();                                                                                \
	}                                                                                          \
    }                                                                                              \
  while (0)

/* Make releasing another thread's MQ mutex fail deterministically instead
 * of corrupting glibc's owner/user accounting until a later lock aborts. */
int
pthread_mutex_init (pthread_mutex_t *mutex, const pthread_mutexattr_t *attr)
{
  int (*init) (pthread_mutex_t *, const pthread_mutexattr_t *) =
    dlsym (RTLD_NEXT, "pthread_mutex_init");
  pthread_mutexattr_t checked;
  int rv;
  if (attr)
    return init (mutex, attr);
  CHECK (pthread_mutexattr_init (&checked) == 0);
  CHECK (pthread_mutexattr_settype (&checked, PTHREAD_MUTEX_ERRORCHECK) == 0);
  rv = init (mutex, &checked);
  CHECK (pthread_mutexattr_destroy (&checked) == 0);
  return rv;
}

int
pthread_mutex_lock (pthread_mutex_t *mutex)
{
  int (*lock) (pthread_mutex_t *) = dlsym (RTLD_NEXT, "pthread_mutex_lock");
  int rv = lock (mutex);
  CHECK (rv == 0);
  return rv;
}

int
pthread_mutex_unlock (pthread_mutex_t *mutex)
{
  int (*unlock) (pthread_mutex_t *) = dlsym (RTLD_NEXT, "pthread_mutex_unlock");
  int rv = unlock (mutex);
  CHECK (rv == 0);
  return rv;
}

int
vppcom_app_create (const char *name)
{
  vcl_worker_t *wrk;
  clib_mem_init (0, 32 << 20);
  vcm->cfg.max_workers = 1;
  pool_get_zero (vcm->workers, wrk);
  vcl_set_worker_index (0);
  tx_fifo.shr = &tx_shared;
  tx_shared.size = 8192;
  return 0;
}

int
vppcom_session_create (uint8_t proto, uint8_t nonblocking)
{
  vcl_session_t *s = vcl_session_alloc (vcl_worker_get_current ());
  s->session_type = proto;
  s->session_state = VCL_STATE_READY;
  s->tx_fifo = &tx_fifo;
  if (nonblocking)
    vcl_session_set_attr (s, VCL_SESS_ATTR_NONBLOCK);
  return vcl_session_handle (s);
}

int
vppcom_session_accept (uint32_t sh, vppcom_endpt_t *ep, uint32_t flags)
{
  vcl_worker_t *wrk = vcl_worker_get_current ();
  /* Same pre/post wait callbacks as vcl_worker_wait_mq when a blocking
   * accept has no queued connection. Its session-pool writer lock must
   * survive the wait, including repeated spurious wakeups. */
  for (int i = 0; i < 8; i++)
    {
      wrk->pre_wait_fn (VCL_INVALID_SESSION_INDEX);
      wrk->post_wait_fn (VCL_INVALID_SESSION_INDEX);
    }
  atomic_store (&accept_waited, 1);
  while (!atomic_load (&release_accept))
    usleep (1000);
  return VPPCOM_EAGAIN;
}

int
vppcom_session_write (uint32_t sh, void *buf, size_t len)
{
  atomic_store (&reader_entered, 1);
  return len;
}

static void *
accept_connection (void *arg)
{
  CHECK (vls_accept (listener, NULL, 0) == VPPCOM_EAGAIN);
  return NULL;
}

static void *
write_connection (void *arg)
{
  atomic_store (&reader_started, 1);
  CHECK (vls_write (writer, "x", 1) == 1);
  return NULL;
}

int
main (void)
{
  pthread_t accept_thread, reader_thread;
  alarm (10);
  CHECK (vls_app_create ("vls-lock-test") == 0);
  listener = vls_create (VPPCOM_PROTO_TCP, 0);
  writer = vls_create (VPPCOM_PROTO_TCP, 1);
  CHECK (listener >= 0 && writer >= 0 && listener != writer);
  CHECK (pthread_create (&accept_thread, NULL, accept_connection, NULL) == 0);
  while (!atomic_load (&accept_waited))
    usleep (1000);
  CHECK (pthread_create (&reader_thread, NULL, write_connection, NULL) == 0);
  while (!atomic_load (&reader_started))
    usleep (1000);
  usleep (20000);
  CHECK (!atomic_load (&reader_entered));
  atomic_store (&release_accept, 1);
  CHECK (pthread_join (accept_thread, NULL) == 0);
  CHECK (pthread_join (reader_thread, NULL) == 0);
  CHECK (atomic_load (&reader_entered));
  printf ("PASS: %u VLS blocking-accept lock checks\n", atomic_load (&checks));
  return 0;
}
