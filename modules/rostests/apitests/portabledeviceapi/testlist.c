#define STANDALONE
#include <apitest.h>

extern void func_Bulk(void);
extern void func_Device(void);
extern void func_PnP(void);
extern void func_Probe(void);
extern void func_Serializer(void);
extern void func_Shell(void);
extern void func_Transfer(void);

const struct test winetest_testlist[] =
{
    { "Bulk", func_Bulk },
    { "Device", func_Device },
    { "PnP", func_PnP },
    { "Probe", func_Probe },
    { "Serializer", func_Serializer },
    { "Shell", func_Shell },
    { "Transfer", func_Transfer },

    { 0, 0 }
};
