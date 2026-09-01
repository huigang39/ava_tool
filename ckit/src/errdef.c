#include "errdef.h"

struct merr_desc {
    const char *name;
    const char *msg;
};

static const struct merr_desc g_merr_desc[MECOUNT] = {
    [MOK] = {"MOK", "success"},
    [MEINVAL] = {"MEINVAL", "invalid argument"},
    [MENULLPTR] = {"MENULLPTR", "null pointer"},
    [MERANGE] = {"MERANGE", "value out of range"},
    [MEBUSY] = {"MEBUSY", "resource busy"},
    [MEACCES] = {"MEACCES", "access denied"},
    [MECREATE] = {"MECREATE", "resource creation failed"},
    [MEALLOC] = {"MEALLOC", "memory allocation failed"},
    [MEXIST] = {"MEXIST", "resource already exists"},
    [MENOTFOUND] = {"MENOTFOUND", "resource not found"},
    [MENOTSUP] = {"MENOTSUP", "operation not supported"},
    [MEOVERFLOW] = {"MEOVERFLOW", "capacity exceeded"},
    [MESTATE] = {"MESTATE", "invalid state"},
    [MEAGAIN] = {"MEAGAIN", "try again"},
    [MENOTINIT] = {"MENOTINIT", "not initialized"},
    [METIMEOUT] = {"METIMEOUT", "operation timed out"},
    [MESYSERR] = {"MESYSERR", "system call failed"},
    [MEIO] = {"MEIO", "input/output error"},
    [MECFG] = {"MECFG", "invalid peripheral configuration"},
    [MEDMACFG] = {"MEDMACFG", "invalid DMA configuration"},
    [MECHECKSUM] = {"MECHECKSUM", "data integrity check failed"},
    [MEVERIFY] = {"MEVERIFY", "data verification failed"},
    [MESENSOR] = {"MESENSOR", "sensor fault"},
};

const char *merr_name(const int result)
{
    const int code = merr_code(result);
    return code >= MOK && code < MECOUNT ? g_merr_desc[code].name : "MEUNKNOWN";
}

const char *merr_msg(const int result)
{
    const int code = merr_code(result);
    return code >= MOK && code < MECOUNT ? g_merr_desc[code].msg
                                         : "unknown error";
}
