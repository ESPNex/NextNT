#define STANDALONE
#include <apitest.h>

extern void func_PressAndHold(void);

const struct test winetest_testlist[] =
{
    { "PressAndHold", func_PressAndHold },
    { 0, 0 }
};
