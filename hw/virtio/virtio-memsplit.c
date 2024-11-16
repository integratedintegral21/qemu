#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/qdev-properties.h"

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

static void virtio_memsplit_handle_output(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtQueueElement *elem;
    int i;
    qemu_log("virtio memsplit vq handler\n");

    // Process all available buffers
    while ((elem = virtqueue_pop(vq, sizeof(VirtQueueElement)))) {
        // Assume a single buffer in the request
        char *buf = elem->out_sg[0].iov_base;
        size_t len = elem->out_sg[0].iov_len;

        // Handle the request
        qemu_log("Received request of size %zu\n", len);

        // For this example, just print the data
        for (i = 0; i < len; i++) {
            qemu_log("%d: %x\n", i, (int)(buf[i]));
        }

        // Return the buffer to the guest
        virtqueue_push(vq, elem, 0);
        virtio_notify(vdev, vq);

        // Free the element
        // virtqueue_elem_cleanup(&elem, NULL);
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
