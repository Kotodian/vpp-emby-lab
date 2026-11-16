/* SPDX-License-Identifier: Apache-2.0 */
/* Socket-activated userspace relay for kernel control sockets and VCL.
 * Unlike splice-based relays, all forwarded bytes use read/write, which LDP
 * can route to VCL. Each direction retains partial writes independently. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define CLIENTS 64
#define BUFSIZE 16384

typedef struct
{
  int fd[2], connecting, eof[2], shut[2];
  size_t pos[2], len[2];
  char data[2][BUFSIZE];
  time_t last_io;
} connection_t;
static connection_t connections[CLIENTS];

static void
close_connection (connection_t *c)
{
  for (int side = 0; side < 2; side++)
    if (c->fd[side] >= 0)
      close (c->fd[side]);
  memset (c, 0, sizeof (*c));
  c->fd[0] = c->fd[1] = -1;
}

int
main (int argc, char **argv)
{
  struct sockaddr_in backend = { .sin_family = AF_INET };
  struct pollfd fds[1 + CLIENTS * 2];
  int port;
  if (argc != 3 || (port = atoi (argv[2])) <= 0 || port > 65535 ||
      inet_pton (AF_INET, argv[1], &backend.sin_addr) != 1)
    {
      fprintf (stderr, "usage: %s <backend-ip4> <port> (listener on fd 3)\n", argv[0]);
      return 1;
    }
  const char *listen_pid = getenv ("LISTEN_PID");
  const char *listen_fds = getenv ("LISTEN_FDS");
  if (!listen_pid || !listen_fds || atol (listen_pid) != getpid () || atoi (listen_fds) < 1)
    {
      fprintf (stderr, "proxy requires systemd socket activation\n");
      return 1;
    }
  backend.sin_port = htons (port);
  signal (SIGPIPE, SIG_IGN);
  if (fcntl (3, F_SETFL, O_NONBLOCK) < 0)
    {
      perror ("listener nonblocking");
      return 1;
    }
  for (int i = 0; i < CLIENTS; i++)
    connections[i].fd[0] = connections[i].fd[1] = -1;

  for (;;)
    {
      fds[0] = (struct pollfd) { 3, POLLIN, 0 };
      for (int i = 0; i < CLIENTS; i++)
	for (int side = 0; side < 2; side++)
	  {
	    connection_t *c = &connections[i];
	    short events = 0;
	    if (!c->eof[side] && c->len[side] == 0)
	      events |= POLLIN;
	    if (c->len[1 - side] || (side == 1 && c->connecting))
	      events |= POLLOUT;
	    fds[1 + i * 2 + side] = (struct pollfd) { c->fd[side], events, 0 };
	  }
      int count = poll (fds, 1 + CLIENTS * 2, 1000);
      if (count < 0)
	{
	  if (errno == EINTR)
	    continue;
	  perror ("proxy poll");
	  return 1;
	}
      if (fds[0].revents & POLLIN)
	{
	  int client = accept4 (3, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
	  if (client >= 0)
	    {
	      int i;
	      for (i = 0; i < CLIENTS && connections[i].fd[0] >= 0; i++)
		;
	      if (i == CLIENTS)
		close (client);
	      else
		{
		  connection_t *c = &connections[i];
		  memset (c, 0, sizeof (*c));
		  c->fd[0] = client;
		  c->fd[1] = socket (AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
		  c->last_io = time (NULL);
		  if (c->fd[1] < 0)
		    close_connection (c);
		  else if (connect (c->fd[1], (void *) &backend, sizeof (backend)) < 0)
		    {
		      if (errno == EINPROGRESS)
			c->connecting = 1;
		      else
			{
			  perror ("proxy backend connect");
			  close_connection (c);
			}
		    }
		}
	    }
	}
      for (int i = 0; i < CLIENTS; i++)
	{
	  connection_t *c = &connections[i];
	  if (c->fd[0] < 0)
	    continue;
	  if (c->connecting && fds[2 + i * 2].revents)
	    {
	      int error = 0;
	      socklen_t size = sizeof (error);
	      if (getsockopt (c->fd[1], SOL_SOCKET, SO_ERROR, &error, &size) < 0 || error)
		{
		  fprintf (stderr, "proxy backend completion: %s\n",
			   strerror (error ? error : errno));
		  close_connection (c);
		  continue;
		}
	      c->connecting = 0;
	    }
	  int failed = 0;
	  for (int side = 0; side < 2; side++)
	    {
	      short events = fds[1 + i * 2 + side].revents;
	      if ((events & (POLLIN | POLLHUP)) && !c->eof[side] && !c->len[side])
		{
		  ssize_t n = read (c->fd[side], c->data[side], BUFSIZE);
		  if (n > 0)
		    {
		      c->pos[side] = 0;
		      c->len[side] = n;
		      c->last_io = time (NULL);
		    }
		  else if (n == 0)
		    c->eof[side] = 1;
		  else if (errno != EAGAIN && errno != EINTR)
		    {
		      perror (side ? "proxy backend read" : "proxy client read");
		      failed = 1;
		    }
		}
	      int source = 1 - side;
	      if ((events & POLLOUT) && c->len[source] && !c->connecting)
		{
		  ssize_t n = write (c->fd[side], c->data[source] + c->pos[source], c->len[source]);
		  if (n > 0)
		    {
		      c->pos[source] += n;
		      c->len[source] -= n;
		      c->last_io = time (NULL);
		    }
		  else if (n < 0 && errno != EAGAIN && errno != EINTR)
		    {
		      perror (side ? "proxy backend write" : "proxy client write");
		      failed = 1;
		    }
		}
	      if (events & (POLLERR | POLLNVAL))
		{
		  fprintf (stderr, "proxy %s poll error: 0x%x\n", side ? "backend" : "client",
			   events);
		  failed = 1;
		}
	    }
	  for (int side = 0; side < 2; side++)
	    if (!c->connecting && c->eof[side] && !c->len[side] && !c->shut[side])
	      {
		shutdown (c->fd[1 - side], SHUT_WR);
		c->shut[side] = 1;
	      }
	  if (failed || (c->eof[0] && c->eof[1] && !c->len[0] && !c->len[1]) ||
	      time (NULL) - c->last_io > 300)
	    close_connection (c);
	}
    }
}
