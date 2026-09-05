// SPDX-License-Identifier: GPL-3.0-only
//
// The D-Bus interfaces BlueZ expects a GATT application to serve, and the
// advertisement it expects to register.
//
// BlueZ's GATT server model is inverted compared with WinRT's: instead of asking
// a provider to create characteristics, BlueZ asks *us* to export objects and
// then walks them through ObjectManager. So these are the objects we serve, not
// calls we make.

using Tmds.DBus;

namespace IPhoneMirror.LinuxShell.Services;

[DBusInterface("org.bluez.GattManager1")]
public interface IGattManager1 : IDBusObject
{
    Task RegisterApplicationAsync(ObjectPath application,
        IDictionary<string, object> options);
    Task UnregisterApplicationAsync(ObjectPath application);
}

[DBusInterface("org.bluez.LEAdvertisingManager1")]
public interface ILEAdvertisingManager1 : IDBusObject
{
    Task RegisterAdvertisementAsync(ObjectPath advertisement,
        IDictionary<string, object> options);
    Task UnregisterAdvertisementAsync(ObjectPath advertisement);
}

[DBusInterface("org.bluez.LEAdvertisement1")]
public interface ILEAdvertisement1 : IDBusObject
{
    Task ReleaseAsync();
    Task<object> GetAsync(string property);
    Task<IDictionary<string, object>> GetAllAsync();
    Task SetAsync(string property, object value);
    Task<IDisposable> WatchPropertiesAsync(Action<PropertyChanges> handler);
}

[DBusInterface("org.bluez.Adapter1")]
public interface IAdapter1 : IDBusObject
{
    Task<object> GetAsync(string property);
    Task SetAsync(string property, object value);
}

[Dictionary]
internal sealed class GattServiceProperties
{
    public string UUID { get; set; } = string.Empty;
    public bool Primary { get; set; } = true;
}

[Dictionary]
internal sealed class GattCharacteristicProperties
{
    public ObjectPath Service { get; set; }
    public string UUID { get; set; } = string.Empty;
    public string[] Flags { get; set; } = [];
    public byte[] Value { get; set; } = [];
    public bool Notifying { get; set; }
}

[Dictionary]
internal sealed class GattDescriptorProperties
{
    public ObjectPath Characteristic { get; set; }
    public string UUID { get; set; } = string.Empty;
    public string[] Flags { get; set; } = [];
    public byte[] Value { get; set; } = [];
}

[Dictionary]
internal sealed class AdvertisementProperties
{
    public string Type { get; set; } = "peripheral";
    public string[] ServiceUUIDs { get; set; } = [];
    public string LocalName { get; set; } = string.Empty;
    // iOS filters the HID peripherals it offers to pair with by appearance, so
    // 0x03C1 (Keyboard) is load-bearing rather than decorative.
    public ushort Appearance { get; set; }
    public bool Discoverable { get; set; } = true;
}
