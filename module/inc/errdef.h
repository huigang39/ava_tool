#ifndef ERRDEF_H
#define ERRDEF_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum errdef {
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
} errdef_e;

#ifdef __cplusplus
}
#endif

#endif // !ERRDEF_H
