/*
 * Virtio memsplit PCI Bindings
 *
 * Copyright IBM, Corp. 2007
 * Copyright (c) 2009 CodeSourcery
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *  Paul Brook        <paul@codesourcery.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 * Contributions after 2012-01-13 are licensed under the terms of the
 * GNU GPL, version 2 or (at your option) any later version.
 */

#include "qemu/osdep.h"

#include "hw/qdev-properties.h"

#include "virtio-memsplit.h"
#include "hw/virtio/virtio-pci.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qom/object.h"

#include "standard-headers/linux/virtio_ids.h"

typedef struct VirtIOMemsplitPCI VirtIOMemsplitPCI;

/*
 * virtio-memsplit-pci: This extends VirtioPCIProxy.
 */
#define TYPE_VIRTIO_MEMSPLIT_PCI "virtio-memsplit-pci-base"
DECLARE_INSTANCE_CHECKER(VirtIOMemsplitPCI, VIRTIO_MEMSPLIT_PCI,
                         TYPE_VIRTIO_MEMSPLIT_PCI)

struct VirtIOMemsplitPCI {
    VirtIOPCIProxy parent_obj;
    VirtIOMemSplit vdev;
};

static Property virtio_memsplit_pci_properties[] = {
    DEFINE_PROP_UINT32("class", VirtIOPCIProxy, class_code, 0),
    DEFINE_PROP_BIT("ioeventfd", VirtIOPCIProxy, flags,
                    VIRTIO_PCI_FLAG_USE_IOEVENTFD_BIT, true),
    DEFINE_PROP_UINT32("vectors", VirtIOPCIProxy, nvectors,
                       DEV_NVECTORS_UNSPECIFIED),
    DEFINE_PROP_END_OF_LIST(),
};

static void virtio_memsplit_pci_realize(VirtIOPCIProxy *vpci_dev, Error **errp)
{
    VirtIOMemsplitPCI *dev = VIRTIO_MEMSPLIT_PCI(vpci_dev);
    DeviceState *vdev = DEVICE(&dev->vdev);

    vpci_dev->nvectors = 2;

    virtio_pci_force_virtio_1(vpci_dev);
    qdev_realize(vdev, BUS(&vpci_dev->bus), errp);
}

static void virtio_memsplit_pci_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioPCIClass *k = VIRTIO_PCI_CLASS(klass);
    PCIDeviceClass *pcidev_k = PCI_DEVICE_CLASS(klass);

    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    device_class_set_props(dc, virtio_memsplit_pci_properties);
    k->realize = virtio_memsplit_pci_realize;
    pcidev_k->device_id = 0x1041;
    pcidev_k->revision = VIRTIO_PCI_ABI_VERSION;
    pcidev_k->class_id = PCI_CLASS_OTHERS;
}

static void virtio_memsplit_pci_instance_init(Object *obj)
{
    VirtIOMemsplitPCI *dev = VIRTIO_MEMSPLIT_PCI(obj);

    virtio_instance_init_common(obj, &dev->vdev, sizeof(dev->vdev),
                                TYPE_VIRTIO_MEMSPLIT);
}

static const VirtioPCIDeviceTypeInfo virtio_memsplit_pci_info = {
    .base_name              = TYPE_VIRTIO_MEMSPLIT_PCI,
    .generic_name           = "virtio-memsplit-pci",
    .transitional_name      = "virtio-memsplit-transitional",
    .non_transitional_name  = "virtio-memsplit-non-transitional",
    .instance_size = sizeof(VirtIOMemsplitPCI),
    .instance_init = virtio_memsplit_pci_instance_init,
    .class_init    = virtio_memsplit_pci_class_init,
};

static void virtio_memsplit_pci_register(void)
{
    virtio_pci_types_register(&virtio_memsplit_pci_info);
}

type_init(virtio_memsplit_pci_register)