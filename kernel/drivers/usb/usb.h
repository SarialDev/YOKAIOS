#pragma once
#include "types.h"
#include "drivers/usb/xhci.h"

// Nucleul USB: descriptori, enumerare, drivere de clasa. Singurul controller e xHCI (xhci.h)

struct __attribute__((packed)) UsbSetup {
    uint8_t request_type;
    uint8_t request;
    uint16_t value;
    uint16_t index;
    uint16_t length;
};

struct __attribute__((packed)) UsbDeviceDescriptor {
    uint8_t length;
    uint8_t type;
    uint16_t usb_version;
    uint8_t device_class;
    uint8_t device_subclass;
    uint8_t device_protocol;
    uint8_t max_packet_size0;   // la SuperSpeed e exponent (2^n)
    uint16_t vendor;
    uint16_t product;
    uint16_t device_version;
    uint8_t manufacturer_str;
    uint8_t product_str;
    uint8_t serial_str;
    uint8_t num_configurations;
};

struct __attribute__((packed)) UsbConfigDescriptor {
    uint8_t length;
    uint8_t type;
    uint16_t total_length;      // configuratia + toate interfetele/endpoint-urile de dupa ea
    uint8_t num_interfaces;
    uint8_t config_value;
    uint8_t config_str;
    uint8_t attributes;
    uint8_t max_power;
};

struct __attribute__((packed)) UsbInterfaceDescriptor {
    uint8_t length;
    uint8_t type;
    uint8_t number;
    uint8_t alt_setting;
    uint8_t num_endpoints;
    uint8_t iface_class;
    uint8_t iface_subclass;
    uint8_t iface_protocol;
    uint8_t iface_str;
};

struct __attribute__((packed)) UsbEndpointDescriptor {
    uint8_t length;
    uint8_t type;
    uint8_t address;            // bitul 7 = IN
    uint8_t attributes;         // bitii 1:0 = tipul transferului
    uint16_t max_packet;
    uint8_t interval;
};

const uint8_t USB_DIR_IN = 0x80;
const uint8_t USB_TYPE_CLASS = 0x20;
const uint8_t USB_RECIP_INTERFACE = 0x01;

const uint8_t USB_REQ_GET_DESCRIPTOR = 0x06;
const uint8_t USB_REQ_SET_CONFIGURATION = 0x09;

const uint8_t USB_DESC_DEVICE = 0x01;
const uint8_t USB_DESC_CONFIG = 0x02;
const uint8_t USB_DESC_INTERFACE = 0x04;
const uint8_t USB_DESC_ENDPOINT = 0x05;

const uint8_t USB_EP_CONTROL = 0;
const uint8_t USB_EP_ISOCH = 1;
const uint8_t USB_EP_BULK = 2;
const uint8_t USB_EP_INTERRUPT = 3;

// Vitezele, cu valorile implicite din PORTSC (xHCI 7.2.2.1.1)
enum UsbSpeed : uint8_t {
    USB_SPEED_FULL = 1,
    USB_SPEED_LOW = 2,
    USB_SPEED_HIGH = 3,
    USB_SPEED_SUPER = 4,
};

// Rezultatul unui transfer
enum UsbStatus : int {
    USB_OK = 0,             // inclusiv short packet
    USB_STALL = -1,
    USB_ERROR = -2,
    USB_TIMEOUT = -3,
    USB_GONE = -4,          // dispozitivul a fost deconectat
};

const int USB_MAX_ENDPOINTS = 4;
const int USB_MAX_INTERFACES = 4;
const uint16_t USB_CTRL_MAX = 2048;    // date maxime pentru un transfer de control sincron

struct UsbDevice;
struct UsbEndpoint;
struct UsbDriver;

// Apelat cand se termina un transfer pe endpoint (din intrerupere, fara lock-uri luate)
typedef void (*UsbCompleteFn)(UsbEndpoint* ep, int status, uint16_t length);

struct UsbEndpoint {
    UsbDevice* dev;
    uint8_t address;
    uint8_t attributes;
    uint16_t max_packet;
    uint8_t interval;           // bInterval din descriptor
    uint8_t dci;                // Device Context Index: 2 * numar + (IN ? 1 : 0)
    XhciRing ring;
    uint8_t* buf;               // o pagina DMA, alocata la configurare
    uint64_t buf_phys;
    uint16_t buf_len;           // cat cere un transfer (driverul il seteaza, <= 4096)
    volatile bool busy;         // un transfer in curs
    UsbCompleteFn complete;
    void* ctx;                  // al driverului
};

struct UsbDevice {
    XhciController* hc;
    uint8_t slot;
    uint8_t port;               // portul root hub (1..)
    uint8_t speed;              // UsbSpeed
    volatile bool present;
    uint16_t ep0_mps;
    UsbDeviceDescriptor desc;
    uint8_t config_value;
    bool configured;            // SET_CONFIGURATION reusit, numarat in usb_device_count

    // Stare xHCI
    uint8_t* in_ctx;
    uint64_t in_ctx_phys;
    uint8_t* out_ctx;
    uint64_t out_ctx_phys;
    XhciRing ep0;
    uint8_t* ctrl_buf;          // o pagina: [0, 2048) transferuri sincrone, restul transferuri asincrone
    uint64_t ctrl_phys;
    volatile uint64_t ctrl_wait;    // TRB-ul Status asteptat, 0 = nimeni nu asteapta
    volatile int ctrl_status;
    volatile bool ctrl_done;

    UsbEndpoint eps[USB_MAX_ENDPOINTS];
    uint8_t ep_count;

    // Interfetele preluate de drivere
    const UsbDriver* drivers[USB_MAX_INTERFACES];
    void* drvdata[USB_MAX_INTERFACES];
    uint8_t iface_count;
};

// Potrivire pe clasa interfetei; 0xFF = orice
struct UsbDriver {
    const char* name;
    uint8_t iface_class;
    uint8_t iface_subclass;
    uint8_t iface_protocol;
    // Inainte de SET_CONFIGURATION: preia interfata si isi adauga endpoint-urile (usb_add_endpoint).
    // extra = descriptorii de dupa interfata, pana la urmatoarea. nullptr = nu o preia
    void* (*probe)(UsbDevice* dev, const UsbInterfaceDescriptor* iface, const uint8_t* extra, uint16_t extra_len);
    // Dupa SET_CONFIGURATION (din task, poate face transferuri sincrone)
    void (*start)(UsbDevice* dev, void* data);
    // Dispozitivul a disparut; endpoint-urile nu mai primesc nimic
    void (*disconnect)(UsbDevice* dev, void* data);
};

// Inregistreaza driverele de clasa (HID) si driverul PCI xHCI, care enumereaza porturile.
// Tipareste "[OK] usb ..."
void usb_init();
bool usb_register_driver(const UsbDriver* drv);
int usb_device_count();

// Folosite de xhci.cpp: dispozitiv adresat -> descriptori, drivere, SET_CONFIGURATION; deconectare
bool usb_enumerate(UsbDevice* dev);
void usb_disconnect(UsbDevice* dev);

// Transfer de control sincron (din task); data e un buffer kernel oarecare, len <= USB_CTRL_MAX
int usb_control(UsbDevice* dev, uint8_t request_type, uint8_t request, uint16_t value, uint16_t index,
                void* data, uint16_t len);
// Fara asteptare, sigur din intrerupere; doar OUT, len <= 64 (ex. LED-urile tastaturii)
int usb_control_async(UsbDevice* dev, uint8_t request_type, uint8_t request, uint16_t value, uint16_t index,
                      const void* data, uint16_t len);
// Din probe: adauga endpoint-ul (interrupt/bulk) pe care xHCI il configureaza inainte de SET_CONFIGURATION
UsbEndpoint* usb_add_endpoint(UsbDevice* dev, const UsbEndpointDescriptor* desc);
// Porneste un transfer de ep->buf_len octeti; rezultatul vine in ep->complete
bool usb_submit(UsbEndpoint* ep);

const char* usb_speed_name(uint8_t speed);
