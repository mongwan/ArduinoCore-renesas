/* Internal: checkpoints kept in .noinit RAM so that the last position before
   a hang, an assert or a HardFault survives the reset that follows. */
#pragma once
#include <stdint.h>

enum CoexCp : uint8_t {
  CP_NONE = 0,
  CP_LOCK_WAIT = 1,
  CP_LOCK_TAKEN = 2,
  CP_UNLOCK = 3,
  CP_ACQ_ENTER = 4,
  CP_ACQ_OK = 5,
  CP_ACQ_DEFER = 6,
  CP_ACQ_TIMEOUT = 7,
  CP_RELEASE = 8,
  CP_HS_WAIT = 9,
  CP_WAIT_1MS = 10,
  CP_POLL_YIELD = 11,
  CP_NET_LWIP = 20,
  CP_NET_LWIP_DONE = 21,
  CP_NET_UDP_BEGIN = 22,
  CP_NET_UDP_WRITE = 23,
  CP_NET_UDP_END = 24,
  CP_NET_UDP_DONE = 25,
  CP_NET_SLEEP = 26,
  CP_LOOP = 30,
  CP_SEND_UDP = 31,
};

/* Record a checkpoint (any context, a few stores). */
void coex_cp(CoexCp cp);
/* Remember which task is the net task (for the report). */
void coex_diag_set_net_task(void* handle);
