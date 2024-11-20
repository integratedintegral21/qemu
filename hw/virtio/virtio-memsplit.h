#ifndef QEMU_VIRTIO_MEMSPLIT_H
#define QEMU_VIRTIO_MEMSPLIT_H

#include "qemu/osdep.h"

#include "qemu/units.h"
#include "hw/virtio/virtio.h"
#include "net/announce.h"
#include "qemu/option_int.h"
#include "qom/object.h"
#include "qemu/event_notifier.h"

#define TYPE_VIRTIO_MEMSPLIT "virtio-memsplit"
OBJECT_DECLARE_SIMPLE_TYPE(VirtIOMemSplit, VIRTIO_MEMSPLIT)

struct VirtIOMemSplitReq;
struct VirtIOMemSplit {
    VirtIODevice parent_obj;
    uint64_t flags;
    struct VirtIOMemSplitReq *rq;
    EventNotifier irqfd;
};

typedef struct VirtIOMemSplitReq {
    VirtQueueElement elem;
    VirtIOMemSplit *dev;
    VirtQueue *vq;
} VirtIOMemSplitReq;

void virtio_memsplit_handle_vq(VirtIOMemSplit *s, VirtQueue *vq);

#define TYPE_VIRTIO_MEMSPLIT_PCI "virtio-memsplit-pci-base"

#endif /* QEMU_VIRTIO_MEMSPLIT_H */
