// SPDX-License-Identifier: GPL-3.0-only
//
// Reverse control on Linux: publishes the desktop as a Bluetooth LE HID
// peripheral through BlueZ so iOS accepts it as a keyboard and mouse.
//
// This is the Linux counterpart of BluetoothHidMouseService's WinRT GATT server.
// The report descriptor and the report layout are shared with it (see
// HidReportMap); what differs is only the plumbing, because BlueZ inverts the
// model: we export D-Bus objects and BlueZ walks them, rather than asking a
// provider to create characteristics for us.
//
// Feasibility was measured before any of this was written: BlueZ 5.87 accepts a
// 0x1812 GATT application and a peripheral advertisement with a HID appearance.
// See docs/LINUX_PORT.md.

using Tmds.DBus;

namespace IPhoneMirror.LinuxShell.Services;

[DBusInterface("org.freedesktop.DBus.ObjectManager")]
public interface IObjectManager : IDBusObject
{
    Task<IDictionary<ObjectPath, IDictionary<string, IDictionary<string, object>>>>
        GetManagedObjectsAsync();
}

internal sealed class BluezHidService : IObjectManager, IAsyncDisposable
{
    private const string Bluez = "org.bluez";
    private const string Base = "0000-1000-8000-00805f9b34fb";
    private const string HidServiceUuid = $"00001812-{Base}";
    private const string ReportMapUuid = $"00002a4b-{Base}";
    private const string HidInformationUuid = $"00002a4a-{Base}";
    private const string HidControlPointUuid = $"00002a4c-{Base}";
    private const string ReportUuid = $"00002a4d-{Base}";
    private const string ReportReferenceUuid = $"00002908-{Base}";

    private static readonly ObjectPath Root = new("/com/iphonemirror/hid");

    private readonly Connection _connection = new(Address.System);
    private readonly List<GattNode> _objects = [];
    private readonly Dictionary<byte, GattCharacteristicNode> _reports = [];
    private readonly Advertisement _advertisement = new();
    private ObjectPath _adapter = new("/org/bluez/hci0");
    private bool _registered;

    public ObjectPath ObjectPath => Root;

    internal string Diagnostic { get; private set; } = "not started";

    internal async Task<bool> StartAsync(string localName = "iPhoneMirror")
    {
        try
        {
            await _connection.ConnectAsync();
            BuildObjects();

            await _connection.RegisterObjectAsync(this);
            foreach (var entry in _objects)
                await _connection.RegisterObjectAsync(entry);

            var manager = _connection.CreateProxy<IGattManager1>(Bluez, _adapter);
            await manager.RegisterApplicationAsync(Root,
                new Dictionary<string, object>());

            _advertisement.LocalName = localName;
            await _connection.RegisterObjectAsync(_advertisement);
            var advertising =
                _connection.CreateProxy<ILEAdvertisingManager1>(Bluez, _adapter);
            await advertising.RegisterAdvertisementAsync(_advertisement.ObjectPath,
                new Dictionary<string, object>());

            _registered = true;
            Diagnostic = $"advertising as \"{localName}\" on {_adapter}";
            return true;
        }
        catch (Exception error)
        {
            Diagnostic = $"{error.GetType().Name}: {error.Message}";
            return false;
        }
    }

    // Sends one input report. Returns false when the peer has not subscribed,
    // which is the normal state before pairing rather than a failure.
    internal bool SendReport(byte reportId, ReadOnlySpan<byte> payload)
    {
        if (!_registered || !_reports.TryGetValue(reportId, out var report))
            return false;
        if (!report.Notifying) return false;
        // Publishing the Value is how a notification is sent: BlueZ forwards the
        // properties-changed signal it subscribed to when the peer enabled them.
        report.Publish("Value", payload.ToArray());
        return true;
    }

    public Task<IDictionary<ObjectPath,
        IDictionary<string, IDictionary<string, object>>>> GetManagedObjectsAsync()
    {
        var managed =
            new Dictionary<ObjectPath,
                IDictionary<string, IDictionary<string, object>>>();
        foreach (var entry in _objects)
        {
            managed[entry.ObjectPath] =
                new Dictionary<string, IDictionary<string, object>>
                {
                    [entry.InterfaceName] = entry.Snapshot(),
                };
        }
        return Task.FromResult<IDictionary<ObjectPath,
            IDictionary<string, IDictionary<string, object>>>>(managed);
    }

    public async ValueTask DisposeAsync()
    {
        if (_registered)
        {
            try
            {
                var advertising = _connection.CreateProxy<ILEAdvertisingManager1>(
                    Bluez, _adapter);
                await advertising.UnregisterAdvertisementAsync(
                    _advertisement.ObjectPath);
                var manager = _connection.CreateProxy<IGattManager1>(Bluez,
                    _adapter);
                await manager.UnregisterApplicationAsync(Root);
            }
            catch (Exception error)
            {
                // Teardown failures are reported, not thrown: the process is
                // exiting and BlueZ drops the registration when the name goes.
                Diagnostic = $"teardown: {error.Message}";
            }
            _registered = false;
        }
        _connection.Dispose();
    }

    private void BuildObjects()
    {
        var service = Root + "/service0";
        _objects.Add(new GattServiceNode(service, new Dictionary<string, object>
        {
            ["UUID"] = HidServiceUuid,
            ["Primary"] = true,
        }));

        AddCharacteristic(service, 0, ReportMapUuid, ["read"],
            HidReportMap.Descriptor);
        AddCharacteristic(service, 1, HidInformationUuid, ["read"],
            HidReportMap.HidInformation);
        // Write-without-response: the host uses it to announce suspend/exit
        // suspend, and HOGP forbids a response.
        AddCharacteristic(service, 2, HidControlPointUuid,
            ["write-without-response"], []);

        // One Report characteristic per input report in the descriptor, each with
        // the Report Reference descriptor that tells iOS which report it is. The
        // feature report (id 3) is deliberately absent for now: nothing sends it
        // yet, and publishing a characteristic we never write would be a claim we
        // do not honour.
        byte index = 3;
        foreach (var reportId in new[]
        {
            HidReportMap.KeyboardReportId,
            HidReportMap.MouseReportId,
            HidReportMap.ConsumerReportId,
            HidReportMap.NavigationReportId,
        })
        {
            var characteristic = AddCharacteristic(service, index, ReportUuid,
                ["read", "notify"], []);
            _reports[reportId] = characteristic;
            _objects.Add(new GattDescriptorNode(
                characteristic.ObjectPath + "/desc0",
                new Dictionary<string, object>
                {
                    ["Characteristic"] = characteristic.ObjectPath,
                    ["UUID"] = ReportReferenceUuid,
                    ["Flags"] = new[] { "read" },
                    // {report id, 0x01 = Input}
                    ["Value"] = new byte[] { reportId, 0x01 },
                }));
            ++index;
        }
    }

    private GattCharacteristicNode AddCharacteristic(ObjectPath service,
        byte index, string uuid, string[] flags, byte[] value)
    {
        var node = new GattCharacteristicNode(service + $"/char{index}",
            new Dictionary<string, object>
            {
                ["Service"] = service,
                ["UUID"] = uuid,
                ["Flags"] = flags,
                ["Value"] = value,
                ["Notifying"] = false,
            });
        _objects.Add(node);
        return node;
    }

    private sealed class Advertisement : ILEAdvertisement1
    {
        private readonly Dictionary<string, object> _properties = new()
        {
            ["Type"] = "peripheral",
            ["ServiceUUIDs"] = new[] { "1812" },
            ["LocalName"] = "iPhoneMirror",
            // 0x03C1 = Keyboard. iOS filters offered HID peripherals by this.
            ["Appearance"] = (ushort)0x03C1,
            ["Discoverable"] = true,
        };

        internal string LocalName
        {
            get => (string)_properties["LocalName"];
            set => _properties["LocalName"] = value;
        }

        public ObjectPath ObjectPath { get; } = new("/com/iphonemirror/hid/adv0");

        // BlueZ calls this when it drops the advertisement, for example after the
        // adapter is powered off. Nothing to undo: the objects stay valid and a
        // later StartAsync re-registers them.
        public Task ReleaseAsync() => Task.CompletedTask;

        public Task<object> GetAsync(string property) =>
            Task.FromResult(_properties[property]);

        public Task<IDictionary<string, object>> GetAllAsync() =>
            Task.FromResult<IDictionary<string, object>>(_properties);

        public Task SetAsync(string property, object value)
        {
            _properties[property] = value;
            return Task.CompletedTask;
        }

        public Task<IDisposable> WatchPropertiesAsync(
            Action<PropertyChanges> handler) =>
            Task.FromResult<IDisposable>(new Empty());

        private sealed class Empty : IDisposable
        {
            public void Dispose() { }
        }
    }
}
