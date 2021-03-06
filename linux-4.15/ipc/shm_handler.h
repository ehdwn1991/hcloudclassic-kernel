#ifndef SHM_HANDLER_H
#define SHM_HANDLER_H

#include <linux/mm.h>
#include <linux/shm.h>

extern struct iolinker_struct shm_memory_linker;

void shm_handler_finalize(void);
void shm_handler_init(void);

#endif
