/* Reproduce a successful allocator changing errno after failed open. */
#include <errno.h>
#include <stddef.h>
void *__real_g_malloc(size_t length);
void *__wrap_g_malloc(size_t length)
{
    void *result = __real_g_malloc(length);
    errno = EINVAL;
    return result;
}
