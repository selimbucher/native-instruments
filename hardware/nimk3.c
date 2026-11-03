/*
 * Maschine MK3 display interface driver for Wine.
 *
 * Stands in for NI's nimc3usb.sys (KMDF, which Wine can't load) on the
 * display interface (MI_05). Attaches to the wineusb interface PDO, registers
 * the device interface NIHostIntegrationAgent looks for and implements the
 * IOCTLs it sends: descriptors, control transfers, bulk writes to the
 * displays and the per-handle event queue.
 *
 * Messages go to DbgPrint (WINEDEBUG=warn+debugstr).
 */

#include <ntddk.h>
#include <usb.h>
#include <usbioctl.h>
#include <usbdlib.h>
#include <initguid.h>

DEFINE_GUID(GUID_DEVINTERFACE_MK3, 0x64352aa7, 0x9dfd, 0x4fa8, 0xb6, 0xf0, 0x47, 0x4a, 0xf2, 0x3a, 0x78, 0xe9);

#define IOCTL_NI_GET_DESCRIPTOR  0x80892004
#define IOCTL_NI_QUEUE_DEPTH     0x80892008
#define IOCTL_NI_QUEUE_ABORT     0x80892010
#define IOCTL_NI_CONTROL         0x80892014
#define IOCTL_NI_WAIT_EVENT      0x80892048
#define IOCTL_NI_GET_VERSION     0x80892804
#define IOCTL_NI_WRITE_BULK      0x80892880
#define IOCTL_NI_GET_KIND        0x80892884

#define KIND_BULK_DISPLAY 3
#define POOL_TAG 0x336b696e /* nik3 */

#define TRACE(fmt, ...) DbgPrint("nimk3: %s: " fmt, __func__, ##__VA_ARGS__)

static LONG bulk_count;

struct mk3
{
    DEVICE_OBJECT *pdo;
    DEVICE_OBJECT *lower;
    UNICODE_STRING iface;
    USB_DEVICE_DESCRIPTOR desc;
    USB_CONFIGURATION_DESCRIPTOR *config;
    USBD_PIPE_HANDLE bulk;
    BOOLEAN started;
    BOOLEAN removed;
    KSPIN_LOCK lock;
};

struct handle
{
    LIST_ENTRY waiting;
    ULONG depth;
};

static NTSTATUS complete(IRP *irp, NTSTATUS status, ULONG_PTR info)
{
    irp->IoStatus.Status = status;
    irp->IoStatus.Information = info;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}

static NTSTATUS usbd_to_nt(USBD_STATUS status)
{
    if (USBD_SUCCESS(status)) return STATUS_SUCCESS;
    switch (status)
    {
    case USBD_STATUS_CANCELED: return STATUS_CANCELLED;
    case USBD_STATUS_TIMEOUT: return STATUS_IO_TIMEOUT;
    case USBD_STATUS_DEVICE_GONE: return STATUS_DEVICE_NOT_CONNECTED;
    default: return STATUS_UNSUCCESSFUL;
    }
}

static NTSTATUS submit_urb_sync(struct mk3 *dev, URB *urb)
{
    IO_STATUS_BLOCK io;
    NTSTATUS status;
    KEVENT event;
    IRP *irp;

    KeInitializeEvent(&event, NotificationEvent, FALSE);
    if (!(irp = IoBuildDeviceIoControlRequest(IOCTL_INTERNAL_USB_SUBMIT_URB, dev->lower,
            NULL, 0, NULL, 0, TRUE, &event, &io)))
        return STATUS_INSUFFICIENT_RESOURCES;
    IoGetNextIrpStackLocation(irp)->Parameters.Others.Argument1 = urb;

    if ((status = IoCallDriver(dev->lower, irp)) == STATUS_PENDING)
    {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = io.Status;
    }
    if (NT_SUCCESS(status)) status = usbd_to_nt(urb->UrbHeader.Status);
    return status;
}

static NTSTATUS get_descriptor_sync(struct mk3 *dev, UCHAR type, void *buffer, ULONG size, ULONG *ret)
{
    NTSTATUS status;
    URB urb;

    RtlZeroMemory(&urb, sizeof(urb));
    UsbBuildGetDescriptorRequest(&urb, sizeof(struct _URB_CONTROL_DESCRIPTOR_REQUEST), type, 0, 0, buffer, NULL, size, NULL);
    status = submit_urb_sync(dev, &urb);
    if (ret) *ret = urb.UrbControlDescriptorRequest.TransferBufferLength;
    return status;
}

/* Selects the vendor display interface (class 0xff, subclass 0xbd), wineusb only
 * hands out its pipe handles after that. */
static NTSTATUS select_display_interface(struct mk3 *dev)
{
    USB_CONFIGURATION_DESCRIPTOR *config = dev->config;
    UCHAR *p = (UCHAR *)config, *end = p + config->wTotalLength;
    USB_INTERFACE_DESCRIPTOR *iface = NULL;
    USB_ENDPOINT_DESCRIPTOR *eps[8];
    struct _URB_SELECT_CONFIGURATION *urb;
    USBD_INTERFACE_INFORMATION *info;
    ULONG count = 0, size, i;
    NTSTATUS status;

    for (; p + 2 <= end && p[0]; p += p[0])
    {
        if (p[1] == USB_INTERFACE_DESCRIPTOR_TYPE)
        {
            USB_INTERFACE_DESCRIPTOR *d = (USB_INTERFACE_DESCRIPTOR *)p;
            if (iface) break;
            if (d->bInterfaceClass == 0xff && d->bInterfaceSubClass == 0xbd && !d->bAlternateSetting)
                iface = d;
        }
        else if (p[1] == USB_ENDPOINT_DESCRIPTOR_TYPE && iface && count < ARRAYSIZE(eps))
            eps[count++] = (USB_ENDPOINT_DESCRIPTOR *)p;
    }
    if (!iface)
    {
        TRACE("no display interface in the configuration descriptor\n");
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    size = sizeof(*urb) + (count ? count - 1 : 0) * sizeof(USBD_PIPE_INFORMATION);
    if (!(urb = ExAllocatePoolWithTag(NonPagedPool, size, POOL_TAG)))
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(urb, size);
    urb->Hdr.Length = size;
    urb->Hdr.Function = URB_FUNCTION_SELECT_CONFIGURATION;
    urb->ConfigurationDescriptor = config;
    info = &urb->Interface;
    info->Length = sizeof(*info) + (count ? count - 1 : 0) * sizeof(USBD_PIPE_INFORMATION);
    info->InterfaceNumber = iface->bInterfaceNumber;
    info->AlternateSetting = iface->bAlternateSetting;
    info->Class = iface->bInterfaceClass;
    info->SubClass = iface->bInterfaceSubClass;
    info->Protocol = iface->bInterfaceProtocol;
    info->NumberOfPipes = count;
    for (i = 0; i < count; i++)
    {
        info->Pipes[i].EndpointAddress = eps[i]->bEndpointAddress;
        info->Pipes[i].PipeType = eps[i]->bmAttributes & USB_ENDPOINT_TYPE_MASK;
        info->Pipes[i].MaximumPacketSize = eps[i]->wMaxPacketSize;
        info->Pipes[i].Interval = eps[i]->bInterval;
        info->Pipes[i].MaximumTransferSize = 0x400000;
    }

    if (!(status = submit_urb_sync(dev, (URB *)urb)))
    {
        for (i = 0; i < count; i++)
        {
            if (info->Pipes[i].PipeType == UsbdPipeTypeBulk && !USB_ENDPOINT_DIRECTION_IN(info->Pipes[i].EndpointAddress))
            {
                dev->bulk = info->Pipes[i].PipeHandle;
                break;
            }
        }
        TRACE("interface %u, %lu endpoints, bulk out %s\n", iface->bInterfaceNumber, count, dev->bulk ? "found" : "missing");
        if (!dev->bulk) status = STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    ExFreePool(urb);
    return status;
}

static NTSTATUS start_device(struct mk3 *dev)
{
    USB_CONFIGURATION_DESCRIPTOR header;
    NTSTATUS status;
    ULONG len;

    if ((status = get_descriptor_sync(dev, USB_DEVICE_DESCRIPTOR_TYPE, &dev->desc, sizeof(dev->desc), &len)))
        return status;
    if ((status = get_descriptor_sync(dev, USB_CONFIGURATION_DESCRIPTOR_TYPE, &header, sizeof(header), &len)))
        return status;
    if (!(dev->config = ExAllocatePoolWithTag(NonPagedPool, header.wTotalLength, POOL_TAG)))
        return STATUS_INSUFFICIENT_RESOURCES;
    if ((status = get_descriptor_sync(dev, USB_CONFIGURATION_DESCRIPTOR_TYPE, dev->config, header.wTotalLength, &len)))
        return status;
    if (len < header.wTotalLength) return STATUS_DEVICE_DATA_ERROR;

    TRACE("%04x:%04x rev %04x\n", dev->desc.idVendor, dev->desc.idProduct, dev->desc.bcdDevice);
    return select_display_interface(dev);
}

/* event queue */

static void WINAPI cancel_wait(DEVICE_OBJECT *device, IRP *irp)
{
    struct mk3 *dev = device->DeviceExtension;
    KIRQL irql;

    IoReleaseCancelSpinLock(irp->CancelIrql);
    KeAcquireSpinLock(&dev->lock, &irql);
    RemoveEntryList(&irp->Tail.Overlay.ListEntry);
    KeReleaseSpinLock(&dev->lock, irql);
    complete(irp, STATUS_CANCELLED, 0);
}

static NTSTATUS queue_wait(struct mk3 *dev, struct handle *handle, IRP *irp)
{
    KIRQL irql;

    KeAcquireSpinLock(&dev->lock, &irql);
    IoSetCancelRoutine(irp, cancel_wait);
    if (irp->Cancel && IoSetCancelRoutine(irp, NULL))
    {
        KeReleaseSpinLock(&dev->lock, irql);
        return complete(irp, STATUS_CANCELLED, 0);
    }
    IoMarkIrpPending(irp);
    InsertTailList(&handle->waiting, &irp->Tail.Overlay.ListEntry);
    KeReleaseSpinLock(&dev->lock, irql);
    return STATUS_PENDING;
}

static void flush_waits(struct mk3 *dev, struct handle *handle, NTSTATUS status)
{
    LIST_ENTRY done, *entry;
    KIRQL irql;
    IRP *irp;

    InitializeListHead(&done);
    KeAcquireSpinLock(&dev->lock, &irql);
    while (!IsListEmpty(&handle->waiting))
    {
        entry = RemoveHeadList(&handle->waiting);
        irp = CONTAINING_RECORD(entry, IRP, Tail.Overlay.ListEntry);
        if (IoSetCancelRoutine(irp, NULL))
            InsertTailList(&done, entry);
        else
            InitializeListHead(entry); /* cancel routine owns it */
    }
    KeReleaseSpinLock(&dev->lock, irql);

    while (!IsListEmpty(&done))
    {
        entry = RemoveHeadList(&done);
        complete(CONTAINING_RECORD(entry, IRP, Tail.Overlay.ListEntry), status, 0);
    }
}

/* User requests finish before the IOCTL returns, with NI's 10 s timeout: the
 * service sends them on an overlapped handle without an OVERLAPPED. Each has its
 * own IRP, URB and buffer, so one that wineusb never completes (it drops
 * transfers libusb refused, as after an unplug) can be left behind. */

union user_urb
{
    URB urb;
    struct _URB_CONTROL_DESCRIPTOR_REQUEST desc;
    struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST ctrl;
    struct _URB_BULK_OR_INTERRUPT_TRANSFER bulk;
};

struct user_xfer
{
    KEVENT event;
    IO_STATUS_BLOCK io;
    union user_urb u;
    UCHAR data[];
};

static struct user_xfer *alloc_user_xfer(ULONG size)
{
    struct user_xfer *xfer;

    if (!(xfer = ExAllocatePoolWithTag(NonPagedPool, sizeof(*xfer) + size, POOL_TAG))) return NULL;
    RtlZeroMemory(xfer, sizeof(*xfer));
    return xfer;
}

/* wineusb has no cancel routine, aborting the pipe cancels the transfers */
static void abort_pipe(struct mk3 *dev)
{
    struct _URB_PIPE_REQUEST urb;

    RtlZeroMemory(&urb, sizeof(urb));
    urb.Hdr.Length = sizeof(urb);
    urb.Hdr.Function = URB_FUNCTION_ABORT_PIPE;
    urb.PipeHandle = dev->bulk;
    submit_urb_sync(dev, (URB *)&urb);
}

/* *left: the transfer never completed and xfer must not be freed */
static NTSTATUS send_user_xfer(struct mk3 *dev, struct user_xfer *xfer, BOOLEAN *left)
{
    LARGE_INTEGER timeout;
    NTSTATUS status;
    IRP *irp;

    *left = FALSE;
    KeInitializeEvent(&xfer->event, NotificationEvent, FALSE);
    if (!(irp = IoBuildDeviceIoControlRequest(IOCTL_INTERNAL_USB_SUBMIT_URB, dev->lower,
            NULL, 0, NULL, 0, TRUE, &xfer->event, &xfer->io)))
        return STATUS_INSUFFICIENT_RESOURCES;
    IoGetNextIrpStackLocation(irp)->Parameters.Others.Argument1 = &xfer->u.urb;

    if ((status = IoCallDriver(dev->lower, irp)) == STATUS_PENDING)
    {
        timeout.QuadPart = -10 * 10000000LL;
        if (KeWaitForSingleObject(&xfer->event, Executive, KernelMode, FALSE, &timeout) == STATUS_TIMEOUT)
        {
            abort_pipe(dev);
            timeout.QuadPart = -10000000LL;
            if (KeWaitForSingleObject(&xfer->event, Executive, KernelMode, FALSE, &timeout) == STATUS_TIMEOUT)
            {
                TRACE("urb %#x never completed, left behind\n", xfer->u.urb.UrbHeader.Function);
                *left = TRUE;
                return STATUS_IO_TIMEOUT;
            }
            status = STATUS_IO_TIMEOUT;
        }
        else status = xfer->io.Status;
    }
    if (NT_SUCCESS(status)) status = usbd_to_nt(xfer->u.urb.UrbHeader.Status);
    if (!NT_SUCCESS(status))
        TRACE("urb %#x failed, status %#lx, usbd %#lx\n", xfer->u.urb.UrbHeader.Function,
              (long)status, (long)xfer->u.urb.UrbHeader.Status);
    return status;
}

/* in: int recipient (0 device, 1 interface, 2 endpoint), u8 type, u8 index, u16 language */
static NTSTATUS ioctl_get_descriptor(struct mk3 *dev, IRP *irp, ULONG in_len, ULONG out_len)
{
    UCHAR *buf = irp->AssociatedIrp.SystemBuffer;
    struct user_xfer *xfer;
    ULONG recipient, len;
    NTSTATUS status;
    BOOLEAN left;
    USHORT lang;
    UCHAR type, index;

    if (in_len < 8) return complete(irp, STATUS_INVALID_PARAMETER, 0);
    RtlCopyMemory(&recipient, buf, 4);
    type = buf[4];
    index = buf[5];
    RtlCopyMemory(&lang, buf + 6, 2);
    TRACE("recipient %lu, type %u, index %u, lang %#x, out %lu\n", recipient, type, index, lang, out_len);

    if (!recipient && type == USB_DEVICE_DESCRIPTOR_TYPE && !index)
    {
        len = min(out_len, sizeof(dev->desc));
        RtlCopyMemory(buf, &dev->desc, len);
        return complete(irp, STATUS_SUCCESS, len);
    }
    if (recipient)
    {
        TRACE("descriptor type %u for recipient %lu not supported\n", type, recipient);
        return complete(irp, STATUS_NOT_SUPPORTED, 0);
    }

    if (!(xfer = alloc_user_xfer(out_len))) return complete(irp, STATUS_INSUFFICIENT_RESOURCES, 0);
    UsbBuildGetDescriptorRequest(&xfer->u.urb, sizeof(xfer->u.desc), type, index, lang, xfer->data, NULL, out_len, NULL);
    status = send_user_xfer(dev, xfer, &left);
    len = NT_SUCCESS(status) ? min(xfer->u.desc.TransferBufferLength, out_len) : 0;
    RtlCopyMemory(buf, xfer->data, len);
    if (!left) ExFreePool(xfer);
    return complete(irp, status, len);
}

/* in: u8 direction (0 out, data follows), int type (1 class, 2 vendor),
 * int recipient (0 device, 1 interface, 2 endpoint, 3 other), u8 request,
 * u16 value, u16 index, u16 length */
static NTSTATUS ioctl_control(struct mk3 *dev, IRP *irp, ULONG in_len, ULONG out_len)
{
    static const USHORT vendor[] = {URB_FUNCTION_VENDOR_DEVICE, URB_FUNCTION_VENDOR_INTERFACE,
                                    URB_FUNCTION_VENDOR_ENDPOINT, URB_FUNCTION_VENDOR_OTHER};
    static const USHORT class[] = {URB_FUNCTION_CLASS_DEVICE, URB_FUNCTION_CLASS_INTERFACE,
                                   URB_FUNCTION_CLASS_ENDPOINT, URB_FUNCTION_CLASS_OTHER};
    UCHAR *buf = irp->AssociatedIrp.SystemBuffer;
    ULONG type, recipient, len;
    USHORT value, index, length;
    struct user_xfer *xfer;
    UCHAR in, request;
    NTSTATUS status;
    BOOLEAN left;

    if (in_len < 16) return complete(irp, STATUS_INVALID_PARAMETER, 0);
    in = buf[0] != 0;
    RtlCopyMemory(&type, buf + 1, 4);
    RtlCopyMemory(&recipient, buf + 5, 4);
    request = buf[9];
    RtlCopyMemory(&value, buf + 10, 2);
    RtlCopyMemory(&index, buf + 12, 2);
    RtlCopyMemory(&length, buf + 14, 2);
    TRACE("%s, type %lu, recipient %lu, request %#x, value %#x, index %#x, length %u\n",
          in ? "in" : "out", type, recipient, request, value, index, length);

    if ((type != 1 && type != 2) || recipient > 3)
        return complete(irp, STATUS_INVALID_PARAMETER, 0);
    len = in ? min(length, out_len) : min(length, in_len - 16);

    if (!(xfer = alloc_user_xfer(len))) return complete(irp, STATUS_INSUFFICIENT_RESOURCES, 0);
    if (!in) RtlCopyMemory(xfer->data, buf + 16, len);
    xfer->u.ctrl.Hdr.Length = sizeof(xfer->u.ctrl);
    xfer->u.ctrl.Hdr.Function = type == 2 ? vendor[recipient] : class[recipient];
    xfer->u.ctrl.TransferFlags = in ? USBD_TRANSFER_DIRECTION_IN : 0;
    xfer->u.ctrl.TransferBuffer = xfer->data;
    xfer->u.ctrl.TransferBufferLength = len;
    xfer->u.ctrl.Request = request;
    xfer->u.ctrl.Value = value;
    xfer->u.ctrl.Index = index;
    status = send_user_xfer(dev, xfer, &left);
    len = NT_SUCCESS(status) && in ? min(xfer->u.ctrl.TransferBufferLength, len) : 0;
    RtlCopyMemory(buf, xfer->data, len);
    if (!left) ExFreePool(xfer);
    return complete(irp, status, len);
}

static NTSTATUS ioctl_write_bulk(struct mk3 *dev, IRP *irp, ULONG in_len)
{
    struct user_xfer *xfer;
    NTSTATUS status;
    BOOLEAN left;

    if (in_len < 4) return complete(irp, STATUS_INVALID_PARAMETER, 0);
    if (!(xfer = alloc_user_xfer(in_len))) return complete(irp, STATUS_INSUFFICIENT_RESOURCES, 0);
    RtlCopyMemory(xfer->data, irp->AssociatedIrp.SystemBuffer, in_len);
    UsbBuildInterruptOrBulkTransferRequest(&xfer->u.urb, sizeof(xfer->u.bulk), dev->bulk,
            xfer->data, NULL, in_len, 0, NULL);
    status = send_user_xfer(dev, xfer, &left);
    if (!left) ExFreePool(xfer);
    return complete(irp, status, 0);
}

static NTSTATUS WINAPI dispatch_ioctl(DEVICE_OBJECT *device, IRP *irp)
{
    IO_STACK_LOCATION *stack = IoGetCurrentIrpStackLocation(irp);
    struct mk3 *dev = device->DeviceExtension;
    struct handle *handle = stack->FileObject ? stack->FileObject->FsContext : NULL;
    ULONG code = stack->Parameters.DeviceIoControl.IoControlCode;
    ULONG in_len = stack->Parameters.DeviceIoControl.InputBufferLength;
    ULONG out_len = stack->Parameters.DeviceIoControl.OutputBufferLength;
    UCHAR *buf = irp->AssociatedIrp.SystemBuffer;

    if (code != IOCTL_NI_WRITE_BULK)
        TRACE("handle %p, code %#lx, in %lu, out %lu\n", handle, code, in_len, out_len);
    else
    {
        LONG count = InterlockedIncrement(&bulk_count);
        if (count <= 5 || !(count % 500)) TRACE("bulk write #%ld, %lu bytes\n", count, in_len);
    }

    if (dev->removed) return complete(irp, STATUS_DELETE_PENDING, 0);
    if (!dev->started) return complete(irp, STATUS_DEVICE_NOT_READY, 0);

    switch (code)
    {
    case IOCTL_NI_GET_KIND:
        if (out_len < 4) return complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
        *(ULONG *)buf = KIND_BULK_DISPLAY;
        return complete(irp, STATUS_SUCCESS, 4);

    case IOCTL_NI_GET_VERSION:
    {
        static const UCHAR version[12] = {2, 0, 5, 0, 1, 0, 0, 0};
        if (out_len < sizeof(version)) return complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
        RtlCopyMemory(buf, version, sizeof(version));
        return complete(irp, STATUS_SUCCESS, sizeof(version));
    }

    case IOCTL_NI_GET_DESCRIPTOR:
        return ioctl_get_descriptor(dev, irp, in_len, out_len);

    case IOCTL_NI_CONTROL:
        return ioctl_control(dev, irp, in_len, out_len);

    case IOCTL_NI_WRITE_BULK:
        return ioctl_write_bulk(dev, irp, in_len);

    case IOCTL_NI_QUEUE_DEPTH:
    {
        ULONG depth;
        if (!handle || in_len < 4) return complete(irp, STATUS_INVALID_PARAMETER, 0);
        RtlCopyMemory(&depth, buf, 4);
        if (depth < 1 || depth > 100) return complete(irp, STATUS_INVALID_PARAMETER, 0);
        handle->depth = depth;
        return complete(irp, STATUS_SUCCESS, 0);
    }

    case IOCTL_NI_QUEUE_ABORT:
        if (!handle) return complete(irp, STATUS_INVALID_PARAMETER, 0);
        flush_waits(dev, handle, STATUS_REQUEST_ABORTED);
        return complete(irp, STATUS_SUCCESS, 0);

    case IOCTL_NI_WAIT_EVENT:
        /* the display interface has no IN pipe, so nothing ever arrives */
        if (!handle) return complete(irp, STATUS_INVALID_PARAMETER, 0);
        return queue_wait(dev, handle, irp);

    default:
        TRACE("unknown ioctl %#lx, in %lu, out %lu\n", code, in_len, out_len);
        return complete(irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }
}

static NTSTATUS WINAPI dispatch_create(DEVICE_OBJECT *device, IRP *irp)
{
    IO_STACK_LOCATION *stack = IoGetCurrentIrpStackLocation(irp);
    struct mk3 *dev = device->DeviceExtension;
    struct handle *handle;

    if (dev->removed) return complete(irp, STATUS_DELETE_PENDING, 0);
    if (!(handle = ExAllocatePoolWithTag(NonPagedPool, sizeof(*handle), POOL_TAG)))
        return complete(irp, STATUS_INSUFFICIENT_RESOURCES, 0);
    InitializeListHead(&handle->waiting);
    handle->depth = 1;
    stack->FileObject->FsContext = handle;
    TRACE("handle %p, name length %u\n", handle, stack->FileObject->FileName.Length);
    return complete(irp, STATUS_SUCCESS, 0);
}

static NTSTATUS WINAPI dispatch_cleanup(DEVICE_OBJECT *device, IRP *irp)
{
    IO_STACK_LOCATION *stack = IoGetCurrentIrpStackLocation(irp);
    struct handle *handle = stack->FileObject->FsContext;

    if (handle) flush_waits(device->DeviceExtension, handle, STATUS_CANCELLED);
    return complete(irp, STATUS_SUCCESS, 0);
}

static NTSTATUS WINAPI dispatch_close(DEVICE_OBJECT *device, IRP *irp)
{
    IO_STACK_LOCATION *stack = IoGetCurrentIrpStackLocation(irp);
    struct handle *handle = stack->FileObject->FsContext;

    if (handle)
    {
        TRACE("handle %p\n", handle);
        flush_waits(device->DeviceExtension, handle, STATUS_CANCELLED);
        ExFreePool(handle);
        stack->FileObject->FsContext = NULL;
    }
    return complete(irp, STATUS_SUCCESS, 0);
}

static NTSTATUS WINAPI forward_done(DEVICE_OBJECT *device, IRP *irp, void *context)
{
    if (irp->PendingReturned) KeSetEvent(context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS forward_sync(struct mk3 *dev, IRP *irp)
{
    NTSTATUS status;
    KEVENT event;

    KeInitializeEvent(&event, NotificationEvent, FALSE);
    IoCopyCurrentIrpStackLocationToNext(irp);
    IoSetCompletionRoutine(irp, forward_done, &event, TRUE, TRUE, TRUE);
    if ((status = IoCallDriver(dev->lower, irp)) == STATUS_PENDING)
    {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = irp->IoStatus.Status;
    }
    return status;
}

static NTSTATUS WINAPI dispatch_pnp(DEVICE_OBJECT *device, IRP *irp)
{
    IO_STACK_LOCATION *stack = IoGetCurrentIrpStackLocation(irp);
    struct mk3 *dev = device->DeviceExtension;
    NTSTATUS status;

    switch (stack->MinorFunction)
    {
    case IRP_MN_START_DEVICE:
        if (NT_SUCCESS(status = forward_sync(dev, irp)) && NT_SUCCESS(status = start_device(dev)))
        {
            dev->started = TRUE;
            IoSetDeviceInterfaceState(&dev->iface, TRUE);
        }
        TRACE("start: %#lx\n", (long)status);
        return complete(irp, status, 0);

    case IRP_MN_SURPRISE_REMOVAL:
        dev->removed = TRUE;
        IoSetDeviceInterfaceState(&dev->iface, FALSE);
        irp->IoStatus.Status = STATUS_SUCCESS;
        break;

    case IRP_MN_REMOVE_DEVICE:
        if (!dev->removed)
        {
            dev->removed = TRUE;
            IoSetDeviceInterfaceState(&dev->iface, FALSE);
        }
        irp->IoStatus.Status = STATUS_SUCCESS;
        IoSkipCurrentIrpStackLocation(irp);
        status = IoCallDriver(dev->lower, irp);
        IoDetachDevice(dev->lower);
        if (dev->config) ExFreePool(dev->config);
        RtlFreeUnicodeString(&dev->iface);
        IoDeleteDevice(device);
        return status;
    }

    IoSkipCurrentIrpStackLocation(irp);
    return IoCallDriver(dev->lower, irp);
}

static NTSTATUS WINAPI add_device(DRIVER_OBJECT *driver, DEVICE_OBJECT *pdo)
{
    DEVICE_OBJECT *device;
    struct mk3 *dev;
    NTSTATUS status;

    TRACE("pdo %p\n", pdo);

    if ((status = IoCreateDevice(driver, sizeof(*dev), NULL, FILE_DEVICE_UNKNOWN,
            FILE_DEVICE_SECURE_OPEN, FALSE, &device)))
        return status;

    dev = device->DeviceExtension;
    RtlZeroMemory(dev, sizeof(*dev));
    dev->pdo = pdo;
    KeInitializeSpinLock(&dev->lock);

    if ((status = IoRegisterDeviceInterface(pdo, &GUID_DEVINTERFACE_MK3, NULL, &dev->iface)))
    {
        TRACE("IoRegisterDeviceInterface failed: %#lx\n", (long)status);
        IoDeleteDevice(device);
        return status;
    }
    if (!(dev->lower = IoAttachDeviceToDeviceStack(device, pdo)))
    {
        RtlFreeUnicodeString(&dev->iface);
        IoDeleteDevice(device);
        return STATUS_NO_SUCH_DEVICE;
    }

    device->Flags |= DO_BUFFERED_IO;
    device->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}

static void WINAPI unload(DRIVER_OBJECT *driver)
{
}

NTSTATUS WINAPI DriverEntry(DRIVER_OBJECT *driver, UNICODE_STRING *path)
{
    driver->DriverExtension->AddDevice = add_device;
    driver->DriverUnload = unload;
    driver->MajorFunction[IRP_MJ_CREATE] = dispatch_create;
    driver->MajorFunction[IRP_MJ_CLEANUP] = dispatch_cleanup;
    driver->MajorFunction[IRP_MJ_CLOSE] = dispatch_close;
    driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = dispatch_ioctl;
    driver->MajorFunction[IRP_MJ_PNP] = dispatch_pnp;
    return STATUS_SUCCESS;
}
