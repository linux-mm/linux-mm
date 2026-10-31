/* 
  Memory Tatic: PPM (Page-Plugger Under Memory).
  This File is for PPM Flags/Macros/Functions/Types.
  Engineer (brazilian): João Guilherme da Silva Freitas Lima <joanandalima@gmail.com>
 */

/* Types Include Guards */
#ifndef __PPM_TYPES_MEMORY 
#define __PPM_TYPES_MEMORY 

typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef uint64_t gfp_t;
/* For errors in Allocation */
#define __PPM_ALLOC_ERROR -102
/* For errors in Desallocation */
#define __PPM_FREE_ERROR 0x04 
/* For success */
#define __PPM_SUCCESS 0
#endif

/* Functions Include Guards */
#ifndef __PPM_FUNCTIONS_MEMORY
#define __PPM_FUNCTIONS_MEMORY 

/* Initializer */
void __init_ppm_memory();

/* Allocator Optimized */
int ppm_kalloc(unsigned long address, uint64_t size, gfp_t flags);

/* Desallocator Optimized */
int ppm_kfree(uint64_t size);

#endif

/* Flags Include Guards */
#ifndef __PPM_FLAGS_MEMORY 
#define __PPM_FLAGS_MEMORY

/* PPM inits/starts in Bit 54 of a 
   64-bit variable 
 */
gfp_t GFP_PLUG_MEMORY = (1ULL << 54); 

/* For free memory, the Bit 55 is 
   used 
 */
gfp_t GFP_FREE_T = (1ULL << 55);

#endif 

/* Macros Include Guards */
#ifndef __PPM_MACROS_MEMORY
#define __PPM_MACROS_MEMORY

/* For Kernel Kernel tests 2 Bits (54 and 55) in 
  1 Clock Cycle (or in small clocks, example: 5 Clocks), 
  is used a mask using | 
 */
gfp_t GFP_MASK = GFP_PLUG_MEMORY | GFP_FREE_T;

#endif

/* 
  Notes:
    E-Mail of RFC:
        [RFC] mm: page-fault: overcommit: PPM (Page-Plugger Under Memory)
    
    
    Assigned-Off-by: João <joanandalima@gmail.com>
 */
