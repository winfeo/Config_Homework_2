#include_next <sys/mman.h>

#if defined(NEED_MEMFD_CREATE) || defined(NEED_FEXECVE)
# define memfd_create(name, flags) ({errno = ENOSYS; -1;})
#endif
