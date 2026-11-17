/* SPDX-License-Identifier: Apache-2.0 */
/* Exercise session lifetime through the real deferred-event consumers. */
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include "vppcom.c"

static unsigned checks;
static svm_msg_q_t app_queue;
#define CHECK(expr)                                                                                \
  do                                                                                               \
    {                                                                                              \
      checks++;                                                                                    \
      if (!(expr))                                                                                 \
	{                                                                                          \
	  fprintf (stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr);                              \
	  exit (1);                                                                                \
	}                                                                                          \
    }                                                                                              \
  while (0)

static void
consume_closed (vcl_worker_t *wrk, session_event_t *e, int use_select)
{
  struct epoll_event ev = { .events = 0x12345678 };
  uword read = 0, write = 0, except = 0;
  u32 count = 0;

  if (use_select == 2)
    {
      vcl_poll_t pollfd = { 0 };
      CHECK (vppcom_poll (&pollfd, 0, 0) == 0);
    }
  else if (use_select)
    {
      vcl_select_handle_mq_event (wrk, e, BITS (uword), &read, &write, &except, &count);
      CHECK (read == 0 && write == 0 && except == 0);
    }
  else
    {
      vcl_epoll_wait_handle_mq_event (wrk, e, &ev, &count);
      CHECK (ev.events == 0x12345678);
    }
  CHECK (count == 0);
}

static void
check_close_order (vcl_worker_t *wrk, u8 type, int use_select, int detach_first, int do_disconnect)
{
  session_event_t event = { .event_type = type };
  vcl_session_t *s = vcl_session_alloc (wrk), *other;
  session_cleanup_msg_t cleanup = { .type = SESSION_CLEANUP_SESSION, .handle = 42 };
  u32 index = s->session_index;

  s->session_state = VCL_STATE_READY;
  s->vpp_handle = cleanup.handle;
  vcl_session_set_attr (s, VCL_SESS_ATTR_NONBLOCK);
  vcl_session_table_add_vpp_handle (wrk, cleanup.handle, index);
  ((session_disconnected_msg_t *) event.data)->handle = cleanup.handle;
  vcl_handle_mq_event (wrk, &event);
  CHECK (vec_len (wrk->unhandled_evts_vector) == 1);
  CHECK (s->flags & VCL_SESSION_F_PENDING_DISCONNECT);

  if (detach_first)
    {
      vcl_session_cleanup_handler (wrk, &cleanup);
      CHECK (s->session_state == VCL_STATE_DETACHED);
    }

  /* VLS passes do_disconnect=0 while another fork subscriber remains. This
   * must retain the pool slot just like a normal close of a detached session. */
  CHECK (vcl_session_cleanup (wrk, s, vcl_session_handle (s), do_disconnect) == 0);
  CHECK (vcl_session_get (wrk, index) == s);
  CHECK (s->session_state == VCL_STATE_CLOSED);
  CHECK (s->flags & VCL_SESSION_F_PENDING_FREE);
  CHECK (!vcl_session_get_w_vpp_handle (wrk, cleanup.handle));
  other = vcl_session_alloc (wrk);
  CHECK (other->session_index != index);
  vcl_session_free (wrk, other);

  consume_closed (wrk, &wrk->unhandled_evts_vector[0], use_select);
  CHECK (vcl_session_get (wrk, index) == 0);
  vec_reset_length (wrk->unhandled_evts_vector);
  CHECK (pool_elts (wrk->sessions) == 0);
}

static void
check_pending_connect_close (vcl_worker_t *wrk, int do_disconnect)
{
  vcl_session_t *s = vcl_session_alloc (wrk), *other;
  u32 index = s->session_index;
  s->session_state = VCL_STATE_UPDATED;
  s->flags = VCL_SESSION_F_CONNECTED;
  s->vpp_handle = VCL_INVALID_SESSION_HANDLE;
  vcl_session_set_attr (s, VCL_SESS_ATTR_NONBLOCK);
  CHECK (vcl_session_cleanup (wrk, s, vcl_session_handle (s), do_disconnect) == 0);
  CHECK (vcl_session_get (wrk, index) == s && s->session_state == VCL_STATE_CLOSED);
  other = vcl_session_alloc (wrk);
  CHECK (other->session_index != index);
  vcl_session_free (wrk, other);
  session_connected_msg_t reply = { .context = index, .retval = SESSION_E_REFUSED };
  CHECK (vcl_session_connected_handler (wrk, &reply) == VCL_INVALID_SESSION_INDEX);
  CHECK (vcl_session_get (wrk, index) == 0);
}

static void
check_failed_connect (vcl_worker_t *wrk)
{
  vcl_session_t *s = vcl_session_alloc (wrk);
  s->session_state = VCL_STATE_DETACHED;
  s->vpp_handle = VCL_INVALID_SESSION_HANDLE;
  s->vpp_error = SESSION_E_REFUSED;
  vcl_session_set_attr (s, VCL_SESS_ATTR_NONBLOCK);
  /* A late old-handle notification must not turn this failed, FIFO-less
   * connect back into an open VPP_CLOSING session, as in the qbit core. */
  vcl_session_table_add_vpp_handle (wrk, 42, s->session_index);
  session_event_t e = { .event_type = SESSION_CTRL_EVT_DISCONNECTED };
  ((session_disconnected_msg_t *) e.data)->handle = 42;
  vcl_handle_mq_event (wrk, &e);
  CHECK (s->session_state == VCL_STATE_DETACHED);
  CHECK (vec_len (wrk->unhandled_evts_vector) == 0);
  CHECK (vcl_session_disconnected_handler (wrk, (void *) e.data) == 0);
  e.event_type = SESSION_CTRL_EVT_RESET;
  vcl_handle_mq_event (wrk, &e);
  CHECK (s->session_state == VCL_STATE_DETACHED);
  CHECK (vcl_session_reset_handler (wrk, (void *) e.data) == VCL_INVALID_SESSION_INDEX);
  vcl_poll_t pollfd = { .sh = vcl_session_handle (s), .events = POLLOUT };
  CHECK (vppcom_poll (&pollfd, 1, 0) == 1 && pollfd.revents == POLLERR);
  vcl_session_table_del_vpp_handle (wrk, 42);
  vcl_session_free (wrk, s);

  s = vcl_session_alloc (wrk);
  s->session_state = VCL_STATE_VPP_CLOSING;
  CHECK (vcl_session_write_ready (s) == VPPCOM_ENOTCONN);
  CHECK (vcl_session_read_ready (s) == VPPCOM_ENOTCONN);
  CHECK (vcl_session_read_ready2 (s) == VPPCOM_ENOTCONN);
  char byte = 0;
  vppcom_data_segment_t segment;
  CHECK (vppcom_session_read (vcl_session_handle (s), &byte, 1) == VPPCOM_ENOTCONN);
  CHECK (vppcom_session_read_segments (vcl_session_handle (s), &segment, 1, 1) == VPPCOM_ENOTCONN);
  CHECK (vppcom_session_write (vcl_session_handle (s), &byte, 1) == VPPCOM_ENOTCONN);
  vcl_session_free (wrk, s);
}

static void
check_detached_connect (vcl_worker_t *wrk)
{
  vcl_session_t *s = vcl_session_alloc (wrk);
  u8 ip[4] = { 192, 0, 2, 1 };
  vppcom_endpt_t peer = { .is_ip4 = 1, .ip = ip, .port = clib_host_to_net_u16 (12345) };
  u8 state = s->session_state;
  CHECK (wrk->ctrl_mq == 0);
  CHECK (vppcom_session_connect (vcl_session_handle (s), &peer) == VPPCOM_ENOTCONN);
  CHECK (s->session_state == state);
  CHECK (!(s->flags & VCL_SESSION_F_CONNECTED));
  vcl_session_free (wrk, s);
}

int
main (void)
{
  struct rlimit no_core = { 0, 0 };
  vcl_worker_t *wrk;

  CHECK (setrlimit (RLIMIT_CORE, &no_core) == 0);
  clib_mem_init (0, 32 << 20);
  pool_get_zero (vcm->workers, wrk);
  pool_alloc (wrk->sessions, 8);
  vcl_set_worker_index (0);
  clib_time_init (&wrk->clib_time);
  svm_msg_q_ring_cfg_t ring = { 8, sizeof (session_event_t), 0 };
  svm_msg_q_cfg_t cfg = { .consumer_pid = ~0, .q_nitems = 8, .n_rings = 1, .ring_cfgs = &ring };
  svm_msg_q_shared_t *shared = svm_msg_q_alloc (&cfg);
  CHECK (shared != NULL);
  svm_msg_q_attach (&app_queue, shared);
  wrk->app_event_queue = &app_queue;

  check_pending_connect_close (wrk, 0);
  check_pending_connect_close (wrk, 1);
  check_failed_connect (wrk);
  check_detached_connect (wrk);

  for (int use_select = 0; use_select < 3; use_select++)
    for (int reset = 0; reset < 2; reset++)
      {
	u8 type = reset ? SESSION_CTRL_EVT_RESET : SESSION_CTRL_EVT_DISCONNECTED;
	check_close_order (wrk, type, use_select, 1, 0);
	check_close_order (wrk, type, use_select, 0, 0);
	check_close_order (wrk, type, use_select, 1, 1);
      }

  /* Without a pending notification, shared-session close frees immediately. */
  vcl_session_t *s = vcl_session_alloc (wrk);
  u32 index = s->session_index;
  s->session_state = VCL_STATE_READY;
  CHECK (vcl_session_cleanup (wrk, s, vcl_session_handle (s), 0) == 0);
  CHECK (vcl_session_get (wrk, index) == 0);

  /* An open session still reports its normal half-close notification. */
  s = vcl_session_alloc (wrk);
  s->session_state = VCL_STATE_VPP_CLOSING;
  s->flags = VCL_SESSION_F_IS_VEP_SESSION | VCL_SESSION_F_PENDING_DISCONNECT;
  s->vep.lt_next = VCL_INVALID_SESSION_INDEX;
  s->vep.ev.events = EPOLLRDHUP | EPOLLHUP | EPOLLET;
  s->vep.ev.data.u64 = 0xabcdef;
  session_event_t event = { .event_type = SESSION_CTRL_EVT_DISCONNECTED,
			    .postponed = 1,
			    .session_index = s->session_index };
  struct epoll_event ev = { 0 };
  u32 count = 0;
  vec_add1 (wrk->unhandled_evts_vector, event);
  vcl_poll_t pollfd = { 0 };
  CHECK (vppcom_poll (&pollfd, 0, 0) == 0);
  CHECK (vec_len (wrk->unhandled_evts_vector) == 1);
  CHECK (s->flags & VCL_SESSION_F_PENDING_DISCONNECT);
  vcl_epoll_wait_handle_mq_event (wrk, &event, &ev, &count);
  CHECK (count == 1 && ev.events == EPOLLRDHUP && ev.data.u64 == 0xabcdef);
  CHECK (!(s->flags & VCL_SESSION_F_PENDING_DISCONNECT));
  vcl_session_free (wrk, s);
  printf ("PASS: %u deferred session lifetime checks\n", checks);
  return 0;
}
