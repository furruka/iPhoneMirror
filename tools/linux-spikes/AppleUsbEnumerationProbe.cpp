// SPDX-License-Identifier: GPL-3.0-only
//
// Linux port spike S1: read-only Apple USB enumeration.
//
// Answers the questions that gate the Linux USB capture design:
//   1. Can an unprivileged user open an Apple device through libusb-1.0?
//   2. Does the hidden QuickTime capture configuration exist on iOS/iPadOS 27,
//      and which bConfigurationValue does it use?
//   3. Does usbmuxd hold a kernel driver on the class 0xFE mux interface, and
//      would a configuration change therefore contend with it?
//
// This probe performs NO writes. It never calls libusb_set_configuration,
// libusb_claim_interface, libusb_control_transfer or any bulk transfer, so it
// cannot disturb usbmuxd, an active sync session or the device itself.

#include <libusb.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr std::uint16_t AppleVendorId = 0x05ac;

// Apple exposes the usbmux tunnel as a vendor-specific class 0xFE interface
// with subclass 0x2A, and the private screen-capture interface as class 0xFF
// subclass 0x2A. The capture interface only appears once the hidden
// configuration is selected, so its presence in a descriptor is what tells us
// the device supports QuickTime capture at all.
constexpr std::uint8_t MuxInterfaceClass = 0xFE;
constexpr std::uint8_t QuickTimeInterfaceClass = 0xFF;
constexpr std::uint8_t AppleCaptureSubClass = 0x2A;

const char* class_name(std::uint8_t value) noexcept {
    switch (value) {
    case 0x00: return "per-interface";
    case 0x01: return "audio";
    case 0x02: return "cdc";
    case 0x03: return "hid";
    case 0x06: return "image/ptp";
    case 0x08: return "mass-storage";
    case 0x0e: return "video";
    case 0xfe: return "application/usbmux";
    case 0xff: return "vendor-specific";
    default: return "other";
    }
}

std::string topology_id(libusb_device* device) {
    std::array<std::uint8_t, 8> ports{};
    const int depth = libusb_get_port_numbers(device, ports.data(),
        static_cast<int>(ports.size()));
    std::string result = std::to_string(libusb_get_bus_number(device));
    for (int index = 0; index < depth; ++index)
        result += "." + std::to_string(ports[index]);
    return result;
}

std::string descriptor_string(libusb_device_handle* handle, std::uint8_t index) {
    if (handle == nullptr || index == 0) return {};
    std::array<unsigned char, 256> buffer{};
    const int length = libusb_get_string_descriptor_ascii(handle, index,
        buffer.data(), static_cast<int>(buffer.size()));
    if (length <= 0) return {};
    return std::string(reinterpret_cast<const char*>(buffer.data()),
        static_cast<std::size_t>(length));
}

struct ConfigurationReport {
    std::uint8_t value{};
    bool has_mux_interface{};
    bool has_quicktime_interface{};
};

// Reports whether usbmuxd (or any other kernel driver) currently owns an
// interface. This is the direct evidence for the usbmuxd contention question:
// an attached kernel driver on the mux interface means a configuration change
// will race with usbmuxd's own claim.
void report_kernel_drivers(libusb_device_handle* handle,
    const libusb_config_descriptor& configuration) {
    if (handle == nullptr) {
        std::printf("      kernel driver : unknown (device not open)\n");
        return;
    }
    for (std::uint8_t index = 0; index < configuration.bNumInterfaces; ++index) {
        const int active = libusb_kernel_driver_active(handle, index);
        const char* state = active == 1 ? "ATTACHED"
            : active == 0 ? "free"
            : active == LIBUSB_ERROR_NOT_SUPPORTED ? "unsupported"
            : libusb_error_name(active);
        std::printf("      kernel driver : interface %u -> %s\n", index, state);
    }
}

ConfigurationReport report_configuration(libusb_device* device,
    libusb_device_handle* handle, std::uint8_t index) {
    ConfigurationReport report{};
    libusb_config_descriptor* configuration{};
    const int result = libusb_get_config_descriptor(device, index, &configuration);
    if (result != LIBUSB_SUCCESS) {
        std::printf("    configuration[%u]: cannot read (%s)\n", index,
            libusb_error_name(result));
        return report;
    }

    report.value = configuration->bConfigurationValue;
    std::printf("    configuration[%u]: bConfigurationValue=%u interfaces=%u\n",
        index, configuration->bConfigurationValue, configuration->bNumInterfaces);

    for (std::uint8_t interface_index = 0;
        interface_index < configuration->bNumInterfaces; ++interface_index) {
        const auto& interface = configuration->interface[interface_index];
        for (int alternate = 0; alternate < interface.num_altsetting; ++alternate) {
            const auto& setting = interface.altsetting[alternate];
            const bool is_mux = setting.bInterfaceClass == MuxInterfaceClass &&
                setting.bInterfaceSubClass == AppleCaptureSubClass;
            const bool is_quicktime =
                setting.bInterfaceClass == QuickTimeInterfaceClass &&
                setting.bInterfaceSubClass == AppleCaptureSubClass;
            if (is_mux) report.has_mux_interface = true;
            if (is_quicktime) report.has_quicktime_interface = true;

            std::printf("      interface %u.%u : class=0x%02x (%s) subclass=0x%02x "
                "protocol=0x%02x endpoints=%u%s%s\n",
                setting.bInterfaceNumber, setting.bAlternateSetting,
                setting.bInterfaceClass, class_name(setting.bInterfaceClass),
                setting.bInterfaceSubClass, setting.bInterfaceProtocol,
                setting.bNumEndpoints,
                is_mux ? "  <- usbmux" : "",
                is_quicktime ? "  <- QuickTime capture" : "");

            for (std::uint8_t endpoint = 0; endpoint < setting.bNumEndpoints;
                ++endpoint) {
                const auto& descriptor = setting.endpoint[endpoint];
                const bool is_in = (descriptor.bEndpointAddress & 0x80U) != 0;
                const auto type =
                    static_cast<std::uint8_t>(descriptor.bmAttributes & 0x03U);
                const char* type_name = type == 0 ? "control"
                    : type == 1 ? "isochronous"
                    : type == 2 ? "bulk" : "interrupt";
                std::printf("        endpoint 0x%02x : %-3s %-11s max_packet=%u\n",
                    descriptor.bEndpointAddress, is_in ? "IN" : "OUT", type_name,
                    descriptor.wMaxPacketSize);
            }
        }
    }

    report_kernel_drivers(handle, *configuration);
    libusb_free_config_descriptor(configuration);
    return report;
}

int report_device(libusb_device* device) {
    libusb_device_descriptor descriptor{};
    const int descriptor_result = libusb_get_device_descriptor(device, &descriptor);
    if (descriptor_result != LIBUSB_SUCCESS) {
        std::printf("  cannot read device descriptor (%s)\n",
            libusb_error_name(descriptor_result));
        return 1;
    }

    libusb_device_handle* handle{};
    const int open_result = libusb_open(device, &handle);
    const bool opened = open_result == LIBUSB_SUCCESS;

    std::printf("\nApple device vid=0x%04x pid=0x%04x topology=%s\n",
        descriptor.idVendor, descriptor.idProduct, topology_id(device).c_str());
    std::printf("  usb_version   : %x.%02x  device_class=0x%02x configurations=%u\n",
        descriptor.bcdUSB >> 8, descriptor.bcdUSB & 0xffU,
        descriptor.bDeviceClass, descriptor.bNumConfigurations);

    if (!opened) {
        std::printf("  open          : FAILED (%s)\n", libusb_error_name(open_result));
        std::printf("  hint          : %s\n",
            open_result == LIBUSB_ERROR_ACCESS
                ? "no permission; a udev rule granting the seat/plugdev access is required"
                : "device may be claimed exclusively or disconnected");
    } else {
        std::printf("  open          : ok\n");
        std::printf("  serial (udid) : %s\n",
            descriptor_string(handle, descriptor.iSerialNumber).c_str());
        std::printf("  product       : %s\n",
            descriptor_string(handle, descriptor.iProduct).c_str());
        int active_configuration{};
        const int active_result =
            libusb_get_configuration(handle, &active_configuration);
        if (active_result == LIBUSB_SUCCESS)
            std::printf("  active config : %d\n", active_configuration);
        else
            std::printf("  active config : unknown (%s)\n",
                libusb_error_name(active_result));
    }

    std::vector<ConfigurationReport> reports;
    for (std::uint8_t index = 0; index < descriptor.bNumConfigurations; ++index)
        reports.push_back(report_configuration(device, handle, index));

    std::uint8_t quicktime_configuration{};
    std::uint8_t highest_configuration{};
    bool mux_seen = false;
    for (const auto& report : reports) {
        highest_configuration = std::max(highest_configuration, report.value);
        if (report.has_mux_interface) mux_seen = true;
        if (report.has_quicktime_interface)
            quicktime_configuration = report.value;
    }

    std::printf("  summary       : usbmux_interface=%s quicktime_configuration=%s",
        mux_seen ? "yes" : "no",
        quicktime_configuration != 0
            ? std::to_string(quicktime_configuration).c_str() : "absent");
    if (quicktime_configuration == 0)
        std::printf(" highest_configuration=%u", highest_configuration);
    std::printf("\n");

    if (handle != nullptr) libusb_close(handle);
    return 0;
}

} // namespace

int main() {
    std::printf("Linux port spike S1: read-only Apple USB enumeration\n");
    std::printf("This probe performs no writes and changes no device state.\n");

    libusb_context* context{};
    const int init_result = libusb_init(&context);
    if (init_result != LIBUSB_SUCCESS) {
        std::fprintf(stderr, "libusb_init failed: %s\n",
            libusb_error_name(init_result));
        return 2;
    }

    const libusb_version* version = libusb_get_version();
    if (version != nullptr)
        std::printf("libusb        : %u.%u.%u%s\n", version->major, version->minor,
            version->micro, version->rc != nullptr ? version->rc : "");

    libusb_device** devices{};
    const auto count = libusb_get_device_list(context, &devices);
    if (count < 0) {
        std::fprintf(stderr, "libusb_get_device_list failed: %s\n",
            libusb_error_name(static_cast<int>(count)));
        libusb_exit(context);
        return 2;
    }

    int apple_devices = 0;
    for (ssize_t index = 0; index < count; ++index) {
        libusb_device_descriptor descriptor{};
        if (libusb_get_device_descriptor(devices[index], &descriptor) !=
            LIBUSB_SUCCESS)
            continue;
        if (descriptor.idVendor != AppleVendorId) continue;
        ++apple_devices;
        report_device(devices[index]);
    }

    libusb_free_device_list(devices, 1);
    libusb_exit(context);

    std::printf("\nApple devices found: %d\n", apple_devices);
    if (apple_devices == 0) {
        std::printf("Connect an unlocked, trusted iPhone or iPad and run again.\n");
        return 1;
    }
    return 0;
}
