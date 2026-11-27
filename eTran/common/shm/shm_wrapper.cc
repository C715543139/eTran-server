#include <shm/shm_wrapper.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <new>

struct shm_wrapper *_shm_wrapper_create(const std::string &__name, size_t __len, int __oflag, mode_t __mode)
{
    int fd;
    void *addr;
    struct shm_wrapper *mem = NULL;

    mem = new (std::nothrow) shm_wrapper{};
    if (mem == NULL) {
        fprintf(stderr, "Failed to allocate memory for shared memory\n");
        return NULL;
    }

    fd = shm_open(__name.c_str(), __oflag, __mode);

    if (fd < 0) {
        fprintf(stderr, "Failed to open shared memory %s (%s)\n", __name.c_str(), strerror(errno));
        goto err;
    }

    if (__oflag & O_CREAT) {
        if (fchmod(fd, __mode) < 0 || ftruncate(fd, __len) < 0) {
            perror("Failed to initialize shared memory");
            goto err;
        }
    } else {
        struct stat status{};
        if (fstat(fd, &status) < 0 || status.st_size < 0 || static_cast<size_t>(status.st_size) < __len) {
            fprintf(stderr, "Shared memory %s is shorter than requested\n", __name.c_str());
            goto err;
        }
    }

    addr = mmap(NULL, __len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, 0);

    if (addr == MAP_FAILED) {
        fprintf(stderr, "Failed to map shared memory\n");
        goto err;
    }

    mem->name = __name;
    mem->fd = fd;
    mem->addr = addr;
    mem->size = __len;

    close(fd);

    return mem;
err:
    if (fd >= 0) {
        close(fd);
        if (__oflag & O_CREAT)
            shm_unlink(__name.c_str());
    }
    delete mem;
    return NULL;
}

struct shm_wrapper *shm_wrapper_create(const std::string &__name, size_t __len)
{
    shm_unlink(__name.c_str());
    return _shm_wrapper_create(__name, __len, O_RDWR | O_CREAT | O_EXCL, 0666);
}

struct shm_wrapper *shm_wrapper_attach(const std::string &__name, size_t __len)
{
    struct shm_wrapper *mem;

    mem = _shm_wrapper_create(__name, __len, O_RDWR, 0);
    if (!mem) {
        return NULL;
    }

    return mem;
}

void shm_wrapper_destroy(struct shm_wrapper *mem)
{
    if (!mem)
        return;
    if (munmap(mem->addr, mem->size) < 0) {
        fprintf(stderr, "Failed to unmap shared memory\n");
    }
    /* unlink this shm */
    if (shm_unlink(mem->name.c_str()) < 0) {
        fprintf(stderr, "Failed to unlink shared memory %s\n", mem->name.c_str());
    } else
        printf("%s has been destroyed.\n", mem->name.c_str());
    delete mem;

    return;
}

void shm_wrapper_detach(struct shm_wrapper *mem)
{
    if (!mem)
        return;
    /* we just unmap this memory */
    if (munmap(mem->addr, mem->size) < 0) {
        fprintf(stderr, "Failed to unmap shared memory\n");
    }
    printf("%s has been detached.\n", mem->name.c_str());
    delete mem;
    
    return;
}
