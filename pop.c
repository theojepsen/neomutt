/**
 * @file
 * POP network mailbox
 *
 * @authors
 * Copyright (C) 2000-2002 Vsevolod Volkov <vvv@mutt.org.ua>
 * Copyright (C) 2006-2007,2009 Rocco Rutte <pdmef@gmx.net>
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

#include "config.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "mutt/mutt.h"
#include "conn/conn.h"
#include "mutt.h"
#include "pop.h"
#include "bcache.h"
#include "body.h"
#include "context.h"
#include "envelope.h"
#include "globals.h"
#include "header.h"
#include "mailbox.h"
#include "mutt_account.h"
#include "mutt_curses.h"
#include "mutt_socket.h"
#include "mx.h"
#include "ncrypt/ncrypt.h"
#include "options.h"
#include "protos.h"
#include "url.h"
#ifdef USE_HCACHE
#include "hcache/hcache.h"
#endif

#ifdef USE_HCACHE
#define HC_FNAME "neomutt" /* filename for hcache as POP lacks paths */
#define HC_FEXT "hcache"   /* extension for hcache as POP lacks paths */
#endif

/**
 * fetch_message - write line to file
 */
static int fetch_message(char *line, void *file)
{
  FILE *f = (FILE *) file;

  fputs(line, f);
  if (fputc('\n', f) == EOF)
    return -1;

  return 0;
}

/**
 * pop_read_header - Read header
 * @param pop_data POP data
 * @param h        Email header
 * @retval  0 Success
 * @retval -1 Connection lost
 * @retval -2 Invalid command or execution error
 * @retval -3 Error writing to tempfile
 */
static int pop_read_header(struct PopData *pop_data, struct Header *h)
{
  FILE *f = NULL;
  int rc, index;
  size_t length;
  char buf[LONG_STRING];
  char tempfile[_POSIX_PATH_MAX];

  mutt_mktemp(tempfile, sizeof(tempfile));
  f = mutt_file_fopen(tempfile, "w+");
  if (!f)
  {
    mutt_perror(tempfile);
    return -3;
  }

  snprintf(buf, sizeof(buf), "LIST %d\r\n", h->refno);
  rc = pop_query(pop_data, buf, sizeof(buf));
  if (rc == 0)
  {
    sscanf(buf, "+OK %d %zu", &index, &length);

    snprintf(buf, sizeof(buf), "TOP %d 0\r\n", h->refno);
    rc = pop_fetch_data(pop_data, buf, NULL, fetch_message, f);

    if (pop_data->cmd_top == 2)
    {
      if (rc == 0)
      {
        pop_data->cmd_top = 1;

        mutt_debug(1, "set TOP capability\n");
      }

      if (rc == -2)
      {
        pop_data->cmd_top = 0;

        mutt_debug(1, "unset TOP capability\n");
        snprintf(pop_data->err_msg, sizeof(pop_data->err_msg), "%s",
                 _("Command TOP is not supported by server."));
      }
    }
  }

  switch (rc)
  {
    case 0:
    {
      rewind(f);
      h->env = mutt_read_rfc822_header(f, h, 0, 0);
      h->content->length = length - h->content->offset + 1;
      rewind(f);
      while (!feof(f))
      {
        h->content->length--;
        fgets(buf, sizeof(buf), f);
      }
      break;
    }
    case -2:
    {
      mutt_error("%s", pop_data->err_msg);
      break;
    }
    case -3:
    {
      mutt_error(_("Can't write header to temporary file!"));
      break;
    }
  }

  mutt_file_fclose(&f);
  unlink(tempfile);
  return rc;
}

/**
 * fetch_uidl - parse UIDL
 */
static int fetch_uidl(char *line, void *data)
{
  int i, index;
  struct Context *ctx = (struct Context *) data;
  struct PopData *pop_data = (struct PopData *) ctx->data;
  char *endp = NULL;

  errno = 0;
  index = strtol(line, &endp, 10);
  if (errno)
    return -1;
  while (*endp == ' ')
    endp++;
  memmove(line, endp, strlen(endp) + 1);

  for (i = 0; i < ctx->msgcount; i++)
    if (mutt_str_strcmp(line, ctx->hdrs[i]->data) == 0)
      break;

  if (i == ctx->msgcount)
  {
    mutt_debug(1, "new header %d %s\n", index, line);

    if (i >= ctx->hdrmax)
      mx_alloc_memory(ctx);

    ctx->msgcount++;
    ctx->hdrs[i] = mutt_new_header();
    ctx->hdrs[i]->data = mutt_str_strdup(line);
  }
  else if (ctx->hdrs[i]->index != index - 1)
    pop_data->clear_cache = true;

  ctx->hdrs[i]->refno = index;
  ctx->hdrs[i]->index = index - 1;

  return 0;
}

static int msg_cache_check(const char *id, struct BodyCache *bcache, void *data)
{
  struct Context *ctx = NULL;
  struct PopData *pop_data = NULL;

  ctx = (struct Context *) data;
  if (!ctx)
    return -1;
  pop_data = (struct PopData *) ctx->data;
  if (!pop_data)
    return -1;

#ifdef USE_HCACHE
  /* keep hcache file if hcache == bcache */
  if (strcmp(HC_FNAME "." HC_FEXT, id) == 0)
    return 0;
#endif

  for (int i = 0; i < ctx->msgcount; i++)
  {
    /* if the id we get is known for a header: done (i.e. keep in cache) */
    if (ctx->hdrs[i]->data && (mutt_str_strcmp(ctx->hdrs[i]->data, id) == 0))
      return 0;
  }

  /* message not found in context -> remove it from cache
   * return the result of bcache, so we stop upon its first error
   */
  return mutt_bcache_del(bcache, id);
}

#ifdef USE_HCACHE
static int pop_hcache_namer(const char *path, char *dest, size_t destlen)
{
  return snprintf(dest, destlen, "%s." HC_FEXT, path);
}

static header_cache_t *pop_hcache_open(struct PopData *pop_data, const char *path)
{
  struct Url url;
  char p[LONG_STRING];

  if (!pop_data || !pop_data->conn)
    return mutt_hcache_open(HeaderCache, path, NULL);

  mutt_account_tourl(&pop_data->conn->account, &url);
  url.path = HC_FNAME;
  url_tostring(&url, p, sizeof(p), U_PATH);
  return mutt_hcache_open(HeaderCache, p, pop_hcache_namer);
}
#endif

/**
 * pop_fetch_headers - Read headers
 * @param ctx Context
 * @retval  0 Success
 * @retval -1 Connection lost
 * @retval -2 Invalid command or execution error
 * @retval -3 Error writing to tempfile
 */
static int pop_fetch_headers(struct Context *ctx)
{
  int i, ret, old_count, new_count, deleted;
  bool hcached = false, bcached;
  struct PopData *pop_data = (struct PopData *) ctx->data;
  struct Progress progress;

#ifdef USE_HCACHE
  header_cache_t *hc = NULL;
  void *data = NULL;

  hc = pop_hcache_open(pop_data, ctx->path);
#endif

  time(&pop_data->check_time);
  pop_data->clear_cache = false;

  for (i = 0; i < ctx->msgcount; i++)
    ctx->hdrs[i]->refno = -1;

  old_count = ctx->msgcount;
  ret = pop_fetch_data(pop_data, "UIDL\r\n", NULL, fetch_uidl, ctx);
  new_count = ctx->msgcount;
  ctx->msgcount = old_count;

  if (pop_data->cmd_uidl == 2)
  {
    if (ret == 0)
    {
      pop_data->cmd_uidl = 1;

      mutt_debug(1, "set UIDL capability\n");
    }

    if (ret == -2 && pop_data->cmd_uidl == 2)
    {
      pop_data->cmd_uidl = 0;

      mutt_debug(1, "unset UIDL capability\n");
      snprintf(pop_data->err_msg, sizeof(pop_data->err_msg), "%s",
               _("Command UIDL is not supported by server."));
    }
  }

  if (!ctx->quiet)
    mutt_progress_init(&progress, _("Fetching message headers..."),
                       MUTT_PROGRESS_MSG, ReadInc, new_count - old_count);

  if (ret == 0)
  {
    for (i = 0, deleted = 0; i < old_count; i++)
    {
      if (ctx->hdrs[i]->refno == -1)
      {
        ctx->hdrs[i]->deleted = true;
        deleted++;
      }
    }
    if (deleted > 0)
    {
      mutt_error(_("%d messages have been lost. Try reopening the mailbox."), deleted);
    }

    for (i = old_count; i < new_count; i++)
    {
      if (!ctx->quiet)
        mutt_progress_update(&progress, i + 1 - old_count, -1);
#ifdef USE_HCACHE
      data = mutt_hcache_fetch(hc, ctx->hdrs[i]->data, strlen(ctx->hdrs[i]->data));
      if (data)
      {
        char *uidl = mutt_str_strdup(ctx->hdrs[i]->data);
        int refno = ctx->hdrs[i]->refno;
        int index = ctx->hdrs[i]->index;
        /*
         * - POP dynamically numbers headers and relies on h->refno
         *   to map messages; so restore header and overwrite restored
         *   refno with current refno, same for index
         * - h->data needs to a separate pointer as it's driver-specific
         *   data freed separately elsewhere
         *   (the old h->data should point inside a malloc'd block from
         *   hcache so there shouldn't be a memleak here)
         */
        struct Header *h = mutt_hcache_restore((unsigned char *) data);
        mutt_hcache_free(hc, &data);
        mutt_free_header(&ctx->hdrs[i]);
        ctx->hdrs[i] = h;
        ctx->hdrs[i]->refno = refno;
        ctx->hdrs[i]->index = index;
        ctx->hdrs[i]->data = uidl;
        ret = 0;
        hcached = true;
      }
      else
#endif
          if ((ret = pop_read_header(pop_data, ctx->hdrs[i])) < 0)
        break;
#ifdef USE_HCACHE
      else
      {
        mutt_hcache_store(hc, ctx->hdrs[i]->data, strlen(ctx->hdrs[i]->data),
                          ctx->hdrs[i], 0);
      }
#endif

      /*
       * faked support for flags works like this:
       * - if 'hcached' is true, we have the message in our hcache:
       *        - if we also have a body: read
       *        - if we don't have a body: old
       *          (if $mark_old is set which is maybe wrong as
       *          $mark_old should be considered for syncing the
       *          folder and not when opening it XXX)
       * - if 'hcached' is false, we don't have the message in our hcache:
       *        - if we also have a body: read
       *        - if we don't have a body: new
       */
      bcached = (mutt_bcache_exists(pop_data->bcache, ctx->hdrs[i]->data) == 0);
      ctx->hdrs[i]->old = false;
      ctx->hdrs[i]->read = false;
      if (hcached)
      {
        if (bcached)
          ctx->hdrs[i]->read = true;
        else if (MarkOld)
          ctx->hdrs[i]->old = true;
      }
      else
      {
        if (bcached)
          ctx->hdrs[i]->read = true;
      }

      ctx->msgcount++;
    }

    if (i > old_count)
      mx_update_context(ctx, i - old_count);
  }

#ifdef USE_HCACHE
  mutt_hcache_close(hc);
#endif

  if (ret < 0)
  {
    for (i = ctx->msgcount; i < new_count; i++)
      mutt_free_header(&ctx->hdrs[i]);
    return ret;
  }

  /* after putting the result into our structures,
   * clean up cache, i.e. wipe messages deleted outside
   * the availability of our cache
   */
  if (MessageCacheClean)
    mutt_bcache_list(pop_data->bcache, msg_cache_check, (void *) ctx);

  mutt_clear_error();
  return (new_count - old_count);
}

/**
 * pop_open_mailbox - open POP mailbox, fetch only headers
 */
static int pop_open_mailbox(struct Context *ctx)
{
  int ret;
  char buf[LONG_STRING];
  struct Connection *conn = NULL;
  struct Account acct;
  struct PopData *pop_data = NULL;
  struct Url url;

  if (pop_parse_path(ctx->path, &acct))
  {
    mutt_error(_("%s is an invalid POP path"), ctx->path);
    return -1;
  }

  mutt_account_tourl(&acct, &url);
  url.path = NULL;
  url_tostring(&url, buf, sizeof(buf), 0);
  conn = mutt_conn_find(NULL, &acct);
  if (!conn)
    return -1;

  FREE(&ctx->path);
  FREE(&ctx->realpath);
  ctx->path = mutt_str_strdup(buf);
  ctx->realpath = mutt_str_strdup(ctx->path);

  pop_data = mutt_mem_calloc(1, sizeof(struct PopData));
  pop_data->conn = conn;
  ctx->data = pop_data;

  if (pop_open_connection(pop_data) < 0)
    return -1;

  conn->data = pop_data;
  pop_data->bcache = mutt_bcache_open(&acct, NULL);

  /* init (hard-coded) ACL rights */
  memset(ctx->rights, 0, sizeof(ctx->rights));
  mutt_bit_set(ctx->rights, MUTT_ACL_SEEN);
  mutt_bit_set(ctx->rights, MUTT_ACL_DELETE);
#ifdef USE_HCACHE
  /* flags are managed using header cache, so it only makes sense to
   * enable them in that case */
  mutt_bit_set(ctx->rights, MUTT_ACL_WRITE);
#endif

  while (true)
  {
    if (pop_reconnect(ctx) < 0)
      return -1;

    ctx->size = pop_data->size;

    mutt_message(_("Fetching list of messages..."));

    ret = pop_fetch_headers(ctx);

    if (ret >= 0)
      return 0;

    if (ret < -1)
    {
      mutt_sleep(2);
      return -1;
    }
  }
}

/**
 * pop_clear_cache - delete all cached messages
 */
static void pop_clear_cache(struct PopData *pop_data)
{
  if (!pop_data->clear_cache)
    return;

  mutt_debug(1, "delete cached messages\n");

  for (int i = 0; i < POP_CACHE_LEN; i++)
  {
    if (pop_data->cache[i].path)
    {
      unlink(pop_data->cache[i].path);
      FREE(&pop_data->cache[i].path);
    }
  }
}

/**
 * pop_close_mailbox - close POP mailbox
 */
static int pop_close_mailbox(struct Context *ctx)
{
  struct PopData *pop_data = (struct PopData *) ctx->data;

  if (!pop_data)
    return 0;

  pop_logout(ctx);

  if (pop_data->status != POP_NONE)
    mutt_socket_close(pop_data->conn);

  pop_data->status = POP_NONE;

  pop_data->clear_cache = true;
  pop_clear_cache(pop_data);

  if (!pop_data->conn->data)
    mutt_socket_free(pop_data->conn);

  mutt_bcache_close(&pop_data->bcache);

  return 0;
}

/**
 * pop_fetch_message - fetch message from POP server
 */
static int pop_fetch_message(struct Context *ctx, struct Message *msg, int msgno)
{
  int ret;
  void *uidl = NULL;
  char buf[LONG_STRING];
  char path[_POSIX_PATH_MAX];
  struct Progress progressbar;
  struct PopData *pop_data = (struct PopData *) ctx->data;
  struct PopCache *cache = NULL;
  struct Header *h = ctx->hdrs[msgno];
  unsigned short bcache = 1;

  /* see if we already have the message in body cache */
  msg->fp = mutt_bcache_get(pop_data->bcache, h->data);
  if (msg->fp)
    return 0;

  /*
   * see if we already have the message in our cache in
   * case $message_cachedir is unset
   */
  cache = &pop_data->cache[h->index % POP_CACHE_LEN];

  if (cache->path)
  {
    if (cache->index == h->index)
    {
      /* yes, so just return a pointer to the message */
      msg->fp = fopen(cache->path, "r");
      if (msg->fp)
        return 0;

      mutt_perror(cache->path);
      return -1;
    }
    else
    {
      /* clear the previous entry */
      unlink(cache->path);
      FREE(&cache->path);
    }
  }

  while (true)
  {
    if (pop_reconnect(ctx) < 0)
      return -1;

    /* verify that massage index is correct */
    if (h->refno < 0)
    {
      mutt_error(
          _("The message index is incorrect. Try reopening the mailbox."));
      return -1;
    }

    mutt_progress_init(&progressbar, _("Fetching message..."), MUTT_PROGRESS_SIZE,
                       NetInc, h->content->length + h->content->offset - 1);

    /* see if we can put in body cache; use our cache as fallback */
    msg->fp = mutt_bcache_put(pop_data->bcache, h->data);
    if (!msg->fp)
    {
      /* no */
      bcache = 0;
      mutt_mktemp(path, sizeof(path));
      msg->fp = mutt_file_fopen(path, "w+");
      if (!msg->fp)
      {
        mutt_perror(path);
        return -1;
      }
    }

    snprintf(buf, sizeof(buf), "RETR %d\r\n", h->refno);

    ret = pop_fetch_data(pop_data, buf, &progressbar, fetch_message, msg->fp);
    if (ret == 0)
      break;

    mutt_file_fclose(&msg->fp);

    /* if RETR failed (e.g. connection closed), be sure to remove either
     * the file in bcache or from POP's own cache since the next iteration
     * of the loop will re-attempt to put() the message */
    if (!bcache)
      unlink(path);

    if (ret == -2)
    {
      mutt_error("%s", pop_data->err_msg);
      return -1;
    }

    if (ret == -3)
    {
      mutt_error(_("Can't write message to temporary file!"));
      return -1;
    }
  }

  /* Update the header information.  Previously, we only downloaded a
   * portion of the headers, those required for the main display.
   */
  if (bcache)
    mutt_bcache_commit(pop_data->bcache, h->data);
  else
  {
    cache->index = h->index;
    cache->path = mutt_str_strdup(path);
  }
  rewind(msg->fp);
  uidl = h->data;

  /* we replace envelop, key in subj_hash has to be updated as well */
  if (ctx->subj_hash && h->env->real_subj)
    mutt_hash_delete(ctx->subj_hash, h->env->real_subj, h);
  mutt_label_hash_remove(ctx, h);
  mutt_env_free(&h->env);
  h->env = mutt_read_rfc822_header(msg->fp, h, 0, 0);
  if (ctx->subj_hash && h->env->real_subj)
    mutt_hash_insert(ctx->subj_hash, h->env->real_subj, h);
  mutt_label_hash_add(ctx, h);

  h->data = uidl;
  h->lines = 0;
  fgets(buf, sizeof(buf), msg->fp);
  while (!feof(msg->fp))
  {
    ctx->hdrs[msgno]->lines++;
    fgets(buf, sizeof(buf), msg->fp);
  }

  h->content->length = ftello(msg->fp) - h->content->offset;

  /* This needs to be done in case this is a multipart message */
  if (!WithCrypto)
    h->security = crypt_query(h->content);

  mutt_clear_error();
  rewind(msg->fp);

  return 0;
}

static int pop_close_message(struct Context *ctx, struct Message *msg)
{
  return mutt_file_fclose(&msg->fp);
}

/**
 * pop_sync_mailbox - update POP mailbox, delete messages from server
 */
static int pop_sync_mailbox(struct Context *ctx, int *index_hint)
{
  int i, j, ret = 0;
  char buf[LONG_STRING];
  struct PopData *pop_data = (struct PopData *) ctx->data;
  struct Progress progress;
#ifdef USE_HCACHE
  header_cache_t *hc = NULL;
#endif

  pop_data->check_time = 0;

  while (true)
  {
    if (pop_reconnect(ctx) < 0)
      return -1;

    mutt_progress_init(&progress, _("Marking messages deleted..."),
                       MUTT_PROGRESS_MSG, WriteInc, ctx->deleted);

#ifdef USE_HCACHE
    hc = pop_hcache_open(pop_data, ctx->path);
#endif

    for (i = 0, j = 0, ret = 0; ret == 0 && i < ctx->msgcount; i++)
    {
      if (ctx->hdrs[i]->deleted && ctx->hdrs[i]->refno != -1)
      {
        j++;
        if (!ctx->quiet)
          mutt_progress_update(&progress, j, -1);
        snprintf(buf, sizeof(buf), "DELE %d\r\n", ctx->hdrs[i]->refno);
        ret = pop_query(pop_data, buf, sizeof(buf));
        if (ret == 0)
        {
          mutt_bcache_del(pop_data->bcache, ctx->hdrs[i]->data);
#ifdef USE_HCACHE
          mutt_hcache_delete(hc, ctx->hdrs[i]->data, strlen(ctx->hdrs[i]->data));
#endif
        }
      }

#ifdef USE_HCACHE
      if (ctx->hdrs[i]->changed)
      {
        mutt_hcache_store(hc, ctx->hdrs[i]->data, strlen(ctx->hdrs[i]->data),
                          ctx->hdrs[i], 0);
      }
#endif
    }

#ifdef USE_HCACHE
    mutt_hcache_close(hc);
#endif

    if (ret == 0)
    {
      mutt_str_strfcpy(buf, "QUIT\r\n", sizeof(buf));
      ret = pop_query(pop_data, buf, sizeof(buf));
    }

    if (ret == 0)
    {
      pop_data->clear_cache = true;
      pop_clear_cache(pop_data);
      pop_data->status = POP_DISCONNECTED;
      return 0;
    }

    if (ret == -2)
    {
      mutt_error("%s", pop_data->err_msg);
      return -1;
    }
  }
}

/**
 * pop_check_mailbox - Check for new messages and fetch headers
 */
static int pop_check_mailbox(struct Context *ctx, int *index_hint)
{
  int ret;
  struct PopData *pop_data = (struct PopData *) ctx->data;

  if ((pop_data->check_time + PopCheckinterval) > time(NULL))
    return 0;

  pop_logout(ctx);

  mutt_socket_close(pop_data->conn);

  if (pop_open_connection(pop_data) < 0)
    return -1;

  ctx->size = pop_data->size;

  mutt_message(_("Checking for new messages..."));

  ret = pop_fetch_headers(ctx);
  pop_clear_cache(pop_data);

  if (ret < 0)
    return -1;

  if (ret > 0)
    return MUTT_NEW_MAIL;

  return 0;
}

/**
 * pop_fetch_mail - Fetch messages and save them in $spoolfile
 */
void pop_fetch_mail(void)
{
  char buffer[LONG_STRING];
  char msgbuf[SHORT_STRING];
  char *url = NULL, *p = NULL;
  int delanswer, last = 0, msgs, bytes, rset = 0, ret;
  struct Connection *conn = NULL;
  struct Context ctx;
  struct Message *msg = NULL;
  struct Account acct;
  struct PopData *pop_data = NULL;

  if (!PopHost)
  {
    mutt_error(_("POP host is not defined."));
    return;
  }

  url = p = mutt_mem_calloc(strlen(PopHost) + 7, sizeof(char));
  if (url_check_scheme(PopHost) == U_UNKNOWN)
  {
    strcpy(url, "pop://");
    p = strchr(url, '\0');
  }
  strcpy(p, PopHost);

  ret = pop_parse_path(url, &acct);
  FREE(&url);
  if (ret)
  {
    mutt_error(_("%s is an invalid POP path"), PopHost);
    return;
  }

  conn = mutt_conn_find(NULL, &acct);
  if (!conn)
    return;

  pop_data = mutt_mem_calloc(1, sizeof(struct PopData));
  pop_data->conn = conn;

  if (pop_open_connection(pop_data) < 0)
  {
    mutt_socket_free(pop_data->conn);
    FREE(&pop_data);
    return;
  }

  conn->data = pop_data;

  mutt_message(_("Checking for new messages..."));

  /* find out how many messages are in the mailbox. */
  mutt_str_strfcpy(buffer, "STAT\r\n", sizeof(buffer));
  ret = pop_query(pop_data, buffer, sizeof(buffer));
  if (ret == -1)
    goto fail;
  if (ret == -2)
  {
    mutt_error("%s", pop_data->err_msg);
    goto finish;
  }

  sscanf(buffer, "+OK %d %d", &msgs, &bytes);

  /* only get unread messages */
  if (msgs > 0 && PopLast)
  {
    mutt_str_strfcpy(buffer, "LAST\r\n", sizeof(buffer));
    ret = pop_query(pop_data, buffer, sizeof(buffer));
    if (ret == -1)
      goto fail;
    if (ret == 0)
      sscanf(buffer, "+OK %d", &last);
  }

  if (msgs <= last)
  {
    mutt_message(_("No new mail in POP mailbox."));
    goto finish;
  }

  if (mx_open_mailbox(NONULL(SpoolFile), MUTT_APPEND, &ctx) == NULL)
    goto finish;

  delanswer = query_quadoption(PopDelete, _("Delete messages from server?"));

  snprintf(msgbuf, sizeof(msgbuf), _("Reading new messages (%d bytes)..."), bytes);
  mutt_message("%s", msgbuf);

  for (int i = last + 1; i <= msgs; i++)
  {
    msg = mx_open_new_message(&ctx, NULL, MUTT_ADD_FROM);
    if (!msg)
      ret = -3;
    else
    {
      snprintf(buffer, sizeof(buffer), "RETR %d\r\n", i);
      ret = pop_fetch_data(pop_data, buffer, NULL, fetch_message, msg->fp);
      if (ret == -3)
        rset = 1;

      if (ret == 0 && mx_commit_message(msg, &ctx) != 0)
      {
        rset = 1;
        ret = -3;
      }

      mx_close_message(&ctx, &msg);
    }

    if (ret == 0 && delanswer == MUTT_YES)
    {
      /* delete the message on the server */
      snprintf(buffer, sizeof(buffer), "DELE %d\r\n", i);
      ret = pop_query(pop_data, buffer, sizeof(buffer));
    }

    if (ret == -1)
    {
      mx_close_mailbox(&ctx, NULL);
      goto fail;
    }
    if (ret == -2)
    {
      mutt_error("%s", pop_data->err_msg);
      break;
    }
    if (ret == -3)
    {
      mutt_error(_("Error while writing mailbox!"));
      break;
    }

    mutt_message(_("%s [%d of %d messages read]"), msgbuf, i - last, msgs - last);
  }

  mx_close_mailbox(&ctx, NULL);

  if (rset)
  {
    /* make sure no messages get deleted */
    mutt_str_strfcpy(buffer, "RSET\r\n", sizeof(buffer));
    if (pop_query(pop_data, buffer, sizeof(buffer)) == -1)
      goto fail;
  }

finish:
  /* exit gracefully */
  mutt_str_strfcpy(buffer, "QUIT\r\n", sizeof(buffer));
  if (pop_query(pop_data, buffer, sizeof(buffer)) == -1)
    goto fail;
  mutt_socket_close(conn);
  FREE(&pop_data);
  return;

fail:
  mutt_error(_("Server closed connection!"));
  mutt_socket_close(conn);
  FREE(&pop_data);
}

struct MxOps mx_pop_ops = {
  .open = pop_open_mailbox,
  .open_append = NULL,
  .close = pop_close_mailbox,
  .open_msg = pop_fetch_message,
  .close_msg = pop_close_message,
  .check = pop_check_mailbox,
  .commit_msg = NULL,
  .open_new_msg = NULL,
  .sync = pop_sync_mailbox,
  .edit_msg_tags = NULL,
  .commit_msg_tags = NULL,
};
