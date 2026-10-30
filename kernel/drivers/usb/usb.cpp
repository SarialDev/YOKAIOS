#include "drivers/usb/usb.h"
#include "drivers/usb/hid.h"
#include "mm/kmalloc.h"
#include "lib/kprintf.h"
#include "lib/string.h"

namespace {

const int USB_MAX_DRIVERS = 8;
const uint16_t CONFIG_MAX = 1024;

const UsbDriver* drivers[USB_MAX_DRIVERS];
int driver_total = 0;
volatile int device_total = 0;

bool matches(const UsbDriver* drv, const UsbInterfaceDescriptor* i){
    return (drv->iface_class == 0xFF || drv->iface_class == i->iface_class) &&
           (drv->iface_subclass == 0xFF || drv->iface_subclass == i->iface_subclass) &&
           (drv->iface_protocol == 0xFF || drv->iface_protocol == i->iface_protocol);
}

int get_descriptor(UsbDevice* dev, uint8_t type, uint8_t index, void* buf, uint16_t len){
    return usb_control(dev, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR, (uint16_t)(type << 8 | index), 0, buf, len);
}

// Descriptorii de configurare: pentru fiecare interfata (setarea alternativa 0) cauta un driver
void probe_interfaces(UsbDevice* dev, const uint8_t* cfg, uint16_t total){
    uint16_t off = cfg[0];
    while(off + 2 <= total && dev->iface_count < USB_MAX_INTERFACES){
        uint8_t len = cfg[off];
        if(len < 2 || off + len > total){
            break;
        }
        if(cfg[off + 1] != USB_DESC_INTERFACE || len < sizeof(UsbInterfaceDescriptor)){
            off += len;
            continue;
        }
        const UsbInterfaceDescriptor* iface = (const UsbInterfaceDescriptor*)(cfg + off);
        off += len;
        uint16_t extra = off;
        while(off + 2 <= total && cfg[off] >= 2 && cfg[off + 1] != USB_DESC_INTERFACE){
            off += cfg[off];
        }
        if(off > total){
            off = total;
        }
        if(iface->alt_setting != 0){
            continue;
        }
        kprintf("usb:   interfata %u clasa %02x:%02x:%02x, %u endpoint-uri\n", iface->number, iface->iface_class,
                iface->iface_subclass, iface->iface_protocol, iface->num_endpoints);
        for(int d = 0; d < driver_total; d++){
            if(!matches(drivers[d], iface)){
                continue;
            }
            void* data = drivers[d]->probe(dev, iface, cfg + extra, off - extra);
            if(data){
                dev->drivers[dev->iface_count] = drivers[d];
                dev->drvdata[dev->iface_count] = data;
                dev->iface_count++;
                break;
            }
        }
    }
}

}

const char* usb_speed_name(uint8_t speed){
    switch(speed){
        case USB_SPEED_LOW: return "low";
        case USB_SPEED_FULL: return "full";
        case USB_SPEED_HIGH: return "high";
        case USB_SPEED_SUPER: return "super";
        default: return "?";
    }
}

bool usb_register_driver(const UsbDriver* drv){
    if(!drv || !drv->probe || driver_total >= USB_MAX_DRIVERS){
        return false;
    }
    drivers[driver_total++] = drv;
    return true;
}

int usb_device_count(){
    return device_total;
}

void usb_init(){
    hid_init();
    xhci_init();
    kprintf("[OK] usb %d controller(e), %d dispozitive\n", xhci_controller_count(), usb_device_count());
}

int usb_control(UsbDevice* dev, uint8_t request_type, uint8_t request, uint16_t value, uint16_t index,
                void* data, uint16_t len){
    UsbSetup s = {request_type, request, value, index, len};
    return xhci_control(dev, s, data, true);
}

int usb_control_async(UsbDevice* dev, uint8_t request_type, uint8_t request, uint16_t value, uint16_t index,
                      const void* data, uint16_t len){
    UsbSetup s = {request_type, request, value, index, len};
    return xhci_control(dev, s, (void*)data, false);
}

UsbEndpoint* usb_add_endpoint(UsbDevice* dev, const UsbEndpointDescriptor* desc){
    uint8_t type = desc->attributes & 3;
    if(dev->ep_count >= USB_MAX_ENDPOINTS || (type != USB_EP_INTERRUPT && type != USB_EP_BULK)){
        return nullptr;
    }
    UsbEndpoint& ep = dev->eps[dev->ep_count++];
    memset(&ep, 0, sizeof(ep));
    ep.dev = dev;
    ep.address = desc->address;
    ep.attributes = desc->attributes;
    ep.max_packet = desc->max_packet & 0x7FF;
    ep.interval = desc->interval;
    ep.dci = (desc->address & 0x0F) * 2 + ((desc->address & USB_DIR_IN) ? 1 : 0);
    ep.buf_len = ep.max_packet;
    return &ep;
}

bool usb_submit(UsbEndpoint* ep){
    return xhci_submit(ep);
}

bool usb_enumerate(UsbDevice* dev){
    // Primii 8 octeti dau bMaxPacketSize0; EP0 a pornit cu valoarea implicita a vitezei
    UsbDeviceDescriptor dd;
    if(get_descriptor(dev, USB_DESC_DEVICE, 0, &dd, 8) != USB_OK){
        kprintf("[WARN] usb: portul %u nu raspunde la GET_DESCRIPTOR\n", dev->port);
        return false;
    }
    uint16_t mps = dev->speed == USB_SPEED_SUPER ? (uint16_t)(1u << dd.max_packet_size0) : dd.max_packet_size0;
    if(mps != dev->ep0_mps && !xhci_set_ep0_mps(dev, mps)){
        return false;
    }
    if(get_descriptor(dev, USB_DESC_DEVICE, 0, &dd, sizeof(dd)) != USB_OK){
        return false;
    }
    dev->desc = dd;
    kprintf("usb: port %u slot %u %s-speed %04x:%04x clasa %02x, %u configuratii\n", dev->port, dev->slot,
            usb_speed_name(dev->speed), dd.vendor, dd.product, dd.device_class, dd.num_configurations);

    UsbConfigDescriptor cd;
    if(dd.num_configurations == 0 || get_descriptor(dev, USB_DESC_CONFIG, 0, &cd, sizeof(cd)) != USB_OK){
        return false;
    }
    uint16_t total = cd.total_length < CONFIG_MAX ? cd.total_length : CONFIG_MAX;
    uint8_t* cfg = (uint8_t*)kmalloc(total);
    if(!cfg){
        return false;
    }
    if(total < sizeof(cd) || get_descriptor(dev, USB_DESC_CONFIG, 0, cfg, total) != USB_OK){
        kfree(cfg);
        return false;
    }
    dev->config_value = cd.config_value;
    probe_interfaces(dev, cfg, total);
    kfree(cfg);

    // Spec xHCI 4.3.5: Configure Endpoint inainte de SET_CONFIGURATION
    if(dev->ep_count && !xhci_configure_endpoints(dev)){
        return false;
    }
    if(usb_control(dev, 0, USB_REQ_SET_CONFIGURATION, dev->config_value, 0, nullptr, 0) != USB_OK){
        kprintf("[WARN] usb: SET_CONFIGURATION %u a esuat\n", dev->config_value);
        return false;
    }
    for(int i = 0; i < dev->iface_count; i++){
        if(dev->drivers[i]->start){
            dev->drivers[i]->start(dev, dev->drvdata[i]);
        }
    }
    dev->configured = true;
    device_total++;
    return true;
}

void usb_disconnect(UsbDevice* dev){
    for(int i = 0; i < dev->iface_count; i++){
        if(dev->drivers[i]->disconnect){
            dev->drivers[i]->disconnect(dev, dev->drvdata[i]);
        }
    }
    if(dev->configured){
        dev->configured = false;
        device_total--;
    }
    dev->iface_count = 0;
}
