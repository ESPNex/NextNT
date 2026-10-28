/*
 * PROJECT:     NextNT host-native tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     RTL balanced tree types shared by the host builds of ntoskrnl/rtl/rbtree.c
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#pragma once

#define RTL_BALANCED_NODE_RESERVED_PARENT_MASK 3

typedef struct _RTL_BALANCED_NODE
{
    union
    {
        struct _RTL_BALANCED_NODE *Children[2];
        struct
        {
            struct _RTL_BALANCED_NODE *Left;
            struct _RTL_BALANCED_NODE *Right;
        };
    };
    union
    {
        UCHAR Red : 1;
        UCHAR Balance : 2;
        ULONG_PTR ParentValue;
    };
} RTL_BALANCED_NODE, *PRTL_BALANCED_NODE;

typedef struct _RTL_RB_TREE
{
    PRTL_BALANCED_NODE Root;
    union
    {
        UCHAR Encoded : 1;
        PRTL_BALANCED_NODE Min;
    };
} RTL_RB_TREE, *PRTL_RB_TREE;

VOID NTAPI RtlRbInsertNodeEx(PRTL_RB_TREE Tree, PRTL_BALANCED_NODE Parent, BOOLEAN Right, PRTL_BALANCED_NODE Node);
VOID NTAPI RtlRbRemoveNode(PRTL_RB_TREE Tree, PRTL_BALANCED_NODE Node);
