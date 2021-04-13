#include "common/extensions/migration_barrier.h"

#include <errno.h>
#include <fcntl.h> /* For O_* constants */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h> /* For mode constants */

#define CHECK_ERR(expr, failure, error_value)                              \
    do {                                                                   \
        if (expr == failure) {                                             \
            fprintf(stderr, #expr " failed: %s\n", strerror(error_value)); \
            exit(EXIT_FAILURE);                                            \
        }                                                                  \
    } while (0)

#define CHECK_RET(expr, success, error_value)                              \
    do {                                                                   \
        if (expr != success) {                                             \
            fprintf(stderr, #expr " failed: %s\n", strerror(error_value)); \
            exit(EXIT_FAILURE);                                            \
        }                                                                  \
    } while (0)

static int migration_barrier_participants = 0;
static int migration_barrier_index = -1;
static long long int migration_barrier_api_id = -1;
static int barrier_shm_fd = -1;
static const char *ava_barrier_shm_name = "/ava_barrier_shm";

typedef struct {
    pthread_barrier_t barrier;
    int flag;
} barrier_plus_flag;

static barrier_plus_flag *migration_barrier;

void migration_barrier_init(void) {
    char *env_migration_barrier_participants = NULL;
    env_migration_barrier_participants =
        getenv("AVA_MIGRATION_BARRIER_PARTICIPANTS");
    if (env_migration_barrier_participants != NULL) {
        migration_barrier_participants =
            atoi(env_migration_barrier_participants);
        printf("AVA_MIGRATION_BARRIER_PARTICIPANTS=%d\n",
               migration_barrier_participants);
        fflush(stdout);
    }
    char *env_migration_barrier_index = NULL;
    env_migration_barrier_index = getenv("AVA_MIGRATION_BARRIER_INDEX");
    if (env_migration_barrier_index != NULL) {
        migration_barrier_index = atoi(env_migration_barrier_index);
        printf("AVA_MIGRATION_BARRIER_INDEX=%d\n", migration_barrier_index);
        fflush(stdout);
    }
    char *env_migration_barrier_api_id = NULL;
    env_migration_barrier_api_id = getenv("AVA_MIGRATION_BARRIER_API_ID");
    if (env_migration_barrier_api_id != NULL) {
        migration_barrier_api_id = atoll(env_migration_barrier_api_id);
        printf("AVA_MIGRATION_BARRIER_API_ID=%lld\n", migration_barrier_api_id);
        fflush(stdout);
    }
    if (migration_barrier_participants && migration_barrier_api_id != -1) {
        if (migration_barrier_index == 0) {
            // only the first process creates the barrier
            CHECK_ERR((barrier_shm_fd = shm_open(ava_barrier_shm_name,
                                                 O_RDWR | O_CREAT, 0666)),
                      -1, errno);
            CHECK_ERR(ftruncate(barrier_shm_fd, sizeof(*migration_barrier)), -1,
                      errno);
        } else {
            // just map an existing barrier
            do {
                // loop until the shm object is created
                barrier_shm_fd = shm_open(ava_barrier_shm_name, O_RDWR, 0666);
            } while (errno == ENOENT && barrier_shm_fd == -1);
            CHECK_ERR(barrier_shm_fd, -1, errno);
        }
        CHECK_ERR((migration_barrier = mmap(NULL, sizeof(*migration_barrier),
                                            PROT_READ | PROT_WRITE, MAP_SHARED,
                                            barrier_shm_fd, 0)),
                  MAP_FAILED, errno);

        if (migration_barrier_index == 0) {
            int ret;
            pthread_barrierattr_t attr;
            CHECK_RET((ret = pthread_barrierattr_init(&attr)), 0, ret);
            CHECK_RET((ret = pthread_barrierattr_setpshared(
                           &attr, PTHREAD_PROCESS_SHARED)),
                      0, ret);
            CHECK_RET(
                (ret = pthread_barrier_init(&migration_barrier->barrier, &attr,
                                            migration_barrier_participants)),
                0, ret);
            CHECK_RET((ret = pthread_barrierattr_destroy(&attr)), 0, ret);
            migration_barrier->flag = 1;
        } else {
            // spin waiting for barrier to be available
            // migration_barrier->flag is initialized to zero by shm_open with
            // O_CREAT
            do {
                ;
            } while (!migration_barrier->flag);
        }
    }
}

void migration_barrier_destroy(void) {
    if (migration_barrier_participants && migration_barrier_index == 0) {
        int ret;
        CHECK_RET((ret = pthread_barrier_destroy(&migration_barrier->barrier)),
                  0, ret);
        CHECK_RET(shm_unlink(ava_barrier_shm_name), 0, errno);
    }
}

void migration_barrier_wait(long long int call_id) {
    if (migration_barrier_participants && migration_barrier_api_id == call_id) {
        printf("migration barrier wait %d %lld %lld\n", migration_barrier_participants,
               migration_barrier_api_id, call_id);
        fflush(stdout);
        pthread_barrier_wait(&migration_barrier->barrier);
        printf("migration barrier waited %d %lld %lld\n", migration_barrier_participants,
               migration_barrier_api_id, call_id);
        fflush(stdout);
    }
}
