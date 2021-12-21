
#ifndef _FIFO_H_
#define _FIFO_H_

struct fifo_param_t {
    unsigned int size; /* fifo buffer size, unit: B */
};

enum fifo_errno {
    /* SUCC: 2xx */
    FIFO_SUCC = 200,
    
    /* TRY AGAIN: 3xx */
    FIFO_TIMEOUT = 300,
    FIFO_NO_RESOURCES = 301,
    FIFO_USER_CREATE_FAIL = 302,
    FIFO_NO_SUCH_USER = 303,
    FIFO_PTHREAD_COND_WAIT_FAIL = 304,
    FIFO_PTHREAD_COND_TIMEDWAIT_FAIL = 305,
    FIFO_HEAD_MAGIC_MISMATCH = 306,
    FIFO_HEAD_CHKSUM_MISMATCH = 307,
    FIFO_USER_BUFF_NOT_ENOUGH = 308,

    /* ABORT: 4xx */
    FIFO_INVALID_INPUT = 400,
    FIFO_SIZE_TOO_SMALL = 401,
    FIFO_OUT_OF_MEMORY = 402,
    FIFO_INVALID_FIFO = 403,
    FIFO_BAD_FIFO = 404,
};

#define INVALID_FIFO_USER_HANDLE NULL

typedef void * FIFO_USER_HANDLE;

#ifdef __cplusplus
extern "C" {
#endif

int fifo_create(const struct fifo_param_t *param, enum fifo_errno *err);

int fifo_write(int id, const void *buff, unsigned int len, int wait_ms, enum fifo_errno *err);

FIFO_USER_HANDLE fifo_user_add(int id, enum fifo_errno *err);

int fifo_user_read(int id, FIFO_USER_HANDLE h, void *buff, unsigned int size, int wait_ms, enum fifo_errno *err);

int fifo_user_remove(int id, FIFO_USER_HANDLE h, enum fifo_errno *err);

int fifo_destroy(int id, enum fifo_errno *err);

const char * fifo_strerr(enum fifo_errno err);

#ifdef __cplusplus
}
#endif

#endif

