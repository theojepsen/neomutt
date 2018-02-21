/**
 * @file
 * Low-level socket handling
 *
 * @authors
 * Copyright (C) 1998,2000 Michael R. Elkins <me@mutt.org>
 * Copyright (C) 1999-2006,2008 Brendan Cully <brendan@kublai.com>
 * Copyright (C) 1999-2000 Tommi Komulainen <Tommi.Komulainen@iki.fi>
 *
 * @copyright
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 2 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @page conn_socket Low-level socket handling
 *
 * Low-level socket handling
 *
 * | Function               | Description
 * | :--------------------- | :-----------------------------------
 * | mutt_socket_close()    | Close a socket
 * | mutt_socket_open()     | Simple wrapper
 * | mutt_socket_poll()     | Checks whether reads would block
 * | mutt_socket_readchar() | simple read buffering to speed things up
 * | mutt_socket_readln_d() | Read a line from a socket
 * | mutt_socket_write_d()  | Write data to a socket
 * | raw_socket_close()     | Close a socket
 * | raw_socket_open()      | Open a socket
 * | raw_socket_poll()      | Checks whether reads would block
 * | raw_socket_read()      | Read data from a socket
 * | raw_socket_write()     | Write data to a socket
 * | socket_new_conn()      | allocate and initialise a new connection
 */

#include "config.h"
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "mutt/mutt.h"
#include "account.h"
#include "conn_globals.h"
#include "connection.h"
#include "globals.h"
#include "options.h"
#include "protos.h"
#ifdef USE_SSL
#include "ssl.h"
#endif

/**
 * socket_preconnect - Execute a command before opening a socket
 * @retval 0  Success
 * @retval >0 An errno, e.g. EPERM
 */
static int socket_preconnect(void)
{
  int rc;
  int save_errno;

  if (mutt_str_strlen(Preconnect))
  {
    mutt_debug(2, "Executing preconnect: %s\n", Preconnect);
    rc = mutt_system(Preconnect);
    mutt_debug(2, "Preconnect result: %d\n", rc);
    if (rc != 0)
    {
      save_errno = errno;
      mutt_perror(_("Preconnect command failed."));

      return save_errno;
    }
  }

  return 0;
}

/**
 * socket_connect - set up to connect to a socket fd
 * @param fd File descriptor to connect with
 * @param sa Address info
 * @retval  0 Success
 * @retval >0 An errno, e.g. EPERM
 * @retval -1 Error
 */
static int socket_connect(int fd, struct sockaddr *sa)
{
  int sa_size;
  int save_errno;
  sigset_t set;

  if (sa->sa_family == AF_INET)
    sa_size = sizeof(struct sockaddr_in);
#ifdef HAVE_GETADDRINFO
  else if (sa->sa_family == AF_INET6)
    sa_size = sizeof(struct sockaddr_in6);
#endif
  else
  {
    mutt_debug(1, "Unknown address family!\n");
    return -1;
  }

  if (ConnectTimeout > 0)
    alarm(ConnectTimeout);

  mutt_sig_allow_interrupt(1);

  /* FreeBSD's connect() does not respect SA_RESTART, meaning
   * a SIGWINCH will cause the connect to fail. */
  sigemptyset(&set);
  sigaddset(&set, SIGWINCH);
  sigprocmask(SIG_BLOCK, &set, NULL);

  save_errno = 0;

  if (connect(fd, sa, sa_size) < 0)
  {
    save_errno = errno;
    mutt_debug(2, "Connection failed. errno: %d...\n", errno);
    SigInt = 0; /* reset in case we caught SIGINTR while in connect() */
  }

  if (ConnectTimeout > 0)
    alarm(0);
  mutt_sig_allow_interrupt(0);
  sigprocmask(SIG_UNBLOCK, &set, NULL);

  return save_errno;
}

/**
 * mutt_socket_open - Simple wrapper
 * @param conn Connection to a server
 * @retval  0 Success
 * @retval -1 Error
 */
int mutt_socket_open(struct Connection *conn)
{
  int rc;

  if (socket_preconnect())
    return -1;

  rc = conn->conn_open(conn);

  mutt_debug(2, "Connected to %s:%d on fd=%d\n", conn->account.host,
             conn->account.port, conn->fd);

  return rc;
}

/**
 * mutt_socket_close - Close a socket
 * @param conn Connection to a server
 * @retval  0 Success
 * @retval -1 Error
 */
int mutt_socket_close(struct Connection *conn)
{
  int rc = -1;

  if (conn->fd < 0)
    mutt_debug(1, "Attempt to close closed connection.\n");
  else
    rc = conn->conn_close(conn);

  conn->fd = -1;
  conn->ssf = 0;

  return rc;
}

/**
 * mutt_socket_write_d - Write data to a socket
 * @param conn Connection to a server
 * @param buf Buffer with data to write
 * @param len Length of data to write
 * @param dbg Debug level for logging
 * @retval >0 Number of bytes written
 * @retval -1 Error
 */
int mutt_socket_write_d(struct Connection *conn, const char *buf, int len, int dbg)
{
  int rc;
  int sent = 0;

  mutt_debug(dbg, "%d> %s", conn->fd, buf);

  if (conn->fd < 0)
  {
    mutt_debug(1, "attempt to write to closed connection\n");
    return -1;
  }

  if (len < 0)
    len = mutt_str_strlen(buf);

  while (sent < len)
  {
    rc = conn->conn_write(conn, buf + sent, len - sent);
    if (rc < 0)
    {
      mutt_debug(1, "error writing (%s), closing socket\n", strerror(errno));
      mutt_socket_close(conn);

      return -1;
    }

    if (rc < len - sent)
      mutt_debug(3, "short write (%d of %d bytes)\n", rc, len - sent);

    sent += rc;
  }

  return sent;
}

/**
 * mutt_socket_poll - Checks whether reads would block
 * @param conn Connection to a server
 * @param wait_secs How long to wait for a response
 * @retval >0 There is data to read
 * @retval  0 Read would block
 * @retval -1 Connection doesn't support polling
 */
int mutt_socket_poll(struct Connection *conn, time_t wait_secs)
{
  if (conn->bufpos < conn->available)
    return conn->available - conn->bufpos;

  if (conn->conn_poll)
    return conn->conn_poll(conn, wait_secs);

  return -1;
}

/**
 * mutt_socket_readchar - simple read buffering to speed things up
 * @param[in]  conn Connection to a server
 * @param[out] c    Character that was read
 * @retval  1 Success
 * @retval -1 Error
 */
int mutt_socket_readchar(struct Connection *conn, char *c)
{
  if (conn->bufpos >= conn->available)
  {
    if (conn->fd >= 0)
      conn->available = conn->conn_read(conn, conn->inbuf, sizeof(conn->inbuf));
    else
    {
      mutt_debug(1, "attempt to read from closed connection.\n");
      return -1;
    }
    conn->bufpos = 0;
    if (conn->available == 0)
    {
      mutt_error(_("Connection to %s closed"), conn->account.host);
    }
    if (conn->available <= 0)
    {
      mutt_socket_close(conn);
      return -1;
    }
  }
  *c = conn->inbuf[conn->bufpos];
  conn->bufpos++;
  return 1;
}

/**
 * mutt_socket_readln_d - Read a line from a socket
 * @param buf    Buffer to store the line
 * @param buflen Length of data to write
 * @param conn   Connection to a server
 * @param dbg    Debug level for logging
 * @retval >0 Success, number of bytes read
 * @retval -1 Error
 */
int mutt_socket_readln_d(char *buf, size_t buflen, struct Connection *conn, int dbg)
{
  char ch;
  int i;

  for (i = 0; i < buflen - 1; i++)
  {
    if (mutt_socket_readchar(conn, &ch) != 1)
    {
      buf[i] = '\0';
      return -1;
    }

    if (ch == '\n')
      break;
    buf[i] = ch;
  }

  /* strip \r from \r\n termination */
  if (i && buf[i - 1] == '\r')
    i--;
  buf[i] = '\0';

  mutt_debug(dbg, "%d< %s\n", conn->fd, buf);

  /* number of bytes read, not strlen */
  return i + 1;
}

/**
 * socket_new_conn - allocate and initialise a new connection
 * @retval ptr New Connection
 */
struct Connection *socket_new_conn(void)
{
  struct Connection *conn = NULL;

  conn = mutt_mem_calloc(1, sizeof(struct Connection));
  conn->fd = -1;

  return conn;
}

/**
 * raw_socket_close - Close a socket
 * @param conn Connection to a server
 * @retval  0 Success
 * @retval -1 Error, see errno
 */
int raw_socket_close(struct Connection *conn)
{
  return close(conn->fd);
}

/**
 * raw_socket_read - Read data from a socket
 * @param conn Connection to a server
 * @param buf Buffer to store the data
 * @param len Number of bytes to read
 * @retval >0 Success, number of bytes read
 * @retval -1 Error, see errno
 */
int raw_socket_read(struct Connection *conn, char *buf, size_t len)
{
  int rc;

  mutt_sig_allow_interrupt(1);
  rc = read(conn->fd, buf, len);
  if (rc == -1)
  {
    mutt_error(_("Error talking to %s (%s)"), conn->account.host, strerror(errno));
    SigInt = 0;
  }
  mutt_sig_allow_interrupt(0);

  if (SigInt)
  {
    mutt_error(_("Connection to %s has been aborted"), conn->account.host);
    SigInt = 0;
    rc = -1;
  }

  return rc;
}

/**
 * raw_socket_write - Write data to a socket
 * @param conn Connection to a server
 * @param buf Buffer to read into
 * @param count Number of bytes to read
 * @retval >0 Success, number of bytes written
 * @retval -1 Error, see errno
 */
int raw_socket_write(struct Connection *conn, const char *buf, size_t count)
{
  int rc;

  mutt_sig_allow_interrupt(1);
  rc = write(conn->fd, buf, count);
  if (rc == -1)
  {
    mutt_error(_("Error talking to %s (%s)"), conn->account.host, strerror(errno));
    SigInt = 0;
  }
  mutt_sig_allow_interrupt(0);

  if (SigInt)
  {
    mutt_error(_("Connection to %s has been aborted"), conn->account.host);
    SigInt = 0;
    rc = -1;
  }

  return rc;
}

/**
 * raw_socket_poll - Checks whether reads would block
 * @param conn Connection to a server
 * @param wait_secs How long to wait for a response
 * @retval >0 There is data to read
 * @retval  0 Read would block
 * @retval -1 Connection doesn't support polling
 */
int raw_socket_poll(struct Connection *conn, time_t wait_secs)
{
  fd_set rfds;
  unsigned long wait_millis, post_t_millis;
  struct timeval tv, pre_t, post_t;
  int rc;

  if (conn->fd < 0)
    return -1;

  wait_millis = wait_secs * 1000UL;

  while (true)
  {
    tv.tv_sec = wait_millis / 1000;
    tv.tv_usec = (wait_millis % 1000) * 1000;

    FD_ZERO(&rfds);
    FD_SET(conn->fd, &rfds);

    gettimeofday(&pre_t, NULL);
    rc = select(conn->fd + 1, &rfds, NULL, NULL, &tv);
    gettimeofday(&post_t, NULL);

    if (rc > 0 || (rc < 0 && errno != EINTR))
      return rc;

    if (SigInt)
      mutt_query_exit();

    wait_millis += (pre_t.tv_sec * 1000UL) + (pre_t.tv_usec / 1000);
    post_t_millis = (post_t.tv_sec * 1000UL) + (post_t.tv_usec / 1000);
    if (wait_millis <= post_t_millis)
      return 0;
    wait_millis -= post_t_millis;
  }
}

/**
 * raw_socket_open - Open a socket
 * @param conn Connection to a server
 * @retval  0 Success
 * @retval -1 Error
 */
int raw_socket_open(struct Connection *conn)
{
  int rc;
  int fd;

  char *host_idna = NULL;

#ifdef HAVE_GETADDRINFO
  /* --- IPv4/6 --- */

  /* "65536\0" */
  char port[6];
  struct addrinfo hints;
  struct addrinfo *res = NULL;
  struct addrinfo *cur = NULL;

  /* we accept v4 or v6 STREAM sockets */
  memset(&hints, 0, sizeof(hints));

  if (UseIpv6)
    hints.ai_family = AF_UNSPEC;
  else
    hints.ai_family = AF_INET;

  hints.ai_socktype = SOCK_STREAM;

  snprintf(port, sizeof(port), "%d", conn->account.port);

#ifdef HAVE_LIBIDN
  if (mutt_idna_to_ascii_lz(conn->account.host, &host_idna, 1) != 0)
  {
    mutt_error(_("Bad IDN \"%s\"."), conn->account.host);
    return -1;
  }
#else
  host_idna = conn->account.host;
#endif

  if (!OPT_NO_CURSES)
    mutt_message(_("Looking up %s..."), conn->account.host);

  rc = getaddrinfo(host_idna, port, &hints, &res);

#ifdef HAVE_LIBIDN
  FREE(&host_idna);
#endif

  if (rc)
  {
    mutt_error(_("Could not find the host \"%s\""), conn->account.host);
    return -1;
  }

  if (!OPT_NO_CURSES)
    mutt_message(_("Connecting to %s..."), conn->account.host);

  rc = -1;
  for (cur = res; cur != NULL; cur = cur->ai_next)
  {
    fd = socket(cur->ai_family, cur->ai_socktype, cur->ai_protocol);
    if (fd >= 0)
    {
      rc = socket_connect(fd, cur->ai_addr);
      if (rc == 0)
      {
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        conn->fd = fd;
        break;
      }
      else
        close(fd);
    }
  }

  freeaddrinfo(res);

#else
  /* --- IPv4 only --- */

  struct sockaddr_in sin;
  struct hostent *he = NULL;

  memset(&sin, 0, sizeof(sin));
  sin.sin_port = htons(conn->account.port);
  sin.sin_family = AF_INET;

#ifdef HAVE_LIBIDN
  if (mutt_idna_to_ascii_lz(conn->account.host, &host_idna, 1) != 0)
  {
    mutt_error(_("Bad IDN \"%s\"."), conn->account.host);
    return -1;
  }
#else
  host_idna = conn->account.host;
#endif

  if (!OPT_NO_CURSES)
    mutt_message(_("Looking up %s..."), conn->account.host);

  he = gethostbyname(host_idna);

#ifdef HAVE_LIBIDN
  FREE(&host_idna);
#endif

  if (!he)
  {
    mutt_error(_("Could not find the host \"%s\""), conn->account.host);

    return -1;
  }

  if (!OPT_NO_CURSES)
    mutt_message(_("Connecting to %s..."), conn->account.host);

  rc = -1;
  for (int i = 0; he->h_addr_list[i] != NULL; i++)
  {
    memcpy(&sin.sin_addr, he->h_addr_list[i], he->h_length);
    fd = socket(PF_INET, SOCK_STREAM, IPPROTO_IP);

    if (fd >= 0)
    {
      rc = socket_connect(fd, (struct sockaddr *) &sin);
      if (rc == 0)
      {
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        conn->fd = fd;
        break;
      }
      else
        close(fd);
    }
  }

#endif
  if (rc)
  {
    mutt_error(_("Could not connect to %s (%s)."), conn->account.host,
               (rc > 0) ? strerror(rc) : _("unknown error"));
    return -1;
  }

  return 0;
}
