#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "xs.h"

#define REFCNT_NUM(ptr) (*((int*)(ptr) - 1))
#define REFCNT_INCRE(ptr) (REFCNT_NUM(ptr)++)
#define REFCNT_DECRE(ptr) (REFCNT_NUM(ptr)--)
#define xs_literal_empty() (xs) { .space_left = 15 }

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

/* Forward declarations */
static inline bool xs_is_ptr(const xs *x);
static inline size_t xs_size(const xs *x);
static inline char *xs_data(const xs *x);
static inline size_t xs_capacity(const xs *x);
static inline int ilog2(uint32_t n);
static inline xs *xs_newempty(xs *x);

static inline bool xs_is_ptr(const xs *x)
{
    return x->is_ptr;
}
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
static inline int ilog2(uint32_t n)
{
    return 32 - __builtin_clz(n) - 1;
}
static inline xs *xs_newempty(xs *x)
{
    *x = xs_literal_empty();
    return x;
}

xs *xs_free(xs *x)
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

xs *xs_new(xs *x, const void *p)
{
    *x = xs_literal_empty();
    size_t len = strlen(p) + 1;
    if (len > 16) {
        x->capacity = ilog2(len) + 1;
        x->size = len - 1;
        x->is_ptr = true;
        x->ptr = malloc(((size_t) 1 << x->capacity) + 4);
        x->ptr += 4; /* leading 4 bytes = refcnt */
        REFCNT_NUM(x->ptr) = 1;
        memcpy(x->ptr, p, len);
    } else {
        memcpy(x->data, p, len);
        x->space_left = 15 - (len - 1);
    }
    return x;
}

/* change to specified size */
xs *xs_grow(xs *x, size_t len)
{
    if (len <= 15) { /* data on stack */
        if (xs_is_ptr(x)) { /* move data from heap to stack */
            char buf[16] = {0};
            memcpy(buf, x->ptr, len);
            xs_free(x);
            memcpy(x->data, buf, 16);
        } else {
            x->data[len] = 0;
        }
        x->space_left = 15 - len;
    } else { /* data on heap */
        size_t capacity = ilog2(len + 1) + 1;
        size_t size = (xs_size(x) > len)? len:xs_size(x);
        if (xs_is_ptr(x)) {
            if (REFCNT_NUM(x->ptr) > 1) { /* copy on write */
                char *tmp = x->ptr;
                REFCNT_DECRE(x->ptr);
                x->ptr = malloc(((size_t) 1 << capacity) + 4);
                x->ptr += 4; /* leading 4 bytes = refcnt */
                REFCNT_NUM(x->ptr) = 1;
                memcpy(x->ptr, tmp, size);
            } else { /* refcnt == 1 */
                x->ptr -= 4;
                x->ptr = realloc(x->ptr, ((size_t) 1 << capacity) + 4);
                x->ptr += 4;
            }
        }
        else { /* move data from stack to heap */
            char buf[16];
            memcpy(buf, x->data, 16);
            x->ptr = malloc(((size_t) 1 << capacity) + 4);
            x->ptr += 4;
            REFCNT_NUM(x->ptr) = 1;
            memcpy(x->ptr, buf, 16);
        }
        x->ptr[size] = 0;
        x->size = size;
        x->is_ptr = true;
        x->capacity = capacity;
    }
    return x;
}

xs *xs_concat(xs *string, const xs *prefix, const xs *suffix)
{
    size_t pres = xs_size(prefix), sufs = xs_size(suffix),
           size = xs_size(string), capacity = xs_capacity(string);

    char *pre = xs_data(prefix), *suf = xs_data(suffix),
         *data = xs_data(string);

    if (size + pres + sufs <= capacity) {
        memmove(data + pres, data, size);
        memcpy(data, pre, pres);
        memcpy(data + pres + size, suf, sufs + 1);
        string->space_left = 15 - (size + pres + sufs);
    } else {
        xs tmps = xs_literal_empty();
        xs_grow(&tmps, size + pres + sufs);
        char *tmpdata = xs_data(&tmps);
        memcpy(tmpdata + pres, data, size);
        memcpy(tmpdata, pre, pres);
        memcpy(tmpdata + pres + size, suf, sufs + 1);
        xs_free(string);
        *string = tmps;
        string->size = size + pres + sufs;
    }
    return string;
}

xs *xs_trim(xs *x, const char *trimset)
{
    if (!trimset[0])
        return x;

    x = xs_grow(x, xs_capacity(x)); /* copy on write */

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
    if (xs_is_ptr(dest))
        xs_free(dest);
    if (xs_is_ptr(src))
        REFCNT_INCRE(src->ptr);
    memcpy(dest->data, src->data, 16);
    return dest;
}

#include <stdio.h>

int main()
{
    printf("---original---\n");
    xs *src = xs_tmp("foobarbar");
    xs *dest = xs_tmp("original");
    xs *prefix = xs_tmp("@I like "), *suffix = xs_tmp("!!!");
    printf("src: [%s] size: %2zu\n", xs_data(src), xs_size(src));
    printf("dest: [%s] size: %2zu\n", xs_data(dest), xs_size(dest));
    printf("prefix: [%s] suffix: [%s]\n", xs_data(prefix), xs_data(suffix));
    xs_concat(src, prefix, suffix);
    printf("---after xs_concat(src, prefix, suffix)---\n");
    printf("src: [%s] size: %2zu\n", xs_data(src), xs_size(src));
    printf("dest: [%s] size: %2zu\n", xs_data(dest), xs_size(dest));
    xs_cpy(dest, src);
    printf("---after xs_cpy(dest, src)---\n");
    printf("src: [%s] size: %2zu\n", xs_data(src), xs_size(src));
    printf("dest: [%s] size: %2zu\n", xs_data(dest), xs_size(dest));
    printf("src refcnt: %d dest refcnt: %d\n", REFCNT_NUM(src->ptr), REFCNT_NUM(dest->ptr));
    printf("src: %p\ndest: %p\n", src->ptr, dest->ptr);
    xs_grow(dest, 19);
    printf("---after xs_grow(dest, 19)---\n");
    printf("src: [%s] size: %2zu\n", xs_data(src), xs_size(src));
    printf("dest: [%s] size: %2zu\n", xs_data(dest), xs_size(dest));
    printf("src refcnt: %d dest refcnt: %d\n", REFCNT_NUM(src->ptr), REFCNT_NUM(dest->ptr));
    printf("src: %p\ndest: %p\n", src->ptr, dest->ptr);
    xs_trim(dest, "!@");
    printf("---after xs_trim(dest, \"@!\")---\n");
    printf("src: [%s] size: %2zu\n", xs_data(src), xs_size(src));
    printf("dest: [%s] size: %2zu\n", xs_data(dest), xs_size(dest));
    return 0;
}