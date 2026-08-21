#ifndef ERRDEF_H
#define ERRDEF_H

#ifdef __cplusplus
extern "C" {
#endif

enum errdef {
    MOK,
    MEINVAL,
    MEBUSY,
    MEACCES,
    METIMEOUT,
    MESYSERR,
    MECREATE,
    MEALLOC,
    MEXIST,
    MENULLPTR,
};

#ifdef __cplusplus
}
#endif

#endif // !ERRDEF_H
