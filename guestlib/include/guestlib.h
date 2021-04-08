#ifndef __VGPU_GUESTLIB_H__
#define __VGPU_GUESTLIB_H__

#include <errno.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/stat.h>        /* For mode constants */
#include <fcntl.h>           /* For O_* constants */

#include "common/cmd_channel.h"
#include "migration.h"


#ifdef __cplusplus
#include <vector>

extern "C" {
#endif

void nw_init_guestlib(intptr_t api_id);
void nw_destroy_guestlib(void);

#ifdef __cplusplus
}

std::vector<struct command_channel*> command_channel_socket_tcp_guest_new();
#endif

#endif
