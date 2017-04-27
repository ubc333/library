#ifndef CPEN333_PROCESS_SEMAPHORE_H
#define CPEN333_PROCESS_SEMAPHORE_H

#include "cpen333/os.h" // identify OS

#ifdef WINDOWS
#include "cpen333/process/impl/windows/semaphore.h"
#else
#include "cpen333/process/impl/posix/semaphore.h"
#endif

#include "cpen333/process/impl/semaphore_guard.h"

#endif //CPEN333_PROCESS_SEMAPHORE_H
