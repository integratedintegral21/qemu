#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/qdev-properties.h"
#include "hw/virtio/virtio-bus.h"
#include "exec/cpu-common.h"  
#include "exec/address-spaces.h"
#include "exec/ramlist.h"
#include "exec/ramblock.h"

#include "standard-headers/linux/virtio_ids.h"

#include "virtio-memsplit.h"

#define QUEUE_SIZE 16

#define PAGE_BITS        12
#define PAGE_SIZE        (1 << PAGE_BITS)
#define PAGE_OFFSET_MASK (PAGE_SIZE - 1) 

static const VMStateDescription vmstate_virtio_memsplit = {
    .name = "virtio-memsplit",
    .minimum_version_id = 9,
    .version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_VIRTIO_DEVICE,
        VMSTATE_END_OF_LIST()
    },
};

static inline bool is_page_aligned(void *ptr) {
    return ((uint64_t)ptr) && PAGE_OFFSET_MASK == 0;
}

static struct VirtIOMemSplitReq *virtio_memsplit_get_request(VirtIOMemSplit *s, VirtQueue *vq) {
    struct VirtIOMemSplitReq *req = virtqueue_pop(vq, sizeof(struct VirtIOMemSplitReq));
    if (req) {
        req->vq = vq;
        req->dev = s;
    }
    return req;
}

static void virtio_memsplit_free_request(struct VirtIOMemSplitReq *req) {
    g_free(req);
}

static void *gpa2hva(hwaddr addr, uint64_t size, Error **errp) {
    Int128 gpa_region_size;
    MemoryRegionSection mrs = memory_region_find(get_system_memory(),
                                                 addr, size);

    if (!mrs.mr) {
        error_setg(errp, "No memory is mapped at address 0x%" HWADDR_PRIx, addr);
        return NULL;
    }

    if (!memory_region_is_ram(mrs.mr) && !memory_region_is_romd(mrs.mr)) {
        error_setg(errp, "Memory at address 0x%" HWADDR_PRIx " is not RAM", addr);
        memory_region_unref(mrs.mr);
        return NULL;
    }

    gpa_region_size = int128_make64(size);
    if (int128_lt(mrs.size, gpa_region_size)) {
        error_setg(errp, "Size of memory region at 0x%" HWADDR_PRIx
                   " exceeded.", addr);
        memory_region_unref(mrs.mr);
        return NULL;
    }
    return qemu_map_ram_ptr(mrs.mr->ram_block, mrs.offset_within_region);
}

static uint64_t vtop(void *ptr, Error **errp)
{
    uint64_t pinfo;
    uint64_t ret = -1;
    uintptr_t addr = (uintptr_t) ptr;
    uintptr_t pagesize = qemu_real_host_page_size();
    off_t offset = addr / pagesize * sizeof(pinfo);
    int fd;

    fd = open("/proc/self/pagemap", O_RDONLY);
    if (fd == -1) {
        error_setg_errno(errp, errno, "Cannot open /proc/self/pagemap");
        return -1;
    }

    /* Force copy-on-write if necessary.  */
    qatomic_add((uint8_t *)ptr, 0);

    if (pread(fd, &pinfo, sizeof(pinfo), offset) != sizeof(pinfo)) {
        error_setg_errno(errp, errno, "Cannot read pagemap");
        goto out;
    }
    if ((pinfo & (1ull << 63)) == 0) {
        error_setg(errp, "Page not present");
        goto out;
    }
    ret = ((pinfo & 0x007fffffffffffffull) * pagesize) | (addr & (pagesize - 1));

out:
    close(fd);
    return ret;
}

static uint64_t gpa2hpa(hwaddr gpa, Error **errp) {
    void *hva = gpa2hva(gpa, 1, errp);
    if (hva == NULL) {
        return 0;
    }

    return vtop(hva, errp);
}

static void init_ram_info(VirtIOMemSplit *ms) {
    MemoryRegion *mr;
    MemoryRegion *sub_mr;

    // Walk over system memory and insert valid GPA ranges into 
    // ms object
    mr = get_system_memory();
    QLIST_INIT(&ms->gpa_ranges);

    qemu_log("System memory subregions:\n");
    QTAILQ_FOREACH(sub_mr, &mr->subregions, subregions_link) {
        if (strcmp(sub_mr->name, "ram-below-4g") == 0 ||
            strcmp(sub_mr->name, "ram-above-4g") == 0) {
            qemu_log("Found %s memory region\n", sub_mr->name);

            hwaddr gpa_start = sub_mr->addr;
            hwaddr gpa_end   = gpa_start + sub_mr->size - 1;
            qemu_log("Subregion gpa range: 0x%lx - 0x%lx\n", gpa_start, gpa_end);

            GPARange *gpa_range = malloc(sizeof(GPARange));
            gpa_range->start = gpa_start;
            gpa_range->size = sub_mr->size;

            QLIST_INSERT_HEAD(&ms->gpa_ranges, gpa_range, next);

            uint8_t *hva = memory_region_get_ram_ptr(sub_mr);
            qemu_log("Subregion hva range: %p - %p\n", hva, hva + sub_mr->size - 1);

            if (ms->hva_ram_start_ptr == NULL || ms->hva_ram_start_ptr > hva) {
                ms->hva_ram_start_ptr = hva;
            }
            ms->hva_ram_size += sub_mr->size;
        }
    }
}

static void virtio_memsplit_handle_gpa_req(struct VirtIOMemSplitReq *req) {
    VirtIOMemSplit *s = req->dev;
    VirtIODevice *vdev = VIRTIO_DEVICE(s);
    int i;
    static int pfns_received = 0;

    qemu_log("GPA handler: n elements = %u\n", req->elem.out_num);
    if (req->elem.out_num > 0) {
        struct VirtIOSendGpaData *buf = req->elem.out_sg[0].iov_base;
        for (i = 0; i < 128 && buf->pfn[i] > 0; i++) {
            pfns_received++;
        }
    }

    qemu_log("PFNs received so far: %d\n", pfns_received);

    virtqueue_push(req->vq, &req->elem, 128 * (sizeof *req));
    virtio_notify(vdev, req->vq);
  
    virtio_memsplit_free_request(req);
}

static void virtio_memsplit_handle_gpa(VirtIODevice *vdev, VirtQueue *vq)
{
    struct VirtIOMemSplitReq *req;
    VirtIOMemSplit *ms = (VirtIOMemSplit *)vdev;

    qemu_log("Send GPA handler called\n");

    while((req = virtio_memsplit_get_request(ms, vq))) {
        virtio_memsplit_handle_gpa_req(req);
    }
}

static uint64_t virtio_memsplit_get_features(VirtIODevice *vdev, uint64_t features, 
                                        Error **errp) 
{
    qemu_log("virtio memsplit get features\n");
    return features;
}

static void virtio_memsplit_realize(DeviceState *dev, Error **errp) 
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VirtIOMemSplit *ms = VIRTIO_MEMSPLIT(dev);
    GPARange *gpa_range;
    int ret;

    init_ram_info(ms);

    if (ms->hva_ram_size == 0) {
        error_setg(errp, "Could not find guest RAM region(s)");
        return;
    }

    // Test mappings
    qemu_log("gpa sectors:\n");
    QLIST_FOREACH(gpa_range, &ms->gpa_ranges, next) {
        hwaddr gpa_start = gpa_range->start;
        hwaddr gpa_end = gpa_range->start + gpa_range->size - 1;
        qemu_log("GPA: 0x%lx - 0x%lx\n", gpa_start, gpa_end);
        qemu_log("HVA: %p - %p\n", gpa2hva(gpa_start, 1, errp), gpa2hva(gpa_end, 1, errp));
        if (*errp) {
            error_setg(errp, "Failed to map GPA to HPA");
            return;
        }

        qemu_log("HPA: 0x%lx - 0x%lx\n", gpa2hpa(gpa_start, errp), gpa2hpa(gpa_end, errp));
        if (*errp) {
            error_setg(errp, "Failed to map GPA to HPA");
            return;
        }
    }

    ret = event_notifier_init(&ms->irqfd, 0);
    if (ret) {
        error_setg(errp, "Failed to initialize event notifier");
        return;
    }

    virtio_init(vdev, VIRTIO_ID_MEMSPLIT, 0);
    ms->gpa_vq = virtio_add_queue(vdev, QUEUE_SIZE, virtio_memsplit_handle_gpa);

    qemu_log("virtio memsplit realize\n");
}

static void virtio_memsplit_unrealize(DeviceState *dev)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    virtio_del_queue(vdev, 0);
    virtio_cleanup(vdev);
    qemu_log("Device unrealized\n");
}

static void virtio_instance_init(Object *obj) 
{
    qemu_log("virtio memsplit instance init\n");
}

static void virtio_memsplit_init(ObjectClass *klass, void *data) 
{
    qemu_log("virtio memsplit init\n");

    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->vmsd = &vmstate_virtio_memsplit;
    VirtioDeviceClass *vdc = VIRTIO_DEVICE_CLASS(klass);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    vdc->realize = virtio_memsplit_realize;
    vdc->unrealize = virtio_memsplit_unrealize;
    vdc->get_features = virtio_memsplit_get_features;
}

static const TypeInfo virtio_memsplit_info = 
{
    .name = TYPE_VIRTIO_MEMSPLIT,
    .parent = TYPE_VIRTIO_DEVICE,
    .instance_size = sizeof(VirtIOMemSplit),
    .instance_init = virtio_instance_init,
    .class_init = virtio_memsplit_init
};

static void virtio_register_types(void)
{
    type_register_static(&virtio_memsplit_info);
}

type_init(virtio_register_types)
