#ifndef QEMU_VIRTIO_MEMSPLIT_H
#define QEMU_VIRTIO_MEMSPLIT_H

#include "qemu/osdep.h"

#include "qemu/queue.h"
#include "qemu/units.h"

#include "hw/virtio/virtio.h"
#include "net/announce.h"
#include "qemu/option_int.h"
#include "qom/object.h"
#include "qemu/event_notifier.h"

#define TYPE_VIRTIO_MEMSPLIT "virtio-memsplit"
OBJECT_DECLARE_SIMPLE_TYPE(VirtIOMemSplit, VIRTIO_MEMSPLIT)

#define VIRTIO_MEMSPLIT_SEND_GPA_CAPACITY 128

struct VirtIOMemSplitReq;
struct GPARange;

struct VirtIOMemSplit {
    VirtIODevice parent_obj;
    uint64_t flags;
    struct VirtIOMemSplitReq *rq;
    EventNotifier irqfd;
    
    struct VirtQueue *gpa_vq;

    // RAM utils
    uint8_t *hva_ram_start_ptr;
    uint64_t hva_ram_size;
    QLIST_HEAD(, GPARange) gpa_ranges;
};

struct VirtIOMemSplitReq {
    VirtQueueElement elem;
    VirtIOMemSplit *dev;
    VirtQueue *vq;
};

struct VirtIOSendGpaData {
    uint64_t pfn[VIRTIO_MEMSPLIT_SEND_GPA_CAPACITY];
};

typedef struct GPARange {
    hwaddr start;
    size_t size;

    QLIST_ENTRY(GPARange) next;
} GPARange;

#define TYPE_VIRTIO_MEMSPLIT_PCI "virtio-memsplit-pci-base"

#endif /* QEMU_VIRTIO_MEMSPLIT_H */
