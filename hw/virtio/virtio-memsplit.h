#ifndef QEMU_VIRTIO_MEMSPLIT_H
#define QEMU_VIRTIO_MEMSPLIT_H

#include "qemu/osdep.h"

#include "qemu/units.h"
#include "hw/virtio/virtio.h"
#include "net/announce.h"
#include "qemu/option_int.h"
#include "qom/object.h"

#define TYPE_VIRTIO_MEMSPLIT "virtio-memsplit"
OBJECT_DECLARE_SIMPLE_TYPE(VirtIOMemSplit, VIRTIO_MEMSPLIT)

struct VirtIOMemSplitReq;
struct VirtIOMemSplit {
    VirtIODevice parent_obj;
    uint64_t flags;
    struct VirtIOMemSplitReq *rq;
};

typedef struct VirtIOMemSplitReq {
    VirtQueueElement elem;
    VirtQueue *vq;
} VirtIOMemSplitReq;

#define TYPE_VIRTIO_MEMSPLIT_PCI "virtio-memsplit-pci-base"

#endif /* QEMU_VIRTIO_MEMSPLIT_H */
