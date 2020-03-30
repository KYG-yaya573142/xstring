#include <stddef.h>
#include <stdint.h>

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

/* Forward declarations */
xs *xs_free(xs *x);
xs *xs_new(xs *x, const void *p);
xs *xs_grow(xs *x, size_t len);
xs *xs_concat(xs *string, const xs *prefix, const xs *suffix);
xs *xs_trim(xs *x, const char *trimset);
xs *xs_cpy(xs *dest, xs* src);