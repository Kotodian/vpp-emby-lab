/* SPDX-License-Identifier: Apache-2.0 */
/* Exercise session lifetime through the real deferred-event consumers. */
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include "vppcom.c"

static unsigned checks;
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

  if (use_select)
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

  for (int use_select = 0; use_select < 2; use_select++)
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
  vcl_epoll_wait_handle_mq_event (wrk, &event, &ev, &count);
  CHECK (count == 1 && ev.events == EPOLLRDHUP && ev.data.u64 == 0xabcdef);
  CHECK (!(s->flags & VCL_SESSION_F_PENDING_DISCONNECT));
  vcl_session_free (wrk, s);
  printf ("PASS: %u deferred session lifetime checks\n", checks);
  return 0;
}
