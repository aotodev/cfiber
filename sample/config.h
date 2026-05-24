#ifndef CFIBER_SAMPLE_CONFIG_H
#define CFIBER_SAMPLE_CONFIG_H

#ifndef FIBERS_PER_SLAB
#ifdef __arm__
#define FIBERS_PER_SLAB 4
#else
#define FIBERS_PER_SLAB 8
#endif
#endif

#ifndef FIBER_STACK_SIZE
#ifdef __arm__
#define FIBER_STACK_SIZE 0x200
#else
#define FIBER_STACK_SIZE 0x100000
#endif
#endif

#endif // CFIBER_SAMPLE_CONFIG_H
