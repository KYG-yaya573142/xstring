#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define REFCNT_NUM(ptr) (*((int*)(ptr) - 1))
#define REFCNT_INCRE(ptr) (REFCNT_NUM(ptr)++)
#define REFCNT_DECRE(ptr) (REFCNT_NUM(ptr)--)

typedef union {
    /* allow strings up to 15 bytes to stay on the stack
     * use the last byte as a null terminator and to store flags
     * much like fbstring:
     * https://github.com/facebook/folly/blob/master/folly/docs/FBString.md
     */
    char data[16];

    struct {
        uint8_t filler[15],
            /* how many free bytes in this stack allocated string
             * same idea as fbstring
             */
            space_left : 4,
            /* if it is on heap, set to 1 */
            is_ptr : 1, flag1 : 1, flag2 : 1, flag3 : 1;
    };

    /* heap allocated */
    struct {
        char *ptr;
        /* supports strings up to 2^54 - 1 bytes */
        size_t size : 54,
            /* capacity is always a power of 2 (unsigned)-1 */
            capacity : 6;
        /* the last 4 bits are important flags */
    };
} xs;

static inline bool xs_is_ptr(const xs *x) { return x->is_ptr; }
static inline size_t xs_size(const xs *x)
{
    return xs_is_ptr(x) ? x->size : 15 - x->space_left;
}
static inline char *xs_data(const xs *x)
{
    return xs_is_ptr(x) ? (char *) x->ptr : (char *) x->data;
}
static inline size_t xs_capacity(const xs *x)
{
    return xs_is_ptr(x) ? ((size_t) 1 << x->capacity) - 1 : 15;
}
static inline size_t xs_is_ref(const xs *x)
{
    if (xs_is_ptr(x)) {
        if (REFCNT_NUM(x->ptr) > 1)
            return true;
    }
    return false;
}

#define xs_literal_empty() \
    (xs) { .space_left = 15 }

static inline int ilog2(uint32_t n) { return 32 - __builtin_clz(n) - 1; }

xs *xs_new(xs *x, const void *p)
{
    *x = xs_literal_empty();
    size_t len = strlen(p) + 1;
    if (len > 16) {
        x->capacity = ilog2(len) + 1;
        x->size = len - 1;
        x->is_ptr = true;
        x->ptr = malloc((size_t) 1 << x->capacity + 4);
        x->ptr += 4; /* leading 4 bytes = refcnt */
        REFCNT_NUM(x->ptr) = 1;
        memcpy(x->ptr, p, len);
    } else {
        memcpy(x->data, p, len);
        x->space_left = 15 - (len - 1);
    }
    return x;
}

static void xs_cow(xs *x)
{
    if (xs_is_ref(x)) {
        REFCNT_DECRE(x->ptr);
        xs_new(x, xs_data(x));
    }
}

/* Memory leaks happen if the string is too long but it is still useful for
 * short strings.
 * "" causes a compile-time error if x is not a string literal or too long.
 */
#define xs_tmp(x)                                          \
    ((void) ((struct {                                     \
         _Static_assert(sizeof(x) <= 16, "it is too big"); \
         int dummy;                                        \
     }){1}),                                               \
     xs_new(&xs_literal_empty(), "" x))

/* grow up to specified size */
xs *xs_grow(xs *x, size_t len)
{
    xs_cow(x);
    if (len <= xs_capacity(x))
        return x;
    len = ilog2(len) + 1;
    if (xs_is_ptr(x)) {
        x->ptr -=4;
        x->ptr = realloc(x->ptr, (size_t) 1 << len + 4);
        x->ptr +=4;
    } else {
        char buf[16];
        memcpy(buf, x->data, 16);
        x->ptr = malloc((size_t) 1 << len + 4);
        x->ptr -=4;
        REFCNT_NUM(x->ptr) = 1;
        memcpy(x->ptr, buf, 16);
    }
    x->is_ptr = true;
    x->capacity = len;
    return x;
}

static inline xs *xs_newempty(xs *x)
{
    *x = xs_literal_empty();
    return x;
}

static inline xs *xs_free(xs *x)
{
    if (xs_is_ptr(x)) {
        if (REFCNT_NUM(x->ptr) > 1) {
            REFCNT_DECRE(x->ptr);
            x->ptr = NULL;
        } else {
            x->ptr -= 4; /* leading 4 bytes = refcnt */
            free(x->ptr);
        }
    }
    return xs_newempty(x);
}

xs *xs_concat(xs *string, const xs *prefix, const xs *suffix)
{
    size_t pres = xs_size(prefix), sufs = xs_size(suffix),
           size = xs_size(string), capacity = xs_capacity(string);

    char *pre = xs_data(prefix), *suf = xs_data(suffix),
         *data = xs_data(string);

    xs_cow(string);

    if (size + pres + sufs <= capacity) {
        memmove(data + pres, data, size);
        memcpy(data, pre, pres);
        memcpy(data + pres + size, suf, sufs + 1);
    } else {
        xs tmps = xs_literal_empty();
        xs_grow(&tmps, size + pres + sufs);
        char *tmpdata = xs_data(&tmps);
        memcpy(tmpdata + pres, data, size);
        memcpy(tmpdata, pre, pres);
        memcpy(tmpdata + pres + size, suf, sufs + 1);
        xs_free(string);
        *string = tmps;
    }
    if (xs_is_ptr(string)) {
        string->size = size + pres + sufs;
    } else {
        string->space_left = 15 - (size + pres + sufs);
    }
    return string;
}

xs *xs_trim(xs *x, const char *trimset)
{
    if (!trimset[0])
        return x;

    xs_cow(x);

    char *dataptr = xs_data(x), *orig = dataptr;

    /* similar to strspn/strpbrk but it operates on binary data */
    uint8_t mask[32] = {0};

#define check_bit(byte) (mask[(uint8_t) byte / 8] &= 1 << (uint8_t) byte % 8)
#define set_bit(byte) (mask[(uint8_t) byte / 8] |= 1 << (uint8_t) byte % 8)

    size_t i, slen = xs_size(x), trimlen = strlen(trimset);

    for (i = 0; i < trimlen; i++)
        set_bit(trimset[i]);
    for (i = 0; i < slen; i++)
        if (!check_bit(dataptr[i]))
            break;
    for (; slen > 0; slen--)
        if (!check_bit(dataptr[slen - 1]))
            break;
    dataptr += i;
    slen -= i;

    /* reserved space as a buffer on the heap.
     * Do not reallocate immediately. Instead, reuse it as possible.
     * Do not shrink to in place if < 16 bytes.
     */
    memmove(orig, dataptr, slen);
    /* do not dirty memory unless it is needed */
    if (orig[slen])
        orig[slen] = 0;

    if (xs_is_ptr(x))
        x->size = slen;
    else
        x->space_left = 15 - slen;
    return x;
#undef check_bit
#undef set_bit
}

xs *xs_cpy(xs *dest, xs* src)
{
    xs_free(dest);
    memcpy(dest->data, src->data, 16);
    if (xs_is_ptr(src))
        REFCNT_INCRE(src->ptr);
    return dest;
}

/* Reentrant xs string tokenizer */
char *xs_strtok_r(xs *x, const char *delim, char **save_ptr)
{
    int src_flag = 0;

    char *s = NULL;
    char *end = NULL;

    if (x == NULL) {
        s = *save_ptr;
    } else { /* use the source x */
        xs_cow(x);
        s = xs_data(x);
        src_flag = 1;
    }

    if (*s == '\0') {
        *save_ptr = s;
        return NULL;
    }

    /* Scan leading delimiters */
    s += strspn(s, delim);
    if (*s == '\0') {
        *save_ptr = s;
        return NULL;
    }

    /* Find the end of the token */
    end = s + strcspn(s, delim);
    if (*end == '\0') {
        *save_ptr = end;
        return s;
    }

    /* Terminate the token and make *SAVE_PTR point past it */
    *end = '\0';
    *save_ptr = end + 1;

    /* contents after the first tok is no longer accessible for x */
    if (src_flag) {
        if (xs_is_ptr(x)) {
            x->size = (size_t) (end - xs_data(x));
        } else {
            x->space_left = 15 - (size_t) (end - xs_data(x));
        }
    }
    return s;
}

char *xs_strtok(xs *x, const char *delim)
{
    static char *old;
    return xs_strtok_r(x, delim, &old);
}

#include <stdio.h>

int main()
{
    printf("---original---\n");
    printf("---after xs_cpy(dest, src)---\n");
    xs *src = xs_new(&xs_literal_empty(), "|foo|bar|||bar|bar!!!|||");
    xs *dest = xs_tmp("original");    
    xs_cpy(dest, src);
    printf("src: [%s] size: %2zu\n", xs_data(src), xs_size(src));
    printf("dest: [%s] size: %2zu\n", xs_data(dest), xs_size(dest));
    printf("src refcnt: %d dest refcnt: %d\n", REFCNT_NUM(src->ptr), REFCNT_NUM(dest->ptr));
    printf("src: %p\ndest: %p\n", src->ptr, dest->ptr);

    printf("---after xs_strtok(dest, \"|\")---\n");
    printf("tok: %s\n", xs_strtok(dest, "|"));
    printf("src: [%s] size: %2zu\n", xs_data(src), xs_size(src));
    printf("dest: [%s] size: %2zu\n", xs_data(dest), xs_size(dest));
    printf("src refcnt: %d dest refcnt: %d\n", REFCNT_NUM(src->ptr), REFCNT_NUM(dest->ptr));
    printf("src: %p\ndest: %p\n", src->ptr, dest->ptr);
    for (int i = 0; i < 5; i++) {
        printf("---after xs_strtok(NULL, \"|\")---\n");
        printf("tok: %s\n", xs_strtok(NULL, "|"));
        printf("dest: [%s] size: %2zu\n", xs_data(dest), xs_size(dest));
    }
    return 0;
}