from nightwatch.generator.c.stubs import function_implementation, unsupported_function_implementation
from .command_handler import *


def source(api: API, errors):
    handle_command_func_code = handle_command_function(
        api,
        api.callback_functions,
        list(api.real_functions) + list(api.callback_functions))
    code = f"""
#define __AVA__ 1
#define ava_is_worker 0
#define ava_is_guest 1

#include "guestlib.h"

{handle_command_header(api)}

#define CHECK_ERR(expr, failure, error_value)                              \\
    do {{                                                                   \\
        if (expr == failure) {{                                             \\
            fprintf(stderr, #expr " failed: %s\\n", strerror(error_value)); \\
            exit(EXIT_FAILURE);                                            \\
        }}                                                                  \\
    }} while (0)

#define CHECK_RET(expr, success, error_value)                              \\
    do {{                                                                   \\
        if (expr != success) {{                                             \\
            fprintf(stderr, #expr " failed: %s\\n", strerror(error_value)); \\
            exit(EXIT_FAILURE);                                            \\
        }}                                                                  \\
    }} while (0)

static int migration_barrier_participants = 0;
static int migration_barrier_index = -1;
static long long int migration_barrier_api_id = -1;
static int barrier_shm_fd = -1;
static const char* ava_barrier_shm_name = "/ava_barrier_shm";
typedef struct {{
    pthread_barrier_t barrier;
    int flag;
}} barrier_plus_flag;
static barrier_plus_flag* migration_barrier;

void __attribute__((constructor(1))) init_{api.identifier.lower()}_guestlib(void) {{
    __handle_command_{api.identifier.lower()}_init();
    {api.guestlib_init_prologue};
    nw_init_guestlib({api.number_spelling});
    {api.guestlib_init_epilogue};
    char* env_migration_barrier_participants = NULL;
    env_migration_barrier_participants = getenv("AVA_MIGRATION_BARRIER_PARTICIPANTS");
    if (env_migration_barrier_participants != NULL)
    {{
        migration_barrier_participants = atoi(env_migration_barrier_participants);
        printf("AVA_MIGRATION_BARRIER_PARTICIPANTS=%d\\n", migration_barrier_participants);
        fflush(stdout);
    }}
    char* env_migration_barrier_index = NULL;
    env_migration_barrier_index = getenv("AVA_MIGRATION_BARRIER_INDEX");
    if (env_migration_barrier_index != NULL)
    {{
        migration_barrier_index = atoi(env_migration_barrier_index);
        printf("AVA_MIGRATION_BARRIER_INDEX=%d\\n", migration_barrier_index);
        fflush(stdout);
    }}
    char* env_migration_barrier_api_id = NULL;
    env_migration_barrier_api_id = getenv("AVA_MIGRATION_BARRIER_API_ID");
    if (env_migration_barrier_api_id != NULL)
    {{
        migration_barrier_api_id = atoll(env_migration_barrier_api_id);
        printf("AVA_MIGRATION_BARRIER_API_ID=%lld\\n", migration_barrier_api_id);
        fflush(stdout);
    }}
    if (migration_barrier_participants && migration_barrier_api_id != -1)
    {{
        if (migration_barrier_index == 0)
        {{
            // only the first process creates the barrier
            CHECK_ERR((barrier_shm_fd = shm_open(ava_barrier_shm_name, O_RDWR | O_CREAT, 0666)), -1, errno);
            CHECK_ERR(ftruncate(barrier_shm_fd, sizeof(*migration_barrier)), -1, errno);
        }}
        else
        {{
            // just map an existing barrier
            do {{
                // loop until the shm object is created
                barrier_shm_fd = shm_open(ava_barrier_shm_name, O_RDWR, 0666);
            }} while (errno == ENOENT && barrier_shm_fd == -1);
            CHECK_ERR(barrier_shm_fd, -1, errno);
        }}
        CHECK_ERR((migration_barrier = mmap(NULL, sizeof(*migration_barrier), PROT_READ | PROT_WRITE, MAP_SHARED,
                                            barrier_shm_fd, 0)), MAP_FAILED, errno);

        if (migration_barrier_index == 0)
        {{
            int ret;
            pthread_barrierattr_t attr;
            CHECK_RET((ret = pthread_barrierattr_init(&attr)), 0, ret);
            CHECK_RET((ret = pthread_barrierattr_setpshared(&attr, PTHREAD_PROCESS_SHARED)), 0, ret);
            CHECK_RET((ret = pthread_barrier_init(&migration_barrier->barrier, &attr, migration_barrier_participants)), 0, ret);
            CHECK_RET((ret = pthread_barrierattr_destroy(&attr)), 0, ret);
            migration_barrier->flag = 1;
        }}
        else
        {{
            // spin waiting for barrier to be available
            // migration_barrier->flag is initialized to zero by shm_open with O_CREAT
            do {{
                ;
            }} while (!migration_barrier->flag);
        }}
    }}
}}

void __attribute__((destructor)) destroy_{api.identifier.lower()}_guestlib(void) {{
    {api.guestlib_fini_prologue};
    nw_destroy_guestlib();
    {api.guestlib_fini_epilogue};
    __handle_command_{api.identifier.lower()}_destroy();
    if (migration_barrier_participants && migration_barrier_index == 0)
    {{
        int ret;
        CHECK_RET((ret = pthread_barrier_destroy(&migration_barrier->barrier)), 0, ret);
        CHECK_RET(shm_unlink(ava_barrier_shm_name), 0, errno);
    }}
}}

{handle_command_func_code}

////// API function stub implementations

#define __chan nw_global_command_channel

{lines(function_implementation(f) for f in api.callback_functions)}
{lines(function_implementation(f) for f in api.real_functions)}
{lines(unsupported_function_implementation(f) for f in api.unsupported_functions)}

////// Replacement declarations

#define ava_begin_replacement 
#define ava_end_replacement 

{api.c_replacement_code}
    """.lstrip()
    return api.c_library_spelling, code
