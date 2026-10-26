/*
 * PROJECT:     LiberNT host-native tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     RtlRbInsertNodeEx and RtlRbRemoveNode invariants under random workloads
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include <stdio.h>
#include <stdlib.h>
#include "ntoskrnl.h"

typedef struct _ITEM
{
    RTL_BALANCED_NODE Node;
    unsigned Key;
    int Linked;
} ITEM;

static unsigned long Checks, Failures;

#define CHECK(c) do { Checks++; if (!(c)) { Failures++; fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static PRTL_BALANCED_NODE Parent(PRTL_BALANCED_NODE Node)
{
    return (PRTL_BALANCED_NODE)(Node->ParentValue & ~(ULONG_PTR)RTL_BALANCED_NODE_RESERVED_PARENT_MASK);
}

static unsigned Key(PRTL_BALANCED_NODE Node)
{
    return ((ITEM *)Node)->Key;
}

static int Validate(PRTL_BALANCED_NODE Node, PRTL_BALANCED_NODE Up, unsigned *Count, long Low, long High)
{
    int LeftHeight, RightHeight;

    if (Node == NULL)
        return 1;
    (*Count)++;
    CHECK(Parent(Node) == Up);
    CHECK((long)Key(Node) >= Low && (long)Key(Node) <= High);
    if (Node->Red)
    {
        CHECK(!(Node->Left && Node->Left->Red));
        CHECK(!(Node->Right && Node->Right->Red));
    }
    LeftHeight = Validate(Node->Left, Node, Count, Low, (long)Key(Node));
    RightHeight = Validate(Node->Right, Node, Count, (long)Key(Node), High);
    CHECK(LeftHeight == RightHeight);
    return LeftHeight + !Node->Red;
}

static void Insert(PRTL_RB_TREE Tree, ITEM *Item)
{
    PRTL_BALANCED_NODE Node = Tree->Root, Up = NULL;
    BOOLEAN Right = FALSE;

    while (Node != NULL)
    {
        Up = Node;
        Right = (Item->Key >= Key(Node));
        Node = Node->Children[Right];
    }
    RtlRbInsertNodeEx(Tree, Up, Right, &Item->Node);
    Item->Linked = 1;
}

static void Verify(PRTL_RB_TREE Tree, unsigned Expected)
{
    PRTL_BALANCED_NODE Min = Tree->Root;
    unsigned Count = 0;

    while ((Min != NULL) && (Min->Left != NULL))
        Min = Min->Left;
    CHECK(Tree->Min == Min);
    CHECK((Tree->Root == NULL) || !Tree->Root->Red);
    CHECK((Tree->Root == NULL) || (Parent(Tree->Root) == NULL));
    Validate(Tree->Root, NULL, &Count, -1, 0x7fffffffL);
    CHECK(Count == Expected);
}

int main(void)
{
    enum { Size = 2000, Rounds = 60000 };
    ITEM *Items = calloc(Size, sizeof(*Items));
    RTL_RB_TREE Tree = { 0 };
    unsigned Linked = 0, Round, Index;

    srand(1);
    for (Index = 0; Index < Size; Index++)
        Items[Index].Key = (unsigned)rand() % 500;

    for (Round = 0; Round < Rounds; Round++)
    {
        ITEM *Item = &Items[(unsigned)rand() % Size];

        if (Item->Linked)
        {
            RtlRbRemoveNode(&Tree, &Item->Node);
            Item->Linked = 0;
            Linked--;
        }
        else
        {
            Insert(&Tree, Item);
            Linked++;
        }
        if ((Round % 97) == 0)
            Verify(&Tree, Linked);
    }
    for (Index = 0; Index < Size; Index++)
    {
        if (Items[Index].Linked)
        {
            RtlRbRemoveNode(&Tree, &Items[Index].Node);
            Linked--;
        }
        if ((Index % 13) == 0)
            Verify(&Tree, Linked);
    }
    Verify(&Tree, 0);
    CHECK(Tree.Root == NULL && Tree.Min == NULL);

    for (Index = 0; Index < Size; Index++)
    {
        Items[Index].Key = Index;
        Insert(&Tree, &Items[Index]);
    }
    Verify(&Tree, Size);
    for (Index = 0; Index < Size; Index++)
    {
        RtlRbRemoveNode(&Tree, &Items[Index].Node);
        CHECK(Tree.Min == ((Index + 1 < Size) ? &Items[Index + 1].Node : NULL));
    }
    Verify(&Tree, 0);

    free(Items);
    printf("rtl_rbtree: %lu checks, %lu failures\n", Checks, Failures);
    return Failures != 0;
}
