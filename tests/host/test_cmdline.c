// SPDX-License-Identifier: MIT
#include "test.h"

#include <lib/cmdline.h>

#include <string.h>

TEST(cmdline_flags_match_whole_tokens)
{
    const char *cl = "  ktest loglevel=debug  quiet ";

    EXPECT_TRUE(cmdline_has_flag(cl, "ktest"));
    EXPECT_TRUE(cmdline_has_flag(cl, "quiet"));
    EXPECT_TRUE(!cmdline_has_flag(cl, "kt"));       /* prefix is not a match */
    EXPECT_TRUE(!cmdline_has_flag(cl, "loglevel")); /* key of key=value is not a flag */
    EXPECT_TRUE(!cmdline_has_flag("", "ktest"));
}

TEST(cmdline_get_values)
{
    const char *cl = "a=1 loglevel=debug empty= long=0123456789";
    char buf[8];

    EXPECT_TRUE(cmdline_get(cl, "loglevel", buf, sizeof(buf)));
    EXPECT_STREQ(buf, "debug");
    EXPECT_TRUE(cmdline_get(cl, "empty", buf, sizeof(buf)));
    EXPECT_STREQ(buf, "");
    EXPECT_TRUE(cmdline_get(cl, "long", buf, sizeof(buf)));
    EXPECT_STREQ(buf, "0123456"); /* truncated, still terminated */
    EXPECT_TRUE(!cmdline_get(cl, "log", buf, sizeof(buf)));
    EXPECT_TRUE(!cmdline_get(cl, "missing", buf, sizeof(buf)));
}
