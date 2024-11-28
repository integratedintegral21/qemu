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

static const VMStateDescription vmstate_virtio_memsplit = {
    .name = "virtio-memsplit",
    .minimum_version_id = 9,
    .version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_VIRTIO_DEVICE,
        VMSTATE_END_OF_LIST()
    },
};

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

static void init_ram_info(VirtIOMemSplit *ms) {
    MemoryRegion *sys_mr = get_system_memory();
    MemoryRegion *subregion;
    qemu_log("System memory subregions:\n");

    ms->below_4g_ram = NULL;
    ms->above_4g_ram = NULL;
    ms->hva_ram_size = 0;
    QTAILQ_FOREACH(subregion, &sys_mr->subregions, subregions_link) {
        if (strcmp(subregion->name, "ram-below-4g") == 0) {
            qemu_log("Found %s memory region\n", subregion->name);

            hwaddr gpa_start = subregion->addr;
            hwaddr gpa_end   = gpa_start + subregion->size - 1;
            qemu_log("Subregion gpa range: 0x%lx - 0x%lx\n", gpa_start, gpa_end);
            qemu_log("Alias offset: 0x%lx\n", subregion->alias_offset);

            uint8_t *hva_start = memory_region_get_ram_ptr(subregion);
            uint8_t *hva_end   = hva_start + subregion->size - 1;
            qemu_log("Subregion hva range: %p - %p\n", hva_start, hva_end);

            ms->below_4g_ram = subregion;
            ms->hva_ram_start_ptr = hva_start;
            ms->hva_ram_size += subregion->size;
        }

        if (strcmp(subregion->name, "ram-above-4g") == 0) {
            qemu_log("Found %s memory region\n", subregion->name);

            hwaddr gpa_start = subregion->addr;
            hwaddr gpa_end   = gpa_start + subregion->size - 1;
            qemu_log("Subregion gpa range: 0x%lx - 0x%lx\n", gpa_start, gpa_end);
            qemu_log("Alias offset: 0x%lx\n", subregion->alias_offset);

            uint8_t *hva_start = memory_region_get_ram_ptr(subregion);
            uint8_t *hva_end   = hva_start + subregion->size - 1;
            qemu_log("Subregion hva range: %p - %p\n", hva_start, hva_end);

            ms->above_4g_ram = subregion;
            ms->hva_ram_size += subregion->size;
        }
    }
}

static hwaddr hva2gpa(VirtIOMemSplit *ms, uint8_t *ptr) {
    MemoryRegion *mr;
    
    size_t offset_in_region = ptr - ms->hva_ram_start_ptr;
    if (offset_in_region < ms->below_4g_ram->size) {
        mr = ms->below_4g_ram;
    } else {
        mr = ms->above_4g_ram;
        offset_in_region = ptr - (uint8_t*) memory_region_get_ram_ptr(ms->above_4g_ram);
    }

    return mr->addr + offset_in_region;  // Linear mapping;
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
    int ret;

    init_ram_info(ms);

    if (!ms->below_4g_ram) {
        error_setg(errp, "Failed to initialize RAM regions");
        return;
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
