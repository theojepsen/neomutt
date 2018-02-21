/**
 * @file
 * DNS lookups
 *
 * @authors
 * Copyright (C) 2009,2013,2016 Derek Martin <code@pizzashack.org>
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
 * @page conn_getdomain DNS lookups
 *
 * DNS lookups
 *
 * | Function           | Description
 * | :----------------- | :-----------------------------------
 * | getdnsdomainname() | Lookup the host's name using DNS
 */

#include "config.h"
#include <netdb.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "mutt/logging.h"
#include "mutt/memory.h"
#include "mutt/string2.h"

/**
 * getdnsdomainname - Lookup the host's name using DNS
 * @param d   Buffer for the result
 * @param len Length of the buffer
 * @retval  0 Success
 * @retval -1 Error
 */
int getdnsdomainname(char *d, size_t len)
{
  int rc = -1;

#if defined(HAVE_GETADDRINFO) || defined(HAVE_GETADDRINFO_A)
  char node[STRING];
  if (gethostname(node, sizeof(node)))
    return rc;

  struct addrinfo hints;
  struct addrinfo *h = NULL;

  *d = '\0';
  memset(&hints, 0, sizeof(struct addrinfo));
  hints.ai_flags = AI_CANONNAME;
  hints.ai_family = AF_UNSPEC;

#ifdef HAVE_GETADDRINFO_A

  /* Allow 0.1 seconds to get the FQDN (fully-qualified domain name).
   * If it takes longer, the system is mis-configured and the network is not
   * working properly, so...
   */
  int status;
  struct timespec timeout = { 0, 100000000 };
  struct gaicb *reqs[1];
  reqs[0] = mutt_mem_calloc(1, sizeof(*reqs[0]));
  reqs[0]->ar_name = node;
  reqs[0]->ar_request = &hints;
  if (getaddrinfo_a(GAI_NOWAIT, reqs, 1, NULL) == 0)
  {
    gai_suspend((const struct gaicb *const *) reqs, 1, &timeout);
    status = gai_error(reqs[0]);
    if (status == 0)
      h = reqs[0]->ar_result;
    else if (status == EAI_INPROGRESS)
    {
      mutt_debug(1, "timeout\n");
      /* request is not finish, cancel it to free it safely */
      if (gai_cancel(reqs[0]) == EAI_NOTCANCELED)
      {
        while (gai_suspend((const struct gaicb *const *) reqs, 1, NULL) != 0)
          continue;
      }
    }
    else
      mutt_debug(1, "fail: (%d) %s\n", status, gai_strerror(status));
  }
  FREE(&reqs[0]);

#else /* !HAVE_GETADDRINFO_A */

  mutt_debug(3, "before getaddrinfo\n");
  getaddrinfo(node, NULL, &hints, &h);
  mutt_debug(3, "after getaddrinfo\n");

#endif

  char *p = NULL;
  if (h != NULL && h->ai_canonname && (p = strchr(h->ai_canonname, '.')))
  {
    mutt_str_strfcpy(d, ++p, len);
    rc = 0;
    mutt_debug(1, "Hostname: %s\n", d);
    freeaddrinfo(h);
  }

#endif /* HAVE_GETADDRINFO || defined HAVE_GETADDRINFO_A */

  return rc;
}
