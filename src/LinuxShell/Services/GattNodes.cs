// SPDX-License-Identifier: GPL-3.0-only
//
// Concrete GATT nodes. Read handlers are injected because two of the
// characteristics answer from static data while the report ones answer from
// whatever was last published, and folding both into one branch would hide which
// is which.

using Tmds.DBus;

namespace IPhoneMirror.LinuxShell.Services;

internal sealed class GattServiceNode(ObjectPath path,
    IDictionary<string, object> properties)
    : GattNode(path, properties), IGattService1
{
    internal override string InterfaceName => "org.bluez.GattService1";
}

internal sealed class GattCharacteristicNode(ObjectPath path,
    IDictionary<string, object> properties)
    : GattNode(path, properties), IGattCharacteristic1
{
    internal override string InterfaceName => "org.bluez.GattCharacteristic1";

    // Set for the HID Control Point, which the host writes to announce suspend.
    internal Action<byte[]>? Written { get; init; }

    internal bool Notifying { get; private set; }

    // Raised when the peer subscribes or unsubscribes. That transition is the
    // only reliable sign iOS has accepted the HID service, so it is surfaced
    // rather than kept private.
    internal Action<bool>? NotifyingChanged { get; init; }

    public Task<byte[]> ReadValueAsync(IDictionary<string, object> options) =>
        Task.FromResult(Properties.TryGetValue("Value", out var value)
            ? (byte[])value
            : []);

    public Task WriteValueAsync(byte[] value, IDictionary<string, object> options)
    {
        Written?.Invoke(value);
        return Task.CompletedTask;
    }

    public Task StartNotifyAsync()
    {
        Notifying = true;
        Publish("Notifying", true);
        NotifyingChanged?.Invoke(true);
        return Task.CompletedTask;
    }

    public Task StopNotifyAsync()
    {
        Notifying = false;
        Publish("Notifying", false);
        NotifyingChanged?.Invoke(false);
        return Task.CompletedTask;
    }
}

internal sealed class GattDescriptorNode(ObjectPath path,
    IDictionary<string, object> properties)
    : GattNode(path, properties), IGattDescriptor1
{
    internal override string InterfaceName => "org.bluez.GattDescriptor1";

    public Task<byte[]> ReadValueAsync(IDictionary<string, object> options) =>
        Task.FromResult(Properties.TryGetValue("Value", out var value)
            ? (byte[])value
            : []);
}
