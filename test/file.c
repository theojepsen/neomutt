#define TEST_NO_MAIN
#include "acutest.h"

#include "mutt/file.h"

void test_file_tidy_path(void)
{
  size_t len = 0;
  //
  ///
  //// No Symlink Resolution
  //
  { /* empty */
    len = mutt_file_tidy_path("", false);
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
    len = mutt_file_tidy_path(basic, false);
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
    len = mutt_file_tidy_path(basic, false);
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
    len = mutt_file_tidy_path(bp, false);
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
    len = mutt_file_tidy_path(bp, false);
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
    len = mutt_file_tidy_path(bp, false);
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
    len = mutt_file_tidy_path(bp, false);
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
    len = mutt_file_tidy_path(bp, false);
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
    len = mutt_file_tidy_path(trial, false);
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


