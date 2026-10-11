#include <assert.h>
#include <stdio.h>

typedef void *mfxHDL;
enum { MFX_HANDLE_CONFIG_INTERFACE = 1 };
static int calls, errors, status;
static int interface_value;

static int MFXVideoCORE_GetHandle(void *session, int type, mfxHDL *handle)
{
    assert(session && type == MFX_HANDLE_CONFIG_INTERFACE);
    calls++;
    if (status >= 0)
        *handle = &interface_value;
    return status;
}

static int ff_qsv_print_error(void *avctx, int error, const char *message)
{
    assert(avctx && error < 0 && message);
    errors++;
    return error;
}

static int acquire(int has_params)
{
    struct { void *session; void *qsv_params; } context = {
        &interface_value, has_params ? &interface_value : NULL
    }, *q = &context;
    void *avctx = &context;
    void *iface = NULL;
    int ret;
#include "qsv-config-interface-under-test.h"
    assert(!has_params || iface == &interface_value);
    return 0;
}

int main(void)
{
    status = -9;
    assert(acquire(0) == 0 && calls == 0 && errors == 0);
    assert(acquire(1) == -9 && calls == 1 && errors == 1);
    status = -3;
    assert(acquire(1) == -3 && calls == 2 && errors == 2);
    status = 0;
    assert(acquire(1) == 0 && calls == 3 && errors == 2);
    assert(acquire(0) == 0 && calls == 3 && errors == 2);
    puts("QSV optional config interface: 5 cases passed");
    return 0;
}
