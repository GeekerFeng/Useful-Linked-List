
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <stddef.h>
#include <unistd.h>
#include "fifo.h"

#define FIFO_MAX_INSTANCES 32

//#define FIFO_DBG
#ifdef FIFO_DBG
#define DBG(fmt, args...) printf("D[%s:%d]: "fmt, __FILE__, __LINE__, ##args)
#else
#define DBG(fmt, args...)
#endif

#define FIFO_INFO
#ifdef FIFO_INFO
#define INFO(fmt, args...) printf("I[%s:%d]: "fmt, __FILE__, __LINE__, ##args)
#else
#define INFO(fmt, args...)
#endif

#define FIFO_WARN
#ifdef FIFO_WARN
#define WARN(fmt, args...) printf("W[%s:%d]: "fmt, __FILE__, __LINE__, ##args)
#else
#define WARN(fmt, args...)
#endif

#define FIFO_ERR
#ifdef FIFO_ERR
#define ERR(fmt, args...) printf("E[%s:%d]: "fmt, __FILE__, __LINE__, ##args)
#else
#define ERR(fmt, args...)
#endif

#ifndef min
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif

#ifndef SAFE_FREE
#define SAFE_FREE(ptr) do { \
	if (NULL != (ptr)) { \
		free((ptr)); \
		(ptr) = NULL; \
	} \
} while (0)
#endif

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof ((x)) / sizeof ((x)[0]))
#endif

#define FIFO_ERRNO_SET(err, e) do { \
    if (NULL != (err)) { \
        switch ((e) / 100) { \
            case 3: \
                if (FIFO_TIMEOUT == (e)) { \
                    DBG("SET fifo_errno => %d(%s)\n", (int)(e), fifo_strerr((e))); \
                } \
                else { \
                    WARN("SET fifo_errno => %d(%s)\n", (int)(e), fifo_strerr((e))); \
                } \
                break; \
            case 4: \
                ERR("SET fifo_errno => %d(%s)\n", (int)(e), fifo_strerr((e))); \
                break; \
            default: \
                break; \
        } \
        *(err) = (e); \
    } \
} while (0)

#define FIFO_HEAD_MAGIC (('F' << 24) | ('I' << 16) | ('F' << 8) | ('O'))

#define FIFO_IDX_NEXT_N(idx, size, n) (((idx) + (n)) % (size))

struct fifo_head_t {
    unsigned int magic; /* 固定为FIFO_HEAD_MAGIC */
    unsigned int len; /* 数据长度 */
    unsigned int chksum;
};

/* fifo最小数据大小 */
#define FIFO_MIN_DATA_SIZE (64 * 1024)

#define FIFO_INVALID_IDX 0xffffffff

/* fifo最小大小 */
#define FIFO_MIN_SIZE (sizeof (struct fifo_head_t) + FIFO_MIN_DATA_SIZE)

struct fifo_user_t {
    pthread_mutex_t mutex;
    pthread_cond_t cond;

    unsigned int r;

    struct fifo_user_t *next;
};

struct fifo_t {
    pthread_rwlock_t lock;
    int inuse;

    struct fifo_param_t param;

    void *buff;
    unsigned int r;
    unsigned int w;
    unsigned int over;

    pthread_mutex_t user_list_mutex;
    struct fifo_user_t *user_list;
};

struct fifos_t {
    pthread_mutex_t mutex;
    int init;

    struct fifo_t fifos[FIFO_MAX_INSTANCES];
};

#if (defined(_WIN32) || defined(_WIN32_WCE) || defined(WIN64))
static struct fifos_t sg_fifos = {
    PTHREAD_MUTEX_INITIALIZER,
    0,
};
#else
static struct fifos_t sg_fifos = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .init = 0,
};
#endif


#if (defined(_WIN32) || defined(_WIN32_WCE) || defined(WIN64))
static void gettimeofday_win(struct timeval *tval)
{
    time_t t = 0;
    struct tm tm;
    SYSTEMTIME wtm;

    GetLocalTime(&wtm);
    tm.tm_year = wtm.wYear - 1900;
    tm.tm_mon = wtm.wMonth - 1;
    tm.tm_mday = wtm.wDay;
    tm.tm_hour = wtm.wHour;
    tm.tm_min = wtm.wMinute;
    tm.tm_sec = wtm.wSecond;
    tm.tm_isdst = -1;

    t = mktime(&tm);
    tval->tv_sec = t;
    tval->tv_usec = wtm.wMilliseconds * 1000;
    
    return;
    
}
#endif

static int fifo_clock_gettime(struct timespec *tp)
{
    int ret = -1;

    if (NULL == tp) {
        return -1;
    }

#if (defined(_WIN32) || defined(_WIN32_WCE) || defined(WIN64))
    struct timeval tval;

    gettimeofday_win(&tval);
    tp->tv_sec = tval.tv_sec;
    tp->tv_nsec = tval.tv_usec * 1000;
#else
    ret = clock_gettime(CLOCK_REALTIME, tp);
#endif

    return ret;

}

static enum fifo_errno fifo_pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex, int wait_ms)
{
    int ret = -1;
    struct timespec timeout;
    enum fifo_errno e = FIFO_SUCC;

    if (NULL == cond || NULL == mutex) {
        return FIFO_INVALID_INPUT;
    }
    
    if (-1 == wait_ms) {
        if (-1 == pthread_cond_wait(cond, mutex)) {
            ERR("FAIL to do pthread_cond_wait, %s\n", strerror(errno));
            e = FIFO_PTHREAD_COND_WAIT_FAIL;
        }
        else {
            e = FIFO_SUCC;
        }
    }
    else {
        memset((char *)&timeout, 0, sizeof (timeout));
        fifo_clock_gettime(&timeout);
        timeout.tv_sec += wait_ms / 1000;
        timeout.tv_nsec += ((wait_ms) % 1000) * 1000 * 1000;
        if (-1 == pthread_cond_timedwait(cond, mutex, &timeout)) {
            if (ETIMEDOUT == errno) {
                e = FIFO_TIMEOUT;
            }
            else {
                ERR("FAIL to do pthread_cond_timedwait, %s\n", strerror(errno));
                e = FIFO_PTHREAD_COND_TIMEDWAIT_FAIL;
            }
        }
        else {
            e = FIFO_SUCC;
        }
    }
    
    return e;
    
}

static unsigned int fifo_chksum_calc(unsigned char *buff, unsigned int len)
{
    int i = 0;
    unsigned int chksum = 0;

    if (NULL == buff || 0 == len) {
        return 0;
    } 

    for (i = 0; i < len; i++) {
        chksum += buff[i];
    }

    return chksum;
    
}

static enum fifo_errno fifo_head_check(struct fifo_head_t *head)
{
    unsigned int chksum_calc = 0;

    if (NULL == head) {
        ERR("INVALID input param, head = %p\n", head);
        return FIFO_INVALID_INPUT;
    }

    if (FIFO_HEAD_MAGIC != head->magic) {
        WARN("FIFO head magic mismatch, curr: 0x %x, right: 0x %x\n", head->magic, FIFO_HEAD_MAGIC);
        return FIFO_HEAD_MAGIC_MISMATCH;
    }

    chksum_calc = fifo_chksum_calc((unsigned char *)head, offsetof(struct fifo_head_t, chksum));
    if (chksum_calc != head->chksum) {
        WARN("FIFO head chksum mismatch, curr: 0x %x, calc: 0x %x\n", head->chksum, chksum_calc);
        return FIFO_HEAD_CHKSUM_MISMATCH;
    }

    return FIFO_SUCC;
    
}

static enum fifo_errno fifo_check(const struct fifo_param_t *param)
{
    if (NULL == param) {
        ERR("INVALID input param, param = %p\n", param);
        return FIFO_INVALID_INPUT;
    }

    if (param->size < FIFO_MIN_SIZE) {
        ERR("fifo size(%u) too small, min size(%u)\n", param->size, (unsigned int)FIFO_MIN_SIZE);
        return FIFO_SIZE_TOO_SMALL;
    }

    INFO("NEW fifo, size = %u\n", param->size);
    
    return FIFO_SUCC;
    
}

static inline void fifos_init(struct fifos_t *fifos)
{
    int i = 0;
    struct fifo_t *p = NULL;

    for (i = 0; i < ARRAY_SIZE(fifos->fifos); i++) {
        p = &(fifos->fifos[i]);
        memset((char *)p, 0, sizeof (*p));
        pthread_rwlock_init(&p->lock, NULL);

        p->buff = NULL;
        p->r = 0;
        p->w = 0;
        p->over = FIFO_INVALID_IDX;

        p->user_list = NULL;
    }
    
    return;
    
}

static inline enum fifo_errno _fifo_init(const struct fifo_param_t *param, struct fifo_t *fifo)
{
    memcpy((char *)&fifo->param, (char *)param, sizeof (struct fifo_param_t));

    fifo->buff = malloc(fifo->param.size);
    if (NULL == fifo->buff) {
        ERR("OUT OF MEMORY, fifo->param.size = %u\n", fifo->param.size);
        return FIFO_OUT_OF_MEMORY;
    }
    memset((char *)fifo->buff, 0, fifo->param.size);

    fifo->r = 0;
    fifo->w = 0;
    fifo->over = FIFO_INVALID_IDX;

    pthread_mutex_init(&fifo->user_list_mutex, NULL);
    fifo->user_list = NULL;

    return FIFO_SUCC;
    
}

static enum fifo_errno fifo_init(const struct fifo_param_t *param, struct fifo_t **pp_fifo)
{
    struct fifos_t *p = &sg_fifos;
    struct fifo_t *fifo = NULL;
    int i = 0;
    enum fifo_errno e = FIFO_SUCC;

    if (NULL == param || NULL == pp_fifo) {
        ERR("INVALID input param, param = %p, pp_fifo = %p\n", param, pp_fifo);
        return FIFO_INVALID_INPUT;
    }

    pthread_mutex_lock(&p->mutex);
    {
        if (!p->init) {
            fifos_init(p);
            p->init = 1;
        }

        for (i = 0; i < ARRAY_SIZE(p->fifos); i++) {
            fifo = &(p->fifos[i]);
            if (!fifo->inuse) {
                e = _fifo_init(param, fifo);
                if (FIFO_SUCC == e) {
                    fifo->inuse = 1;
                    *pp_fifo = fifo;
                }
                pthread_mutex_unlock(&p->mutex);
                return e;
            }
        }
    }
    pthread_mutex_unlock(&p->mutex);

    return FIFO_NO_RESOURCES;
    
}

static inline struct fifo_user_t * fifo_user_create(void)
{
    struct fifo_user_t *user = NULL;

    user = (struct fifo_user_t *)malloc(sizeof (*user));
    if (NULL == user) {
        ERR("OUT OF MEMORY, sizeof (*user) = %d\n", (int)sizeof (*user));
        return NULL;
    }
    memset((char *)user, 0, sizeof (*user));

    pthread_mutex_init(&user->mutex, NULL);
    pthread_cond_init(&user->cond, NULL);
    user->r = 0;
    user->next = NULL;

    return user;
    
}

static inline int fifo_user_destroy(struct fifo_user_t *user)
{
    user->r = 0;
    pthread_cond_destroy(&user->cond);
    pthread_mutex_destroy(&user->mutex);

    SAFE_FREE(user);

    return 0;
    
}

static inline enum fifo_errno _fifo_fini(struct fifo_t *fifo)
{
    struct fifo_user_t *next = NULL;

    while (NULL != fifo->user_list) {
        next = fifo->user_list->next;
        (void)fifo_user_destroy(fifo->user_list);
        fifo->user_list = next;
    }

    pthread_mutex_destroy(&fifo->user_list_mutex);

    fifo->over = FIFO_INVALID_IDX;
    fifo->w = 0;
    fifo->r = 0;

    SAFE_FREE(fifo->buff);

    memset((char *)&fifo->param, 0, sizeof (fifo->param));

    return FIFO_SUCC;
    
}

static enum fifo_errno fifo_fini(struct fifo_t *fifo)
{
    enum fifo_errno e = FIFO_SUCC;

    if (NULL == fifo) {
        ERR("INVALID input param, fifo = %p\n", fifo);
        return FIFO_INVALID_INPUT;
    }

    e = _fifo_fini(fifo);

    pthread_mutex_lock(&sg_fifos.mutex);
    {
        fifo->inuse = 0;
    }
    pthread_mutex_unlock(&sg_fifos.mutex);

    return e;
    
}

#define FIFO_FIRST_ID 1
#define MAP_FIFO_IDX(id) ((id) - FIFO_FIRST_ID)

static int fifo_2_id(const struct fifo_t *fifo)
{
    int i = FIFO_FIRST_ID;
    struct fifos_t *p = &sg_fifos;

    for (i = FIFO_FIRST_ID; i < (ARRAY_SIZE(p->fifos) + FIFO_FIRST_ID); i++) {
        if ((struct fifo_t *)fifo == &(p->fifos[MAP_FIFO_IDX(i)])) {
            return i;
        }
    }

    return -1;
    
}

static struct fifo_t * id_2_fifo(int id)
{
    int id_idx = 0;
    struct fifos_t *p = &sg_fifos;

    id_idx = MAP_FIFO_IDX(id);
    if (id_idx < 0 || id_idx > (ARRAY_SIZE(p->fifos) - 1)) {
        return NULL;
    }

    return &(p->fifos[id_idx]);

}

static unsigned int fifo_input_freelen_get(unsigned int user_r, unsigned int fifo_w, unsigned int buffsize)
{
    return ((user_r + buffsize - fifo_w - 1) % buffsize);

}

static void fifo_output(void *dst, void *buff, unsigned int r, unsigned int size, unsigned int len)
{
    unsigned int part1 = 0;
    unsigned int part2 = 0;
    
    part1 = min(len, size - r);
    if (part1 > 0) {    
        memcpy((char *)dst, (char *)buff + r, part1);
    }

    part2 = len - part1;
    if (part2 > 0) {
        memcpy ((char *)dst + part1, (char *)buff, part2);
    }
    
    return;

}

static unsigned int fifo_user_data_drop(struct fifo_t *fifo, struct fifo_user_t *user, unsigned int len)
{
    unsigned int droplen = 0;
    struct fifo_head_t head;

    while (droplen < len) {
        fifo_output(&head, fifo->buff, user->r, fifo->param.size, sizeof (head));
        if (FIFO_SUCC != fifo_head_check(&head)) {
            user->r = fifo->r;
            return droplen;
        }
        droplen += sizeof (head);
        droplen += head.len;
        user->r = FIFO_IDX_NEXT_N(user->r, fifo->param.size, sizeof (head) + head.len);
    }

    fifo->over = user->r;
    
    return droplen;

}

static void fifo_user_r_modify(struct fifo_t *fifo, unsigned int len)
{
    struct fifo_user_t *user = NULL;
    unsigned int freelen = 0;

    pthread_mutex_lock(&fifo->user_list_mutex);
    {
        for (user = fifo->user_list; NULL != user; user = user->next) {
            freelen = fifo_input_freelen_get(user->r, fifo->w, fifo->param.size);
            if (freelen < len) {
                /* 调整user读指针 */
                DBG("USER: %p, need to drop, user->r: %u, fifo->w: %u, freelen: %u, inputlen: %u\n", user, user->r, fifo->w, freelen, len);
                pthread_mutex_lock(&user->mutex);
                {
                    freelen = fifo_input_freelen_get(user->r, fifo->w, fifo->param.size);
                    if (freelen < len) {
                        if (FIFO_INVALID_IDX == fifo->over) {
                            fifo_user_data_drop(fifo, user, len - freelen);
                        }
                        else {
                            user->r = fifo->over;
                        }
                    }
                }
                pthread_mutex_unlock(&user->mutex);
            }
        }
    }
    pthread_mutex_unlock(&fifo->user_list_mutex);

    fifo->over = FIFO_INVALID_IDX;
    return;
    
}

static void fifo_user_cond_signal(struct fifo_t *fifo)
{
    struct fifo_user_t *user = NULL;
    
    pthread_mutex_lock(&fifo->user_list_mutex);
    {
        for (user = fifo->user_list; NULL != user; user = user->next) {
            pthread_cond_signal(&user->cond);
        }
    }
    pthread_mutex_unlock(&fifo->user_list_mutex);

    return;

}

static void fifo_input(const void *src, void *buff, unsigned int w, unsigned int size, unsigned int len)
{
    unsigned int part1 = 0;
    unsigned int part2 = 0;

    part1 = min(len, size - w);
    if (part1 > 0) {
        memcpy((char *)buff + w, (char *)src, part1);
    }

    part2 = len - part1;
    if (part2 > 0) {
        memcpy((char *)buff, (char *)src + part1, part2);
    }

    return;

}

static enum fifo_errno _fifo_write(struct fifo_t *fifo, const void *buff, unsigned int len, int wait_ms)
{
    struct fifo_head_t head;

    fifo_user_r_modify(fifo, sizeof (struct fifo_head_t) + len);

    memset((char *)&head, 0, sizeof (head));
    head.magic = FIFO_HEAD_MAGIC;
    head.len = len;
    head.chksum = fifo_chksum_calc((unsigned char *)&head, offsetof(struct fifo_head_t, chksum));

    fifo_input(&head, fifo->buff, fifo->w, fifo->param.size, sizeof (head));

    fifo_input(buff, fifo->buff, FIFO_IDX_NEXT_N(fifo->w, fifo->param.size, sizeof (head)), fifo->param.size, len);

    fifo->r = fifo->w;
    fifo->w = FIFO_IDX_NEXT_N(fifo->w, fifo->param.size, sizeof (head) + len);

    fifo_user_cond_signal(fifo);

    return FIFO_SUCC;
    
}

static enum fifo_errno _fifo_user_add(struct fifo_t *fifo, FIFO_USER_HANDLE *h)
{
    struct fifo_user_t *user = NULL;
    struct fifo_user_t *p = NULL;

    user = fifo_user_create();
    if (NULL == user) {
        ERR("FAIL to create fifo user\n");
        return FIFO_USER_CREATE_FAIL;
    }

    user->r = fifo->w;

    pthread_mutex_lock(&fifo->user_list_mutex);
    {
        if (NULL == fifo->user_list) {
            fifo->user_list = user;
        }
        else {
            p = fifo->user_list;
            while (NULL != p->next) {
                p = p->next;
            }
            p->next = user;
        }
    }
    pthread_mutex_unlock(&fifo->user_list_mutex);

    if (NULL != h) {
        *h = (FIFO_USER_HANDLE)user;
    }

    return FIFO_SUCC;
    
}

static enum fifo_errno _fifo_user_read(struct fifo_t *fifo, FIFO_USER_HANDLE h, void *buff, unsigned int size, unsigned int *len, int wait_ms)
{
    struct fifo_user_t *user = (struct fifo_user_t *)h;
    enum fifo_errno e = FIFO_SUCC;
    struct fifo_head_t head;

    pthread_mutex_lock(&user->mutex);
    {
        if (fifo->w == user->r) {
            do {
                e = fifo_pthread_cond_wait(&user->cond, &user->mutex, wait_ms);
                if (FIFO_SUCC != e) {
                    pthread_mutex_unlock(&user->mutex);
                    return e;
                }
            } while (fifo->w == user->r);
        }

        /* 输出数据头 */
        fifo_output((char *)&head, (char *)fifo->buff, user->r, fifo->param.size, sizeof (head));
        /* 检查数据头, 并对异常的头做校验 */
        e = fifo_head_check(&head);
        if (FIFO_SUCC != e) {
            WARN("USER r(%u) wrong, reset it => fifo->r(%u)\n", user->r, fifo->r);
            user->r = fifo->r;
            pthread_mutex_unlock(&user->mutex);
            return e;
        }
        
        /* 检查用户缓存空间大小是否足够 */
        if (head.len > size) {
    		ERR("USER buff too small, data len: %u, user buff size: %u, data can't input.\n", head.len, size);
            pthread_mutex_unlock(&user->mutex);   
            return FIFO_USER_BUFF_NOT_ENOUGH;
        }

        /* 输出数据到用户缓存 */
        user->r = FIFO_IDX_NEXT_N(user->r, fifo->param.size, sizeof (head));
        fifo_output((char *)buff, (char *)fifo->buff, user->r, fifo->param.size, head.len);

        if (NULL != len) {
            *len = head.len;
        }

        user->r = FIFO_IDX_NEXT_N(user->r, fifo->param.size, head.len);
    }
    pthread_mutex_unlock(&user->mutex);
    
    return FIFO_SUCC;
    
}

static enum fifo_errno _fifo_user_remove(struct fifo_t *fifo, FIFO_USER_HANDLE h)
{
    struct fifo_user_t *user = NULL;
    struct fifo_user_t *p_prev = NULL;
    struct fifo_user_t *p = NULL;

    user = (struct fifo_user_t *)h;

    if (NULL == fifo->user_list) {
        WARN("fifo user_list has no user\n");
        return FIFO_NO_SUCH_USER;
    }

    p_prev = NULL;
    p = fifo->user_list;

    pthread_mutex_lock(&fifo->user_list_mutex);
    {
        while (NULL != p) {
            if (user == p) {
                if (NULL == p_prev) {
                    fifo->user_list = p->next;
                }
                else {
                    p_prev->next = p->next;
                }
                pthread_mutex_unlock(&fifo->user_list_mutex);
                fifo_user_destroy(user);
                return FIFO_SUCC;
            }

            p_prev = p;
            p = p->next;
        }   
    }
    pthread_mutex_unlock(&fifo->user_list_mutex);

    return FIFO_NO_SUCH_USER;
    
}

#ifdef __cplusplus
extern "C" {
#endif

int fifo_create(const struct fifo_param_t *param, enum fifo_errno *err)
{
    enum fifo_errno e = FIFO_SUCC;
    struct fifo_t *fifo = NULL;

    e = fifo_check(param);
    if (FIFO_SUCC != e) {
        FIFO_ERRNO_SET(err, e);
        return -1;
    }

    e = fifo_init(param, &fifo);
    if (FIFO_SUCC != e) {
        FIFO_ERRNO_SET(err, e);
        return -1;
    }

    FIFO_ERRNO_SET(err, e);
    return fifo_2_id(fifo);

}

int fifo_write(int id, const void *buff, unsigned int len, int wait_ms, enum fifo_errno *err)
{
    struct fifo_t *fifo = NULL;
    enum fifo_errno e = FIFO_SUCC;

    fifo = id_2_fifo(id);
    if (NULL == fifo) {
        FIFO_ERRNO_SET(err, FIFO_INVALID_FIFO);
        return -1;
    }

    if (NULL == buff || 0 == len) {
        ERR("INVALID input param, buff = %p, len = %u\n", buff, len);
        FIFO_ERRNO_SET(err, FIFO_INVALID_INPUT);
        return -1;
    }

    pthread_rwlock_rdlock(&fifo->lock);
    {
        if (!fifo->inuse) {
            FIFO_ERRNO_SET(err, FIFO_BAD_FIFO);
            pthread_rwlock_unlock(&fifo->lock);
            return -1;
        }
        e = _fifo_write(fifo, buff, len, wait_ms);
    }
    pthread_rwlock_unlock(&fifo->lock);

    FIFO_ERRNO_SET(err, e);

	return (FIFO_SUCC == e) ? (int)len : -1;

}

FIFO_USER_HANDLE fifo_user_add(int id, enum fifo_errno *err)
{
	struct fifo_t *fifo = NULL;
    enum fifo_errno e = FIFO_SUCC;
    FIFO_USER_HANDLE h = INVALID_FIFO_USER_HANDLE;

    fifo = id_2_fifo(id);
    if (NULL == fifo) {
        FIFO_ERRNO_SET(err, FIFO_INVALID_FIFO);
        return INVALID_FIFO_USER_HANDLE;
    }

    pthread_rwlock_rdlock(&fifo->lock);
    {
        if (!fifo->inuse) {
            FIFO_ERRNO_SET(err, FIFO_BAD_FIFO);
            pthread_rwlock_unlock(&fifo->lock);
            return INVALID_FIFO_USER_HANDLE;
        }
        e = _fifo_user_add(fifo, &h);
    }
    pthread_rwlock_unlock(&fifo->lock);

    FIFO_ERRNO_SET(err, e);

	return (FIFO_SUCC == e) ? h : INVALID_FIFO_USER_HANDLE;

}

int fifo_user_read(int id, FIFO_USER_HANDLE h, void *buff, unsigned int size, int wait_ms, enum fifo_errno *err)
{
	struct fifo_t *fifo = NULL;
    enum fifo_errno e = FIFO_SUCC;
    unsigned int len = 0;

    fifo = id_2_fifo(id);
    if (NULL == fifo) {
        FIFO_ERRNO_SET(err, FIFO_INVALID_FIFO);
        return -1;
    }

    if (INVALID_FIFO_USER_HANDLE == h || NULL == buff || size < sizeof (struct fifo_head_t)) {
        ERR("INVALID input param, h = %p, buff = %p, size = %u, min size = %u\n", h, buff, size, (unsigned int)sizeof (struct fifo_head_t));
        FIFO_ERRNO_SET(err, FIFO_INVALID_INPUT);
        return -1;
    }

    pthread_rwlock_rdlock(&fifo->lock);
    {
        if (!fifo->inuse) {
            FIFO_ERRNO_SET(err, FIFO_BAD_FIFO);
            pthread_rwlock_unlock(&fifo->lock);
            return -1;
        }
        e = _fifo_user_read(fifo, h, buff, size, &len, wait_ms);
    }
    pthread_rwlock_unlock(&fifo->lock);

    FIFO_ERRNO_SET(err, e);

	return (FIFO_SUCC == e) ? (int)len : -1;

}

int fifo_user_remove(int id, FIFO_USER_HANDLE h, enum fifo_errno *err)
{
    struct fifo_t *fifo = NULL;
    enum fifo_errno e = FIFO_SUCC;

    fifo = id_2_fifo(id);
    if (NULL == fifo) {
        FIFO_ERRNO_SET(err, FIFO_INVALID_FIFO);
        return -1;
    }

    if (INVALID_FIFO_USER_HANDLE == h) {
        ERR("INVALID input param, h = %p\n", h);
        FIFO_ERRNO_SET(err, FIFO_INVALID_INPUT);
        return -1;
    }

    pthread_rwlock_rdlock(&fifo->lock);
    {
        if (!fifo->inuse) {
            FIFO_ERRNO_SET(err, FIFO_BAD_FIFO);
            pthread_rwlock_unlock(&fifo->lock);
            return -1;
        }
        e = _fifo_user_remove(fifo, h);
    }
    pthread_rwlock_unlock(&fifo->lock);

    FIFO_ERRNO_SET(err, e);

	return (FIFO_SUCC == e) ? 0 : -1;

}

int fifo_destroy(int id, enum fifo_errno *err)
{
    struct fifo_t *fifo = NULL;
    enum fifo_errno e = FIFO_SUCC;
    
    fifo = id_2_fifo(id);
    if (NULL == fifo) {
        FIFO_ERRNO_SET(err, FIFO_INVALID_FIFO);
        return -1;
    }

    pthread_rwlock_wrlock(&fifo->lock);
    {
        if (!fifo->inuse) {
            FIFO_ERRNO_SET(err, FIFO_BAD_FIFO);
            pthread_rwlock_unlock(&fifo->lock);
            return -1;
        }
        e = fifo_fini(fifo);
    }
    pthread_rwlock_unlock(&fifo->lock);

    FIFO_ERRNO_SET(err, e);

    return (FIFO_SUCC == e) ? 0 : -1;

}

const char * fifo_strerr(enum fifo_errno err)
{
    return "fifo_strerr is not implemented";
    
}

static int g_test_id = -1;

void * TEST_fifo_user_read_thread(void *arg)
{
    FIFO_USER_HANDLE h_user = (FIFO_USER_HANDLE)arg;
    char tmpstr[128] = {0};
    enum fifo_errno e = FIFO_SUCC;
    
    for (; ;) {
        if (-1 == fifo_user_read(g_test_id, h_user, (char *)tmpstr, sizeof (tmpstr), -1, &e)) {
            ERR("USER fail to read fifo: %d, %s\n", g_test_id, fifo_strerr(e));
            break;
        }
        INFO("USER(%p) Got: %s\n", h_user, tmpstr);
        if (0 == strcmp(tmpstr, "test_string_99")) {
            INFO("USER(%p) Got EXIT\n", h_user);
            break;
        }
    }

    return NULL;
    
}

#define TEST_MAX_USER 80

void TEST_fifo(void)
{
    struct fifo_param_t param;
    enum fifo_errno e = FIFO_SUCC;
    FIFO_USER_HANDLE h_user[TEST_MAX_USER] = {INVALID_FIFO_USER_HANDLE};
    int i = 0;
    pthread_t tid[TEST_MAX_USER];
    char test_str[128] = {0};

    memset((char *)&param, 0, sizeof (param));
    param.size = 128 * 1024;

    g_test_id = fifo_create(&param, &e);
    if (-1 == g_test_id) {
        ERR("FAIL to create fifo, err = %d(%s)\n", e, fifo_strerr(e));
        return;
    }

    INFO("SUCC to create fifo, id = %d.\n", g_test_id);

    for (i = 0; i < TEST_MAX_USER; i++) {
        INFO("TRY to ADD user %d\n", i);
        h_user[i] = fifo_user_add(g_test_id, &e);
        if (INVALID_FIFO_USER_HANDLE == h_user[i]) {
            ERR("FAIL to add fifo user, id = %d, %s\n", g_test_id, fifo_strerr(e));
            goto exit;
        }
        INFO("SUCC to ADD user %d\n", i);

        pthread_create(&(tid[i]), NULL, TEST_fifo_user_read_thread, h_user[i]);
    }

    for (i = 0; i < 100; i++) {
        snprintf(test_str, sizeof (test_str), "test_string_%02d", i);
        fifo_write(g_test_id, test_str, strlen(test_str), -1, &e);
#if (defined(_WIN32) || defined(_WIN32_WCE) || defined(WIN64))
        Sleep(10);
#else
        usleep(10 * 1000);
#endif
    }

#if 1
    for (i = 0; i < TEST_MAX_USER; i++) {
        INFO("WAIT test thread %d\n", i);
        pthread_join(tid[i], NULL);
        INFO("SUCC TO WAIT test thread %d\n", i);
    }
#endif

exit:

    for (i = 0; i < TEST_MAX_USER; i++) {
        if (INVALID_FIFO_USER_HANDLE != h_user[i]) {
            if (-1 == fifo_user_remove(g_test_id, h_user[i], &e)) {
                WARN("FAIL to remove fifo user, id = %d, h_user[%d] = %p, %s\n", g_test_id, i, h_user[i], fifo_strerr(e));
            }
        }
    }

    if (-1 == fifo_destroy(g_test_id, &e)) {
        ERR("FAIL to destroy fifo, id = %d, err = %d(%s)\n", g_test_id, e, fifo_strerr(e));
        return;
    }

    INFO("SUCC to destroy fifo, id = %d.\n", g_test_id);

	return;
	
}

/* Windows下编译方法
 * gcc fifo.c -L . pthreadVC2.dll -o fifo.exe
 */
int main(int argc, char *argv[])
{
    printf("test fifo...\n");

    TEST_fifo();

    return 0;
    
}

#ifdef __cplusplus
}
#endif
