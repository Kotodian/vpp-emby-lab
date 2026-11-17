/* SPDX-License-Identifier: Apache-2.0 */
/* Offline POSIX/Asio regression tests. VLS transport calls are replaced by
 * local fixtures. No VPP process or IP data transfer is used; the only Linux
 * IP sockets are query-only descriptors for interface metadata.
 * The default high VLS fd range keeps ordinary file descriptors separate. */
#define _GNU_SOURCE
#include <limits.h>
#include <net/if.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vcl/vcl_locked.h>
#include <vcl/vcl_private.h>

static vcl_session_t *sessions;
static svm_fifo_t rx_fifo, tx_fifo;
static svm_fifo_shared_t rx_shared, tx_shared;
static unsigned int checks;
static svm_msg_q_t accept_queue;
static int sends, receives, ready, send_flags, kernel_epfd;
static char sent[128];
static const char *packets[] = { "hello world", "next" };

#define CHECK(expr)                                                                                \
  do                                                                                               \
    {                                                                                              \
      checks++;                                                                                    \
      if (!(expr))                                                                                 \
	{                                                                                          \
	  fprintf (stderr, "%s:%d: %s failed (errno=%d)\n", __FILE__, __LINE__, #expr, errno);     \
	  exit (1);                                                                                \
	}                                                                                          \
    }                                                                                              \
  while (0)

/* Executable symbols interpose VLS for this test only. The LDP constructor
 * calls this fixture instead of attaching to VPP. Real VCL attribute and
 * nonblocking FIFO code is exercised below. */
int
vls_app_create (char *name)
{
  vcl_worker_t *wrk;
  vcl_session_t *s;
  clib_mem_init (0, 32 << 20);
  pool_get_zero (vcm->workers, wrk);
  vcl_set_worker_index (0);
  for (int i = 0; i < 2; i++)
    {
      pool_get_zero (wrk->sessions, s);
      s->session_index = i;
      s->session_type = i ? VPPCOM_PROTO_UDP : VPPCOM_PROTO_TCP;
      s->session_state = VCL_STATE_READY;
      s->transport.is_ip4 = VPPCOM_IS_IP4;
      s->rx_fifo = &rx_fifo;
      s->tx_fifo = &tx_fifo;
    }
  sessions = wrk->sessions;
  rx_fifo.shr = &rx_shared;
  rx_fifo.signals = &rx_shared.signals;
  tx_fifo.shr = &tx_shared;
  tx_fifo.signals = &tx_shared.signals;
  rx_shared.size = tx_shared.size = 8192;
  return 0;
}

vls_handle_t
vls_create (uint8_t proto, uint8_t nonblocking)
{
  return proto == VPPCOM_PROTO_UDP ? 1 : 0;
}

int
vls_close (vls_handle_t vlsh)
{
  return 0;
}

int
vls_get_libc_epfd (vls_handle_t vlsh)
{
  return vlsh == 2 ? kernel_epfd : 0;
}

int
vls_set_libc_epfd (vls_handle_t vlsh, int fd)
{
  kernel_epfd = fd;
  return 0;
}

unsigned char
vls_use_eventfd (void)
{
  return 1;
}

int
vls_epoll_wait (vls_handle_t vlsh, struct epoll_event *events, int n, double timeout)
{
  return 0;
}

int
vls_attr (vls_handle_t vlsh, uint32_t op, void *buf, uint32_t *len)
{
  return vppcom_session_attr (vlsh, op, buf, len);
}

vcl_session_handle_t
vlsh_to_sh (vls_handle_t vlsh)
{
  return vlsh;
}

int
vls_poll (vcl_poll_t *vp, uint32_t n, double timeout)
{
  for (u32 i = 0; i < n; i++)
    vp[i].revents = ready ? vp[i].events & (POLLIN | POLLOUT) : 0;
  /* Older VCL counts events; LDP must count descriptors. */
  return ready ? 2 * n : 0;
}

int
vls_sendto (vls_handle_t vlsh, void *buf, int len, int flags, vppcom_endpt_t *ep)
{
  sends++;
  send_flags = flags;
  if (len <= sizeof (sent))
    memcpy (sent, buf, len);
  return len;
}

ssize_t
vls_recvfrom (vls_handle_t vlsh, void *buf, uint32_t len, int flags, vppcom_endpt_t *ep)
{
  size_t size;
  if (receives == ARRAY_LEN (packets))
    return -EAGAIN;
  size = clib_min (len, strlen (packets[receives]));
  memcpy (buf, packets[receives], size);
  if (!(flags & MSG_PEEK))
    receives++;
  return size;
}

static void
test_options (int tcp, int udp)
{
  struct
  {
    int value;
    u32 canary;
  } opt = { 0, 0xabcdef12 };
  socklen_t len = sizeof (opt.value);
  int value = 4096;

  CHECK (getsockopt (tcp, SOL_SOCKET, SO_RCVBUF, &opt.value, &len) == 0);
  CHECK (len == sizeof (int) && opt.canary == 0xabcdef12);
  CHECK (opt.value == 16384);
  CHECK (setsockopt (tcp, SOL_SOCKET, SO_RCVBUF, &value, sizeof (value)) == 0);
  CHECK (getsockopt (tcp, SOL_SOCKET, SO_RCVBUF, &opt.value, &len) == 0);
  CHECK (opt.value == 8192 && opt.canary == 0xabcdef12);
  CHECK (setsockopt (tcp, SOL_SOCKET, SO_SNDBUF, &value, sizeof (value)) == 0);
  CHECK (getsockopt (tcp, SOL_SOCKET, SO_SNDBUF, &opt.value, &len) == 0);
  CHECK (opt.value == 8192);
  value = -1;
  CHECK (setsockopt (tcp, SOL_SOCKET, SO_RCVBUF, &value, sizeof (value)) == -1 && errno == EINVAL);
  CHECK (setsockopt (tcp, SOL_SOCKET, SO_SNDBUF, &value, 1) == -1 && errno == EINVAL);
  CHECK (getsockopt (tcp, SOL_SOCKET, SO_TYPE, &opt.value, &len) == 0 && opt.value == SOCK_STREAM);
  CHECK (getsockopt (udp, SOL_SOCKET, SO_TYPE, &opt.value, &len) == 0 && opt.value == SOCK_DGRAM);
  CHECK (getsockopt (tcp, SOL_SOCKET, SO_PROTOCOL, &opt.value, &len) == 0 &&
	 opt.value == IPPROTO_TCP);
  CHECK (getsockopt (udp, SOL_SOCKET, SO_PROTOCOL, &opt.value, &len) == 0 &&
	 opt.value == IPPROTO_UDP);
  CHECK (getsockopt (tcp, SOL_SOCKET, SO_TYPE, &opt.value, &(socklen_t) { 1 }) == -1);
  sessions[0].vpp_error = SESSION_E_REFUSED;
  CHECK (getsockopt (tcp, SOL_SOCKET, SO_ERROR, &opt.value, &len) == 0 &&
	 opt.value == ECONNREFUSED);
  CHECK (getsockopt (tcp, SOL_SOCKET, SO_ERROR, &opt.value, &len) == 0 && opt.value == 0);
  rx_shared.tail = 123;
  CHECK (ioctl (tcp, FIONREAD, &value) == 0 && value == 123);
  CHECK (ioctl (tcp, FIONREAD, NULL) == -1 && errno == EFAULT);
  rx_shared.tail = 0;
  tx_shared.tail = 37;
  CHECK (ioctl (tcp, TIOCOUTQ, &value) == 0 && value == 37);
  tx_shared.tail = 0;
  CHECK (fcntl (tcp, F_GETFL) == O_RDWR);
  struct ifreq request = { 0 };
  strcpy (request.ifr_name, "lo");
  CHECK (ioctl (udp, SIOCGIFFLAGS, &request) == 0);
  CHECK (request.ifr_flags & IFF_LOOPBACK);
  CHECK (ioctl (udp, SIOCGIFMTU, &request) == 0 && request.ifr_mtu > 0);
}

static void
signal_handler (int signo)
{
}

static void
test_poll (int tcp)
{
  int pipefd[2];
  struct pollfd fds[3];
  struct timespec zero = { 0, 0 }, invalid = { 0, 1000000000 };
  sigset_t blocked, previous;
  char byte = 'x';

  CHECK (pipe (pipefd) == 0);
  fds[0] = (struct pollfd) { tcp, POLLIN | POLLOUT, 0 };
  fds[1] = (struct pollfd) { pipefd[0], POLLIN, 0 };
  fds[2] = (struct pollfd) { -1, POLLIN, POLLIN };
  ready = 1;
  CHECK (ppoll (fds, 3, &zero, NULL) == 1);
  CHECK (fds[0].fd == tcp && fds[0].revents == (POLLIN | POLLOUT));
  CHECK (fds[2].revents == 0);
  CHECK (write (pipefd[1], &byte, 1) == 1);
  CHECK (ppoll (fds, 3, &zero, NULL) == 2);
  CHECK (fds[1].revents & POLLIN);
  CHECK (poll (fds, 3, 0) == 2);
  ready = 0;
  CHECK (read (pipefd[0], &byte, 1) == 1);
  CHECK (ppoll (fds, 3, &zero, NULL) == 0);
  CHECK (ppoll (fds, 3, &invalid, NULL) == -1 && errno == EINVAL);
  CHECK (ppoll (NULL, 1, &zero, NULL) == -1 && errno == EFAULT);
  CHECK (ppoll (NULL, 0, &zero, NULL) == 0);
  CHECK (signal (SIGUSR1, signal_handler) != SIG_ERR);
  sigemptyset (&blocked);
  sigaddset (&blocked, SIGUSR1);
  CHECK (pthread_sigmask (SIG_BLOCK, &blocked, &previous) == 0);
  CHECK (raise (SIGUSR1) == 0);
  CHECK (ppoll (fds, 3, NULL, &previous) == -1 && errno == EINTR);
  CHECK (pthread_sigmask (SIG_SETMASK, &previous, NULL) == 0);
  /* Kernel-only ppoll is the path used by Qt's eventfd/pipe loop. */
  CHECK (write (pipefd[1], &byte, 1) == 1);
  CHECK (ppoll (&fds[1], 1, &zero, NULL) == 1);
  CHECK (read (pipefd[0], &byte, 1) == 1);
  CHECK (ppoll (&fds[1], 1, &zero, NULL) == 0);
  CHECK (ppoll (&fds[1], 2, &zero, NULL) == 0);

  vcl_worker_get_current ()->mqs_epfd = pipefd[0];
  struct epoll_event event;
  CHECK (pthread_sigmask (SIG_BLOCK, &blocked, &previous) == 0);
  CHECK (raise (SIGUSR1) == 0);
  CHECK (epoll_pwait (tcp + 2, &event, 1, -1, &previous) == -1 && errno == EINTR);
  CHECK (pthread_sigmask (SIG_SETMASK, &previous, NULL) == 0);
  CHECK (close (tcp + 2) == 0);
  CHECK (close (pipefd[0]) == 0 && close (pipefd[1]) == 0);
}

static void
test_datagrams (int udp)
{
  struct iovec iov[2] = { { "hello ", 6 }, { "world", 5 } };
  struct msghdr msg = { .msg_iov = iov, .msg_iovlen = 2 };
  char first[6], second[5];
  CHECK (sendmsg (udp, &msg, MSG_DONTWAIT) == 11);
  CHECK (sends == 1 && send_flags == MSG_DONTWAIT);
  CHECK (memcmp (sent, "hello world", 11) == 0);
  iov[0] = (struct iovec) { first, sizeof (first) };
  iov[1] = (struct iovec) { second, sizeof (second) };
  CHECK (recvmsg (udp, &msg, MSG_PEEK | MSG_DONTWAIT) == 11);
  CHECK (receives == 0 && memcmp (first, "hello ", 6) == 0);
  CHECK (memcmp (second, "world", 5) == 0);
  CHECK (recvmsg (udp, &msg, MSG_DONTWAIT) == 11 && receives == 1);
  CHECK (recvmsg (udp, &msg, MSG_DONTWAIT) == 4 && receives == 2);
  CHECK (recvmsg (udp, &msg, MSG_DONTWAIT) == -1 && errno == EAGAIN);
  receives = 0;
  iov[0].iov_len = 2;
  msg.msg_iovlen = 1;
  CHECK (recvmsg (udp, &msg, MSG_TRUNC) == 11 && (msg.msg_flags & MSG_TRUNC));
  CHECK (recvmsg (udp, &msg, 0) == 2 && (msg.msg_flags & MSG_TRUNC));
  CHECK (receives == 2);
  int pktinfo = 1;
  CHECK (setsockopt (udp, IPPROTO_IP, IP_PKTINFO, &pktinfo, sizeof (pktinfo)) == 0);
  char control[1];
  msg.msg_control = control;
  msg.msg_controllen = sizeof (control);
  receives = 0;
  CHECK (recvmsg (udp, &msg, 0) == 2 && (msg.msg_flags & MSG_CTRUNC));
  CHECK (msg.msg_controllen == 0);
}

/* A TCP listener has no transport event queue. Pending accepts must be
 * rejected through the queue carried by each accept notification. */
int
vcl_segment_attach_mq (uword handle, uword offset, u32 index, svm_msg_q_t **mq)
{
  if (offset != 0x1234 || index != 1)
    return -1;
  *mq = &accept_queue;
  return 0;
}

static void
test_listener_close (void)
{
  vcl_worker_t *wrk = vcl_worker_get_current ();
  vcl_session_t *s;
  vcl_session_msg_t *pending;
  svm_msg_q_ring_cfg_t rings[2] = {
    { 8, sizeof (session_event_t), 0 },
    { 8, sizeof (session_event_t), 0 },
  };
  svm_msg_q_cfg_t cfg = { .consumer_pid = ~0, .q_nitems = 16, .n_rings = 2, .ring_cfgs = rings };
  svm_msg_q_shared_t *shared = svm_msg_q_alloc (&cfg);
  CHECK (shared != NULL);
  svm_msg_q_attach (&accept_queue, shared);
  wrk->ctrl_mq = &accept_queue;
  pool_get_zero (wrk->sessions, s);
  s->session_index = s - wrk->sessions;
  s->session_state = VCL_STATE_LISTEN;
  s->session_type = VPPCOM_PROTO_TCP;
  s->vpp_handle = 0x9876;
  CHECK (s->vpp_evt_q == NULL);
  clib_fifo_add2 (s->accept_evts_fifo, pending);
  memset (pending, 0, sizeof (*pending));
  pending->accepted_msg.handle = 0x5678;
  pending->accepted_msg.vpp_event_queue_address = 0x1234;
  pending->accepted_msg.mq_index = 1;
  CHECK (vppcom_session_close (s->session_index) == 0);
  svm_msg_q_msg_t message;
  CHECK (svm_msg_q_sub (&accept_queue, &message, SVM_Q_NOWAIT, 0) == 0);
  session_event_t *evt = svm_msg_q_msg_data (&accept_queue, &message);
  CHECK (evt->event_type == SESSION_CTRL_EVT_ACCEPTED_REPLY);
  session_accepted_reply_msg_t *reply = (void *) evt->data;
  CHECK (reply->handle == 0x5678 && reply->retval == SESSION_E_REFUSED);
  svm_msg_q_free_msg (&accept_queue, &message);
  CHECK (svm_msg_q_sub (&accept_queue, &message, SVM_Q_NOWAIT, 0) == 0);
  evt = svm_msg_q_msg_data (&accept_queue, &message);
  CHECK (evt->event_type == SESSION_CTRL_EVT_UNLISTEN);
  svm_msg_q_free_msg (&accept_queue, &message);

  /* A Qt poll must not steal a pending Boost.Asio epoll notification. */
  wrk->app_event_queue = &accept_queue;
  clib_time_init (&wrk->clib_time);
  vcl_poll_t empty_poll = { .sh = 0, .events = POLLIN };
  CHECK (vppcom_poll (&empty_poll, 1, 0) == 0);
  session_event_t pending_io = { .event_type = SESSION_IO_EVT_RX,
				 .session_index = s->session_index };
  s->flags |= VCL_SESSION_F_IS_VEP_SESSION;
  vec_add1 (wrk->unhandled_evts_vector, pending_io);
  pending_io.session_index = 0;
  vec_add1 (wrk->unhandled_evts_vector, pending_io);
  vcl_poll_t vp = { .sh = 0, .events = POLLIN };
  CHECK (vppcom_poll (&vp, 1, 0) == 0);
  CHECK (vec_len (wrk->unhandled_evts_vector) == 1);
  CHECK (wrk->unhandled_evts_vector[0].session_index == s->session_index);

  /* Reading a connecting socket must not turn a pending connect into
   * POLLERR while the write side correctly reports no readiness. */
  sessions[0].session_state = VCL_STATE_UPDATED;
  vp.events = POLLIN | POLLOUT;
  CHECK (vppcom_poll (&vp, 1, 0) == 0);
  CHECK (vp.revents == 0);
  sessions[0].session_state = VCL_STATE_READY;
  CHECK (vppcom_poll (&vp, 1, 0) > 0);
  CHECK (vp.revents == POLLOUT);

  /* A drained stream with peer FIN is readable until the application
   * consumes EOF and closes it, including cut-through WebUI connections. */
  sessions[0].session_state = VCL_STATE_VPP_CLOSING;
  vp.events = POLLIN;
  CHECK (vppcom_poll (&vp, 1, 0) == 1);
  CHECK (vp.revents == POLLIN);
  char byte;
  CHECK (vppcom_session_read (0, &byte, 1) == 0);
  CHECK (vppcom_poll (&vp, 1, 0) == 1);
  sessions[0].session_state = VCL_STATE_READY;
}

static void
test_nonblocking (void)
{
  char byte;
  /* Exercise the real VCL read/write paths on an empty/full FIFO. A missing
   * MSG_DONTWAIT would attempt to wait on a nonexistent VPP message queue. */
  CHECK (vppcom_session_recvfrom (0, &byte, 1, MSG_DONTWAIT, NULL) == -EAGAIN);
  tx_shared.tail = tx_shared.size;
  CHECK (vppcom_session_sendto (0, &byte, 1, MSG_DONTWAIT, NULL) == -EAGAIN);
  tx_shared.tail = 0;
  CHECK (!vcl_session_has_attr (&sessions[0], VCL_SESS_ATTR_NONBLOCK));
}

int
main (void)
{
  int tcp, udp, fd, highfd;
  alarm (10);
  tcp = socket (AF_INET, SOCK_STREAM, 0);
  udp = socket (AF_INET, SOCK_DGRAM, 0);
  CHECK (tcp == (1 << 30) && udp == (1 << 30) + 1);
  test_options (tcp, udp);
  test_poll (tcp);
  test_datagrams (udp);
  test_nonblocking ();
  int family;
  socklen_t family_len = sizeof (family);
  int ipv6 = socket (AF_INET6, SOCK_DGRAM, 0);
  CHECK (getsockopt (ipv6, SOL_SOCKET, SO_DOMAIN, &family, &family_len) == 0 && family == AF_INET6);
  CHECK (close (ipv6) == 0);
  fd = open ("/dev/null", O_RDONLY);
  CHECK (fd >= 0);
  highfd = fcntl (fd, F_DUPFD, 64);
  CHECK (highfd >= 64);
  CHECK (read (highfd, &(char) { 0 }, 1) == 0);
  CHECK (close (highfd) == 0 && close (fd) == 0);
  CHECK (close (tcp) == 0 && close (udp) == 0);
  test_listener_close ();
  printf ("PASS: %u offline LDP/VCL socket checks\n", checks);
  return 0;
}
