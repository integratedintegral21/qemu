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

static VirtIOMemSplitReq *virtio_memsplit_get_request(VirtIOMemSplit *s, VirtQueue *vq) {
    VirtIOMemSplitReq *req = virtqueue_pop(vq, sizeof(VirtIOMemSplitReq));
    if (req) {
        req->vq = vq;
        req->dev = s;
    }
    return req;
}

static void virtio_memsplit_free_request(VirtIOMemSplitReq *req) {
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
    qemu_log("System memory subregions:\n");

    RAMBlock *blk;
    MemoryRegion *mr;
    MemoryRegion *sub_mr;

    ms->hva_ram_start_ptr = NULL;
    ms->hva_ram_size = 0;
    QLIST_FOREACH(blk, &ram_list.blocks, next) {
        mr = blk->mr;
        if (strcmp(mr->name, "pc.ram") == 0) {
            ms->hva_ram_start_ptr = memory_region_get_ram_ptr(mr);
            ms->hva_ram_size = mr->size;
            break;
        }
    }

    // Walk over system memory and insert valid GPA ranges into 
    // ms object
    mr = get_system_memory();
    QLIST_INIT(&ms->gpa_ranges);
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
        }
    }
}

static void virtio_memsplit_interrupt_timer_cb(void *opaque) {
    qemu_log("QEMU timer callback\n");
    VirtIOMemSplit *s = opaque;
    VirtIODevice *vdev = VIRTIO_DEVICE(s);

    virtio_notify_config(vdev);

    // timer_mod(s->timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 500);
}

static int virtio_memsplit_handle_request(VirtIOMemSplitReq *req) {
    VirtIOMemSplit *s = req->dev;
    VirtIODevice *vdev = VIRTIO_DEVICE(s);

    qemu_log("Request handler called\n");
    if (req->elem.out_num > 0) {
        VirtIOMemSplitData *buf = req->elem.out_sg[0].iov_base;
        qemu_log("Received request of size %d\n", buf->size);
        qemu_log("%s\n", buf->data);
    }

    if (req->elem.in_num > 0) {
        void *buf = req->elem.in_sg[0].iov_base;
        size_t buf_len = req->elem.in_sg[0].iov_len;
        qemu_log("Sending request of size %zu\n", buf_len);

        const char *res_data = "Hello, driver!";
        size_t res_data_len = strlen(res_data) + 1;  // terminated with \0

        res_data_len = res_data_len > buf_len ? buf_len : res_data_len;  // Truncate if necessary
        memcpy(buf, (void*)res_data, res_data_len);

        virtqueue_push(req->vq, &req->elem, req->elem.in_num);
        virtio_notify(vdev, req->vq);
    }

    virtio_memsplit_free_request(req);

    return 0;
}

void virtio_memsplit_handle_vq(VirtIOMemSplit *s, VirtQueue *vq) {
    VirtIOMemSplitReq *req;
    qemu_log("virtio_memsplit_handle_vq called\n");

    bool suppress_notifications = virtio_queue_get_notification(vq);

    do {
        if (suppress_notifications) {
            virtio_queue_set_notification(vq, 0);
        }

        while ((req = virtio_memsplit_get_request(s, vq))) {
            if (virtio_memsplit_handle_request(req)) {
                virtqueue_detach_element(req->vq, &req->elem, 0);
                virtio_memsplit_free_request(req);
                break;
            }
        }
        if (suppress_notifications) {
            virtio_queue_set_notification(vq, 1);
        }
    } while (!virtio_queue_empty(vq));

}

static void virtio_memsplit_handle_output(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOMemSplit *s = (VirtIOMemSplit *)vdev;
    virtio_memsplit_handle_vq(s, vq);
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
    virtio_add_queue(vdev, QUEUE_SIZE, virtio_memsplit_handle_output);

    ms->timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, virtio_memsplit_interrupt_timer_cb, ms);
    timer_mod(ms->timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 500);

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
