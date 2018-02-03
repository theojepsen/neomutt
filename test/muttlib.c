#define TEST_NO_MAIN
#include "acutest.h"

/// Copied from main.c ////
#define MAIN_C 1
#include "config.h"
#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "mutt/mutt.h"
#include "conn/conn.h"
#include "mutt.h"
#include "address.h"
#include "alias.h"
#include "body.h"
#include "buffy.h"
#include "envelope.h"
#include "globals.h"
#include "header.h"
#include "keymap.h"
#include "mailbox.h"
#include "mutt_curses.h"
#include "mutt_menu.h"
#include "ncrypt/ncrypt.h"
#include "options.h"
#include "protos.h"
#include "url.h"
#include "version.h"
#ifdef ENABLE_NLS
#include <libintl.h>
#endif
#ifdef USE_SIDEBAR
#include "sidebar.h"
#endif
#ifdef USE_IMAP
#include "imap/imap.h"
#endif
#ifdef USE_NNTP
#include "nntp.h"
#endif

char **envlist = NULL;

// END SNIPPET from main.c //
#include "config.h"
#include "protos.h"

void test_mutt_realpath(void)
{
  size_t len = 0;
  //
  ///
  //// No Symlink Resolution
  //
  { /* empty */
    len = mutt_realpath("", false);
    if (!TEST_CHECK(len == 0))
    {
      TEST_MSG("Expected: %zu", 0);
      TEST_MSG("Actual  : %zu", len);
    }
  }

  ///
  ////
  ///// Absolute Paths
  //
  { // Basic
    char basic[] = "/a/b/c";
    len = mutt_realpath(basic, false);
    if (!TEST_CHECK(len == 6))
    {
      TEST_MSG("Expected: %zu", 6);
      TEST_MSG("Actual  : %zu", len);
    }
    if (!TEST_CHECK(strcmp(basic, "/a/b/c") == 0))
    {
      TEST_MSG("Expected: %s", "/a/b/c");
      TEST_MSG("Actual  : %s", basic);
    }
  }

  { // Basic trailing slash
    char basic[] = "/a/b/c/";
    len = mutt_realpath(basic, false);
    if (!TEST_CHECK(len == 6))
    {
      TEST_MSG("Expected: %zu", 6);
      TEST_MSG("Actual  : %zu", len);
    }
    if (!TEST_CHECK(strcmp(basic, "/a/b/c") == 0))
    {
      TEST_MSG("Expected: %s", "/a/b/c");
      TEST_MSG("Actual  : %s", basic);
    }
  }

  { // Basic Trailing Parent
    char bp[] = "/a/b/c/..";
    len = mutt_realpath(bp, false);
    if (!TEST_CHECK(len == 4))
    {
      TEST_MSG("Expected: %zu", 4);
      TEST_MSG("Actual  : %zu", len);
    }
    if (!TEST_CHECK(strcmp(bp, "/a/b") == 0))
    {
      TEST_MSG("Expected: %s", "/a/b");
      TEST_MSG("Actual  : %s", bp);
    }
  }

  { // Double trailing parent
    char bp[] = "/a/b/c/../..";
    len = mutt_realpath(bp, false);
    if (!TEST_CHECK(len == 2))
    {
      TEST_MSG("Expected: %zu", 2);
      TEST_MSG("Actual  : %zu", len);
    }
    if (!TEST_CHECK(strcmp(bp, "/a") == 0))
    {
      TEST_MSG("Expected: %s", "/a");
      TEST_MSG("Actual  : %s", bp);
    }
  }

  { // Double trailing parent trailing slash
    char bp[] = "/a/b/c/../../";
    len = mutt_realpath(bp, false);
    if (!TEST_CHECK(len == 2))
    {
      TEST_MSG("Expected: %zu", 2);
      TEST_MSG("Actual  : %zu", len);
    }
    if (!TEST_CHECK(strcmp(bp, "/a") == 0))
    {
      TEST_MSG("Expected: %s", "/a");
      TEST_MSG("Actual  : %s", bp);
    }
  }

  { // Too many parents
    char bp[] = "/a/../../..";
    len = mutt_realpath(bp, false);
    if (!TEST_CHECK(len == 1))
    {
      TEST_MSG("Expected: %zu", 1);
      TEST_MSG("Actual  : %zu", len);
    }
    if (!TEST_CHECK(strcmp(bp, "/") == 0))
    {
      TEST_MSG("Expected: %s", "/");
      TEST_MSG("Actual  : %s", bp);
    }
  }

  { // Too many parents
    char bp[] = "/..";
    len = mutt_realpath(bp, false);
    if (!TEST_CHECK(len == 1))
    {
      TEST_MSG("Expected: %zu", 1);
      TEST_MSG("Actual  : %zu", len);
    }
    if (!TEST_CHECK(strcmp(bp, "/") == 0))
    {
      TEST_MSG("Expected: %s", "/");
      TEST_MSG("Actual  : %s", bp);
    }
  }

  { /* nuts */
    char trial[] = "/apple/butterfly/../custard/../../dirty";
    const char expected[] = "/dirty";
    len = mutt_realpath(trial, false);
    if (!TEST_CHECK(len == strlen(expected)))
    {
      TEST_MSG("Expected: %zu", strlen(expected));
      TEST_MSG("Actual  : %zu", len);
    }
    if (!TEST_CHECK(strcmp(expected, trial) == 0))
    {
      TEST_MSG("Expected: %s", expected);
      TEST_MSG("Actual  : %s", trial);
    }
  }

  { /* too long */

  }
}


