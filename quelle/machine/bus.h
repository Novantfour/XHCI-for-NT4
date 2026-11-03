/*  machine/bus.h -- Registerzugriff und DMA.
 *
 *  Das ist die Stelle, an der FreeBSD gegen Linux gewinnt: der ganze
 *  Hardwarezugriff des USB-Stapels geht durch diese zwoelf Namen, und
 *  jeder einzelne hat unter NT eine genaue Entsprechung.
 *
 *      bus_space_read_4   Speicher    READ_REGISTER_ULONG
 *                         E/A         READ_PORT_ULONG
 *      bus_dmamem_alloc               HalAllocateCommonBuffer
 *
 *  READ_PORT_* und HalAllocateCommonBuffer liegen in HAL.DLL, nicht in
 *  NTOSKRNL -- gegen tools/ntkrnlmp.def geprueft, dort fehlen sie
 *  erwartungsgemaess.  Der Treiber bindet gegen beide Bibliotheken.  */
#ifndef _MACHINE_BUS_H_
#define _MACHINE_BUS_H_
#include <sys/types.h>

/*  Der tag entscheidet, welcher Adressraum.  Ein Zweig, kein Aufruf --
 *  UHCI liegt im E/A-Raum, alles andere im Speicher, und das steht beim
 *  Anhaengen fest.  */

#define _NTU_IO(t)  ((t) == NTU_SPACE_IO)

uint8_t  ntu_r1(bus_space_tag_t, bus_space_handle_t, bus_size_t);
uint16_t ntu_r2(bus_space_tag_t, bus_space_handle_t, bus_size_t);
uint32_t ntu_r4(bus_space_tag_t, bus_space_handle_t, bus_size_t);
void     ntu_w1(bus_space_tag_t, bus_space_handle_t, bus_size_t, uint8_t);
void     ntu_w2(bus_space_tag_t, bus_space_handle_t, bus_size_t, uint16_t);
void     ntu_w4(bus_space_tag_t, bus_space_handle_t, bus_size_t, uint32_t);

#define bus_space_read_1(t,h,o)     ntu_r1((t),(h),(o))
#define bus_space_read_2(t,h,o)     ntu_r2((t),(h),(o))
#define bus_space_read_4(t,h,o)     ntu_r4((t),(h),(o))
#define bus_space_write_1(t,h,o,v)  ntu_w1((t),(h),(o),(v))
#define bus_space_write_2(t,h,o,v)  ntu_w2((t),(h),(o),(v))
#define bus_space_write_4(t,h,o,v)  ntu_w4((t),(h),(o),(v))

/*  Auf x86 ordnen READ_/WRITE_REGISTER_* bereits; die Schranke ist ein
 *  leerer Rumpf, aber ausdruecklich einer -- nicht stillschweigend
 *  weggelassen.  */
#define BUS_SPACE_BARRIER_READ   1
#define BUS_SPACE_BARRIER_WRITE  2
#define bus_space_barrier(t,h,o,l,f)  ((void)0)

/* ---- DMA ---------------------------------------------------------- */

#define BUS_DMA_WAITOK       0x0000
#define BUS_DMA_NOWAIT       0x0001
#define BUS_DMA_COHERENT     0x0004
#define BUS_DMA_ZERO         0x0008
#define BUS_DMA_NOCACHE      0x0020
#define BUS_DMA_KEEP_PG_OFFSET 0x0400
#define BUS_DMA_ALLOCNOW     0x0010
#define BUS_SPACE_MAXADDR_32BIT  0xFFFFFFFFu
#define BUS_SPACE_MAXADDR        0xFFFFFFFFu
#define BUS_SPACE_MAXSIZE_32BIT  0xFFFFFFFFu
#define BUS_SPACE_MAXSIZE        0xFFFFFFFFu
#define BUS_DMA_LOCK         0
#define BUS_DMASYNC_PREREAD    1
#define BUS_DMASYNC_POSTREAD   2
#define BUS_DMASYNC_PREWRITE   4
#define BUS_DMASYNC_POSTWRITE  8

typedef struct bus_dma_segment {
    bus_addr_t ds_addr;
    bus_size_t ds_len;
} bus_dma_segment_t;

typedef void (*bus_dmamap_callback_t)(void *, bus_dma_segment_t *, int, int);
typedef int bus_dma_lock_op_t;
#define busdma_lock_mutex ((void *)0)

int  bus_dma_tag_create(bus_dma_tag_t parent, bus_size_t align,
                        bus_size_t bnd, bus_addr_t lowaddr,
                        bus_addr_t highaddr, void *ff, void *fa,
                        bus_size_t maxsize, int nseg, bus_size_t maxsegsz,
                        int flags, void *lf, void *la, bus_dma_tag_t *out);
int  bus_dma_tag_destroy(bus_dma_tag_t);
int  bus_dmamem_alloc(bus_dma_tag_t, void **, int, bus_dmamap_t *);
void bus_dmamem_free(bus_dma_tag_t, void *, bus_dmamap_t);
int  bus_dmamap_create(bus_dma_tag_t, int, bus_dmamap_t *);
int  bus_dmamap_destroy(bus_dma_tag_t, bus_dmamap_t);
int  bus_dmamap_load(bus_dma_tag_t, bus_dmamap_t, void *, bus_size_t,
                     bus_dmamap_callback_t, void *, int);
void bus_dmamap_unload(bus_dma_tag_t, bus_dmamap_t);
void bus_dmamap_sync(bus_dma_tag_t, bus_dmamap_t, int);
#endif
