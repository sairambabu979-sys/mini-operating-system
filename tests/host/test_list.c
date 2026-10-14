// SPDX-License-Identifier: MIT
#include "test.h"

#include <lib/list.h>

struct item {
    int value;
    struct list_head link;
};

TEST(list_push_remove_and_iterate)
{
    struct list_head head = LIST_HEAD_INIT(head);
    struct item a = {1, {0}}, b = {2, {0}}, c = {3, {0}};

    EXPECT_TRUE(list_empty(&head));
    list_push_back(&head, &a.link);
    list_push_back(&head, &b.link);
    list_push_front(&head, &c.link);

    int order[3], n = 0;

    list_for_each(pos, &head) order[n++] = list_entry(pos, struct item, link)->value;
    EXPECT_EQ(n, 3);
    EXPECT_EQ(order[0], 3);
    EXPECT_EQ(order[1], 1);
    EXPECT_EQ(order[2], 2);

    list_remove(&a.link);
    list_remove(&a.link); /* self-linked after removal: harmless */
    EXPECT_EQ(list_first_entry(&head, struct item, link)->value, 3);
    list_remove(&c.link);
    list_remove(&b.link);
    EXPECT_TRUE(list_empty(&head));
}
