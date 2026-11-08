/*
 * Maschine MK3 driver for Wine.
 *
 * Stands in for NI's nimc3usb.sys (KMDF, which Wine can't load) on two
 * interfaces, attached to their wineusb interface PDOs:
 * - display (MI_05): bulk writes to the two displays
 * - audio (MI_00): only the virtual "Ctrl MIDI" port NIHostIntegrationAgent
 *   sends MIDI mode to; the audio itself is ALSA's. nimidi.exe publishes the
 *   port in ALSA (nimidi.h). No USB I/O but descriptor reads.
 * Both register the device interface the agent looks for and implement the
 * IOCTLs it sends: kind, descriptors, control transfers, bulk writes, MIDI and
 * the per-handle event queue.
 *
 * Messages go to DbgPrint (WINEDEBUG=warn+debugstr).
 */

#include <ntddk.h>
#include <usb.h>
#include <usbioctl.h>
#include <usbdlib.h>
#include <initguid.h>
#include "nimidi.h"

DEFINE_GUID(GUID_DEVINTERFACE_MK3, 0x64352aa7, 0x9dfd, 0x4fa8, 0xb6, 0xf0, 0x47, 0x4a, 0xf2, 0x3a, 0x78, 0xe9);

#define IOCTL_NI_GET_DESCRIPTOR  0x80892004
#define IOCTL_NI_QUEUE_DEPTH     0x80892008
#define IOCTL_NI_QUEUE_ABORT     0x80892010
#define IOCTL_NI_CONTROL         0x80892014
#define IOCTL_NI_SEND_MIDI       0x80892040
#define IOCTL_NI_WAIT_EVENT      0x80892048
#define IOCTL_NI_GET_VERSION     0x80892804
#define IOCTL_NI_WRITE_BULK      0x80892880
#define IOCTL_NI_GET_KIND        0x80892884

#define KIND_DEFAULT      0 /* NI's driver reports it for the audio interface */
#define KIND_BULK_DISPLAY 3

/* MIDI on the Ctrl port: NI's software sends {cable, length, bytes} with
 * IOCTL_NI_SEND_MIDI and gets what DAWs send as events with the same payload */
#define CTRL_CABLE 1
#define EVENT_CTRL_MIDI 7
#define EVENT_HEADER 12 /* u64 time, u8 type, u8 0, u16 length */
#define MAX_EVENT_MIDI 255
#define CTRL_PORT_NAME "Maschine MK3 Ctrl MIDI"
#define OUT_SIZE 0x4000 /* MIDI from NI that nimidi.exe hasn't read yet */

#define POOL_TAG 0x336b696e /* nik3 */

#define TRACE(fmt, ...) DbgPrint("nimk3: %s: " fmt, __func__, ##__VA_ARGS__)

static LONG bulk_count;

struct mk3
{
    DEVICE_OBJECT *pdo;
    DEVICE_OBJECT *lower;
    BOOLEAN ctrl;               /* the audio interface, see above */
    UNICODE_STRING iface;
    UNICODE_STRING midi_iface;  /* ctrl only */
    USB_DEVICE_DESCRIPTOR desc;
    USB_CONFIGURATION_DESCRIPTOR *config;
    USBD_PIPE_HANDLE bulk;
    BOOLEAN started;
    BOOLEAN removed;
    KSPIN_LOCK lock;            /* everything below and the handles' lists */
    LIST_ENTRY handles;
    LIST_ENTRY reads;           /* pending IOCTL_NIWINE_MIDI_READ */
    ULONG readers;              /* handles that read MIDI */
    ULONG out_len;
    UCHAR out[OUT_SIZE];
};

struct handle
{
    LIST_ENTRY entry;
    LIST_ENTRY waiting;         /* pending IOCTL_NI_WAIT_EVENT */
    LIST_ENTRY events;          /* struct event */
    ULONG depth, count;
    BOOLEAN reader;             /* nimidi.exe's, gets no events */
};

/* as IOCTL_NI_WAIT_EVENT returns it */
struct event
{
    LIST_ENTRY entry;
    ULONG size;
    UCHAR data[];
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
    TRACE("%04x:%04x rev %04x, %s\n", dev->desc.idVendor, dev->desc.idProduct, dev->desc.bcdDevice,
          dev->ctrl ? "audio interface" : "display interface");
    if (dev->ctrl) return STATUS_SUCCESS;

    if ((status = get_descriptor_sync(dev, USB_CONFIGURATION_DESCRIPTOR_TYPE, &header, sizeof(header), &len)))
        return status;
    if (!(dev->config = ExAllocatePoolWithTag(NonPagedPool, header.wTotalLength, POOL_TAG)))
        return STATUS_INSUFFICIENT_RESOURCES;
    if ((status = get_descriptor_sync(dev, USB_CONFIGURATION_DESCRIPTOR_TYPE, dev->config, header.wTotalLength, &len)))
        return status;
    if (len < header.wTotalLength) return STATUS_DEVICE_DATA_ERROR;
    return select_display_interface(dev);
}

/* pending requests: event waits and MIDI reads */

static void WINAPI cancel_pending(DEVICE_OBJECT *device, IRP *irp)
{
    struct mk3 *dev = device->DeviceExtension;
    KIRQL irql;

    IoReleaseCancelSpinLock(irp->CancelIrql);
    KeAcquireSpinLock(&dev->lock, &irql);
    RemoveEntryList(&irp->Tail.Overlay.ListEntry);
    KeReleaseSpinLock(&dev->lock, irql);
    complete(irp, STATUS_CANCELLED, 0);
}

/* with dev->lock held */
static NTSTATUS pend(LIST_ENTRY *list, IRP *irp)
{
    IoSetCancelRoutine(irp, cancel_pending);
    if (irp->Cancel && IoSetCancelRoutine(irp, NULL))
    {
        irp->IoStatus.Status = STATUS_CANCELLED;
        irp->IoStatus.Information = 0;
        return STATUS_CANCELLED;
    }
    IoMarkIrpPending(irp);
    InsertTailList(list, &irp->Tail.Overlay.ListEntry);
    return STATUS_PENDING;
}

/* With dev->lock held: removes the first request of list, or NULL. Takes
 * only requests that aren't being cancelled. */
static IRP *take(LIST_ENTRY *list)
{
    LIST_ENTRY *entry;
    IRP *irp;

    while (!IsListEmpty(list))
    {
        entry = RemoveHeadList(list);
        irp = CONTAINING_RECORD(entry, IRP, Tail.Overlay.ListEntry);
        if (IoSetCancelRoutine(irp, NULL)) return irp;
        InitializeListHead(entry); /* cancel routine owns it */
    }
    return NULL;
}

static void complete_all(LIST_ENTRY *done)
{
    while (!IsListEmpty(done))
        IoCompleteRequest(CONTAINING_RECORD(RemoveHeadList(done), IRP, Tail.Overlay.ListEntry), IO_NO_INCREMENT);
}

/* the requests of file (all if NULL) in list finish with status */
static void flush(struct mk3 *dev, LIST_ENTRY *list, FILE_OBJECT *file, NTSTATUS status)
{
    LIST_ENTRY keep, done;
    KIRQL irql;
    IRP *irp;

    InitializeListHead(&keep);
    InitializeListHead(&done);
    KeAcquireSpinLock(&dev->lock, &irql);
    while ((irp = take(list)))
    {
        if (file && IoGetCurrentIrpStackLocation(irp)->FileObject != file)
        {
            IoSetCancelRoutine(irp, cancel_pending);
            InsertTailList(&keep, &irp->Tail.Overlay.ListEntry);
            continue;
        }
        irp->IoStatus.Status = status;
        irp->IoStatus.Information = 0;
        InsertTailList(&done, &irp->Tail.Overlay.ListEntry);
    }
    while (!IsListEmpty(&keep)) InsertTailList(list, RemoveHeadList(&keep));
    KeReleaseSpinLock(&dev->lock, irql);
    complete_all(&done);
}

/* event queue, as NI's driver keeps it: an event goes to every request
 * waiting on a handle, or else into the handle's queue up to its depth */

static void fill_event(IRP *irp, const UCHAR *data, ULONG size)
{
    ULONG out_len = IoGetCurrentIrpStackLocation(irp)->Parameters.DeviceIoControl.OutputBufferLength;

    if (out_len < size)
    {
        irp->IoStatus.Status = STATUS_BUFFER_TOO_SMALL;
        irp->IoStatus.Information = 0;
        return;
    }
    RtlCopyMemory(irp->AssociatedIrp.SystemBuffer, data, size);
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = size;
}

static void post_event(struct mk3 *dev, UCHAR type, const UCHAR *payload, USHORT len)
{
    UCHAR data[EVENT_HEADER + 2 + MAX_EVENT_MIDI];
    LARGE_INTEGER time = KeQueryPerformanceCounter(NULL);
    ULONG size = EVENT_HEADER + len;
    LIST_ENTRY done, *entry;
    struct handle *handle;
    struct event *event;
    KIRQL irql;
    IRP *irp;

    RtlCopyMemory(data, &time.QuadPart, 8);
    data[8] = type;
    data[9] = 0;
    RtlCopyMemory(data + 10, &len, 2);
    RtlCopyMemory(data + EVENT_HEADER, payload, len);

    InitializeListHead(&done);
    KeAcquireSpinLock(&dev->lock, &irql);
    for (entry = dev->handles.Flink; entry != &dev->handles; entry = entry->Flink)
    {
        handle = CONTAINING_RECORD(entry, struct handle, entry);
        if (handle->reader) continue;
        if (IsListEmpty(&handle->events) && (irp = take(&handle->waiting)))
        {
            do
            {
                fill_event(irp, data, size);
                InsertTailList(&done, &irp->Tail.Overlay.ListEntry);
            } while ((irp = take(&handle->waiting)));
            continue;
        }
        if (handle->count >= handle->depth) continue;
        if (!(event = ExAllocatePoolWithTag(NonPagedPool, sizeof(*event) + size, POOL_TAG))) continue;
        event->size = size;
        RtlCopyMemory(event->data, data, size);
        InsertTailList(&handle->events, &event->entry);
        handle->count++;
    }
    KeReleaseSpinLock(&dev->lock, irql);
    complete_all(&done);
}

static NTSTATUS wait_event(struct mk3 *dev, struct handle *handle, IRP *irp)
{
    struct event *event;
    NTSTATUS status;
    KIRQL irql;

    KeAcquireSpinLock(&dev->lock, &irql);
    if (IsListEmpty(&handle->events))
    {
        status = pend(&handle->waiting, irp);
        KeReleaseSpinLock(&dev->lock, irql);
        if (status != STATUS_PENDING) IoCompleteRequest(irp, IO_NO_INCREMENT);
        return status;
    }
    event = CONTAINING_RECORD(RemoveHeadList(&handle->events), struct event, entry);
    handle->count--;
    KeReleaseSpinLock(&dev->lock, irql);

    fill_event(irp, event->data, event->size);
    ExFreePool(event);
    status = irp->IoStatus.Status;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}

static void free_events(struct mk3 *dev, struct handle *handle)
{
    LIST_ENTRY events;
    KIRQL irql;

    InitializeListHead(&events);
    KeAcquireSpinLock(&dev->lock, &irql);
    while (!IsListEmpty(&handle->events)) InsertTailList(&events, RemoveHeadList(&handle->events));
    handle->count = 0;
    KeReleaseSpinLock(&dev->lock, irql);
    while (!IsListEmpty(&events)) ExFreePool(CONTAINING_RECORD(RemoveHeadList(&events), struct event, entry));
}

/* Ctrl MIDI port */

/* with dev->lock held: hands the buffered MIDI to pending reads */
static void deliver_out(struct mk3 *dev, LIST_ENTRY *done)
{
    ULONG out_len, len;
    IRP *irp;

    while (dev->out_len && (irp = take(&dev->reads)))
    {
        out_len = IoGetCurrentIrpStackLocation(irp)->Parameters.DeviceIoControl.OutputBufferLength;
        len = min(out_len, dev->out_len);
        RtlCopyMemory(irp->AssociatedIrp.SystemBuffer, dev->out, len);
        RtlMoveMemory(dev->out, dev->out + len, dev->out_len - len);
        dev->out_len -= len;
        irp->IoStatus.Status = STATUS_SUCCESS;
        irp->IoStatus.Information = len;
        InsertTailList(done, &irp->Tail.Overlay.ListEntry);
    }
}

/* in: u8 cable, u8 length, bytes */
static NTSTATUS ioctl_send_midi(struct mk3 *dev, IRP *irp, ULONG in_len)
{
    UCHAR *buf = irp->AssociatedIrp.SystemBuffer;
    LIST_ENTRY done;
    KIRQL irql;

    if (in_len < 2 || buf[0] != CTRL_CABLE || buf[1] > in_len - 2)
        return complete(irp, STATUS_INVALID_PARAMETER, 0);

    InitializeListHead(&done);
    KeAcquireSpinLock(&dev->lock, &irql);
    /* dropped while nobody reads, or when nimidi.exe fell behind */
    if (dev->readers && dev->out_len + buf[1] <= OUT_SIZE)
    {
        RtlCopyMemory(dev->out + dev->out_len, buf + 2, buf[1]);
        dev->out_len += buf[1];
        deliver_out(dev, &done);
    }
    KeReleaseSpinLock(&dev->lock, irql);
    complete_all(&done);
    return complete(irp, STATUS_SUCCESS, 0);
}

static NTSTATUS ioctl_read_midi(struct mk3 *dev, struct handle *handle, IRP *irp)
{
    LIST_ENTRY done;
    NTSTATUS status;
    KIRQL irql;

    if (!IoGetCurrentIrpStackLocation(irp)->Parameters.DeviceIoControl.OutputBufferLength)
        return complete(irp, STATUS_BUFFER_TOO_SMALL, 0);

    InitializeListHead(&done);
    KeAcquireSpinLock(&dev->lock, &irql);
    if (!handle->reader)
    {
        handle->reader = TRUE;
        dev->readers++;
    }
    status = pend(&dev->reads, irp);
    if (status == STATUS_PENDING) deliver_out(dev, &done);
    KeReleaseSpinLock(&dev->lock, irql);
    if (status != STATUS_PENDING) IoCompleteRequest(irp, IO_NO_INCREMENT);
    complete_all(&done);
    return status;
}

static NTSTATUS ioctl_write_midi(struct mk3 *dev, IRP *irp, ULONG in_len)
{
    UCHAR *buf = irp->AssociatedIrp.SystemBuffer;
    UCHAR payload[2 + MAX_EVENT_MIDI];
    ULONG pos, len;

    for (pos = 0; pos < in_len; pos += len)
    {
        len = min(in_len - pos, MAX_EVENT_MIDI);
        payload[0] = CTRL_CABLE;
        payload[1] = len;
        RtlCopyMemory(payload + 2, buf + pos, len);
        post_event(dev, EVENT_CTRL_MIDI, payload, 2 + len);
    }
    return complete(irp, STATUS_SUCCESS, 0);
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

    if (!dev->bulk) return;
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
        *(ULONG *)buf = dev->ctrl ? KIND_DEFAULT : KIND_BULK_DISPLAY;
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
        if (dev->ctrl) break;
        return ioctl_write_bulk(dev, irp, in_len);

    case IOCTL_NI_SEND_MIDI:
        if (!dev->ctrl) break;
        return ioctl_send_midi(dev, irp, in_len);

    case IOCTL_NIWINE_MIDI_NAME:
        if (!dev->ctrl) break;
        if (out_len < sizeof(CTRL_PORT_NAME) - 1) return complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
        RtlCopyMemory(buf, CTRL_PORT_NAME, sizeof(CTRL_PORT_NAME) - 1);
        return complete(irp, STATUS_SUCCESS, sizeof(CTRL_PORT_NAME) - 1);

    case IOCTL_NIWINE_MIDI_READ:
        if (!dev->ctrl || !handle) break;
        return ioctl_read_midi(dev, handle, irp);

    case IOCTL_NIWINE_MIDI_WRITE:
        if (!dev->ctrl) break;
        return ioctl_write_midi(dev, irp, in_len);

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
        flush(dev, &handle->waiting, NULL, STATUS_REQUEST_ABORTED);
        free_events(dev, handle);
        handle->depth = 0;
        return complete(irp, STATUS_SUCCESS, 0);

    case IOCTL_NI_WAIT_EVENT:
        /* the display interface has no IN pipe, nothing arrives there */
        if (!handle) return complete(irp, STATUS_INVALID_PARAMETER, 0);
        return wait_event(dev, handle, irp);
    }

    TRACE("unknown ioctl %#lx, in %lu, out %lu\n", code, in_len, out_len);
    return complete(irp, STATUS_INVALID_DEVICE_REQUEST, 0);
}

static NTSTATUS WINAPI dispatch_create(DEVICE_OBJECT *device, IRP *irp)
{
    IO_STACK_LOCATION *stack = IoGetCurrentIrpStackLocation(irp);
    struct mk3 *dev = device->DeviceExtension;
    struct handle *handle;
    KIRQL irql;

    if (dev->removed) return complete(irp, STATUS_DELETE_PENDING, 0);
    if (!(handle = ExAllocatePoolWithTag(NonPagedPool, sizeof(*handle), POOL_TAG)))
        return complete(irp, STATUS_INSUFFICIENT_RESOURCES, 0);
    RtlZeroMemory(handle, sizeof(*handle));
    InitializeListHead(&handle->waiting);
    InitializeListHead(&handle->events);
    handle->depth = 1;
    KeAcquireSpinLock(&dev->lock, &irql);
    InsertTailList(&dev->handles, &handle->entry);
    KeReleaseSpinLock(&dev->lock, irql);
    stack->FileObject->FsContext = handle;
    TRACE("handle %p, name length %u\n", handle, stack->FileObject->FileName.Length);
    return complete(irp, STATUS_SUCCESS, 0);
}

static void close_handle(struct mk3 *dev, FILE_OBJECT *file)
{
    struct handle *handle = file->FsContext;

    flush(dev, &handle->waiting, NULL, STATUS_CANCELLED);
    flush(dev, &dev->reads, file, STATUS_CANCELLED);
}

static NTSTATUS WINAPI dispatch_cleanup(DEVICE_OBJECT *device, IRP *irp)
{
    IO_STACK_LOCATION *stack = IoGetCurrentIrpStackLocation(irp);

    if (stack->FileObject->FsContext) close_handle(device->DeviceExtension, stack->FileObject);
    return complete(irp, STATUS_SUCCESS, 0);
}

static NTSTATUS WINAPI dispatch_close(DEVICE_OBJECT *device, IRP *irp)
{
    IO_STACK_LOCATION *stack = IoGetCurrentIrpStackLocation(irp);
    struct handle *handle = stack->FileObject->FsContext;
    struct mk3 *dev = device->DeviceExtension;
    KIRQL irql;

    if (handle)
    {
        TRACE("handle %p\n", handle);
        close_handle(dev, stack->FileObject);
        free_events(dev, handle);
        KeAcquireSpinLock(&dev->lock, &irql);
        RemoveEntryList(&handle->entry);
        if (handle->reader && !--dev->readers) dev->out_len = 0;
        KeReleaseSpinLock(&dev->lock, irql);
        ExFreePool(handle);
        stack->FileObject->FsContext = NULL;
    }
    return complete(irp, STATUS_SUCCESS, 0);
}

/* pending requests fail, so that their callers notice */
static void set_removed(struct mk3 *dev)
{
    LIST_ENTRY done, *entry, *list;
    KIRQL irql;
    IRP *irp;

    dev->removed = TRUE;
    IoSetDeviceInterfaceState(&dev->iface, FALSE);
    if (dev->ctrl) IoSetDeviceInterfaceState(&dev->midi_iface, FALSE);

    InitializeListHead(&done);
    KeAcquireSpinLock(&dev->lock, &irql);
    for (entry = &dev->handles, list = &dev->reads; ; list = &CONTAINING_RECORD(entry, struct handle, entry)->waiting)
    {
        while ((irp = take(list)))
        {
            irp->IoStatus.Status = STATUS_DELETE_PENDING;
            irp->IoStatus.Information = 0;
            InsertTailList(&done, &irp->Tail.Overlay.ListEntry);
        }
        if ((entry = entry->Flink) == &dev->handles) break;
    }
    KeReleaseSpinLock(&dev->lock, irql);
    complete_all(&done);
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
            if (dev->ctrl) IoSetDeviceInterfaceState(&dev->midi_iface, TRUE);
        }
        TRACE("start: %#lx\n", (long)status);
        return complete(irp, status, 0);

    case IRP_MN_SURPRISE_REMOVAL:
        set_removed(dev);
        irp->IoStatus.Status = STATUS_SUCCESS;
        break;

    case IRP_MN_REMOVE_DEVICE:
        if (!dev->removed) set_removed(dev);
        irp->IoStatus.Status = STATUS_SUCCESS;
        IoSkipCurrentIrpStackLocation(irp);
        status = IoCallDriver(dev->lower, irp);
        IoDetachDevice(dev->lower);
        if (dev->config) ExFreePool(dev->config);
        RtlFreeUnicodeString(&dev->iface);
        RtlFreeUnicodeString(&dev->midi_iface);
        IoDeleteDevice(device);
        return status;
    }

    IoSkipCurrentIrpStackLocation(irp);
    return IoCallDriver(dev->lower, irp);
}

/* the hardware ids end in the interface number, &MI_xx */
static BOOLEAN is_audio_interface(DEVICE_OBJECT *pdo)
{
    static const WCHAR suffix[] = L"&MI_00";
    const ULONG n = ARRAYSIZE(suffix) - 1;
    WCHAR ids[512];
    ULONG len, i, j, k;

    RtlZeroMemory(ids, sizeof(ids));
    if (IoGetDeviceProperty(pdo, DevicePropertyHardwareID, sizeof(ids) - 2 * sizeof(WCHAR), ids, &len))
        return FALSE;
    for (i = 0; ids[i]; i += j + 1)
    {
        for (j = 0; ids[i + j]; j++);
        if (j < n) continue;
        for (k = 0; k < n && (ids[i + j - n + k] & ~0x20) == (suffix[k] & ~0x20); k++);
        if (k == n) return TRUE;
    }
    return FALSE;
}

static NTSTATUS WINAPI add_device(DRIVER_OBJECT *driver, DEVICE_OBJECT *pdo)
{
    DEVICE_OBJECT *device;
    struct mk3 *dev;
    NTSTATUS status;

    if ((status = IoCreateDevice(driver, sizeof(*dev), NULL, FILE_DEVICE_UNKNOWN,
            FILE_DEVICE_SECURE_OPEN, FALSE, &device)))
        return status;

    dev = device->DeviceExtension;
    RtlZeroMemory(dev, sizeof(*dev));
    dev->pdo = pdo;
    dev->ctrl = is_audio_interface(pdo);
    KeInitializeSpinLock(&dev->lock);
    InitializeListHead(&dev->handles);
    InitializeListHead(&dev->reads);
    TRACE("pdo %p, %s\n", pdo, dev->ctrl ? "audio interface" : "display interface");

    if ((status = IoRegisterDeviceInterface(pdo, &GUID_DEVINTERFACE_MK3, NULL, &dev->iface))
            || (dev->ctrl && (status = IoRegisterDeviceInterface(pdo, &GUID_DEVINTERFACE_NIWINE_MIDI, NULL, &dev->midi_iface))))
    {
        TRACE("IoRegisterDeviceInterface failed: %#lx\n", (long)status);
        RtlFreeUnicodeString(&dev->iface);
        IoDeleteDevice(device);
        return status;
    }
    if (!(dev->lower = IoAttachDeviceToDeviceStack(device, pdo)))
    {
        RtlFreeUnicodeString(&dev->iface);
        RtlFreeUnicodeString(&dev->midi_iface);
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
