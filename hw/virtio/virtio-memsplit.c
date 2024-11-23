#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/qdev-properties.h"
#include "hw/virtio/virtio-bus.h"

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

static void virtio_memsplit_interrupt_timer_cb(void *opaque) {
    qemu_log("QEMU timer callback\n");
    VirtIOMemSplitReq *req = opaque;
    VirtIOMemSplit *s = req->dev;
    VirtIODevice *vdev = VIRTIO_DEVICE(s);

    virtio_notify_irqfd(vdev, req->vq);
    virtio_memsplit_free_request(req);
}

static int virtio_memsplit_handle_request(VirtIOMemSplitReq *req) {
    int i;
    // VirtIOMemSplit *s = req->dev;
    // VirtIODevice *vdev = VIRTIO_DEVICE(s);

    qemu_log("Request handler called\n");
    char *buf = req->elem.out_sg[0].iov_base;
    size_t len = req->elem.out_sg[0].iov_len;
    qemu_log("Received request of size %zu\n", len);
    for (i = 0; i < len; i++) {
        qemu_log("%d: %x\n", i, (int)(buf[i]));
    }

    virtqueue_push(req->vq, &req->elem, 0);
    
    QEMUTimer *timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, virtio_memsplit_interrupt_timer_cb, req);
    timer_mod(timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 5000);

    return 0;
}

void virtio_memsplit_handle_vq(VirtIOMemSplit *s, VirtQueue *vq) {
    VirtIOMemSplitReq *req;

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
    // VirtIOMemSplit *ms = VIRTIO_MEMSPLIT(dev);
    // int ret;

    qemu_log("virtio memsplit vq handler\n");
    // ret = event_notifier_init(&ms->irqfd, 0);
    // if (ret) {
    //     error_setg(errp, "Failed to initialize event notifier");
    //     return;
    // }

    virtio_init(vdev, VIRTIO_ID_MEMSPLIT, 0);
    virtio_add_queue(vdev, QUEUE_SIZE, virtio_memsplit_handle_output);

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

static int virtio_memsplit_start_ioeventfd(VirtIODevice *vdev)
{
    qemu_log("virtio memsplit start ioeventfd\n");
    
    int r;
    VirtIOMemSplit *s = VIRTIO_MEMSPLIT(vdev);
    BusState *qbus = BUS(qdev_get_parent_bus(DEVICE(s)));
    VirtioBusClass *k = VIRTIO_BUS_GET_CLASS(qbus);
    r = k->set_guest_notifiers(qbus->parent, 1, true);
    if (r != 0) {
        error_report("virtio-blk failed to set guest notifier (%d), "
                     "ensure -accel kvm is set.", r);
        return -ENOSYS;
    }

    r = virtio_bus_set_host_notifier(VIRTIO_BUS(qbus), 0, true);
    if (r != 0) {
        error_report("virtio-blk failed to set host notifier (%d), "
                     "ensure -accel kvm is set.", r);
        return -ENOSYS;
    }

    qemu_log("virtio memsplit start ioeventfd done\n");
    return 0;
}

static void virtio_memsplit_stop_ioeventfd(VirtIODevice *vdev)
{
    qemu_log("virtio memsplit stop ioeventfd\n");
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
    vdc->start_ioeventfd = virtio_memsplit_start_ioeventfd;
    vdc->stop_ioeventfd = virtio_memsplit_stop_ioeventfd;
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
