// SPDX-License-Identifier: GPL-3.0-only
//
// The three kinds of object BlueZ walks in a GATT application.
//
// Tmds.DBus binds an interface name from an attribute at compile time, so one
// class cannot serve a name chosen at runtime; hence three thin types over one
// shared property bag rather than a single generic object. The standard property
// methods on each interface are what the library maps onto
// org.freedesktop.DBus.Properties.

using Tmds.DBus;

namespace IPhoneMirror.LinuxShell.Services;

[DBusInterface("org.bluez.GattService1")]
public interface IGattService1 : IDBusObject
{
    Task<object> GetAsync(string property);
    Task<IDictionary<string, object>> GetAllAsync();
    Task SetAsync(string property, object value);
    Task<IDisposable> WatchPropertiesAsync(Action<PropertyChanges> handler);
}

[DBusInterface("org.bluez.GattCharacteristic1")]
public interface IGattCharacteristic1 : IDBusObject
{
    Task<byte[]> ReadValueAsync(IDictionary<string, object> options);
    Task WriteValueAsync(byte[] value, IDictionary<string, object> options);
    Task StartNotifyAsync();
    Task StopNotifyAsync();
    Task<object> GetAsync(string property);
    Task<IDictionary<string, object>> GetAllAsync();
    Task SetAsync(string property, object value);
    Task<IDisposable> WatchPropertiesAsync(Action<PropertyChanges> handler);
}

[DBusInterface("org.bluez.GattDescriptor1")]
public interface IGattDescriptor1 : IDBusObject
{
    Task<byte[]> ReadValueAsync(IDictionary<string, object> options);
    Task<object> GetAsync(string property);
    Task<IDictionary<string, object>> GetAllAsync();
    Task SetAsync(string property, object value);
    Task<IDisposable> WatchPropertiesAsync(Action<PropertyChanges> handler);
}

internal abstract class GattNode(ObjectPath path,
    IDictionary<string, object> properties) : IDBusObject
{
    protected readonly IDictionary<string, object> Properties = properties;
    private readonly List<Action<PropertyChanges>> _watchers = [];

    public ObjectPath ObjectPath => path;

    internal abstract string InterfaceName { get; }

    internal IDictionary<string, object> Snapshot() => Properties;

    internal void Publish(string name, object value)
    {
        Properties[name] = value;
        var changes = new PropertyChanges([new(name, value)], []);
        foreach (var watcher in _watchers.ToArray()) watcher(changes);
    }

    public Task<object> GetAsync(string property) =>
        Properties.TryGetValue(property, out var value)
            ? Task.FromResult(value)
            : Task.FromException<object>(
                new ArgumentException($"unknown property {property}"));

    public Task<IDictionary<string, object>> GetAllAsync() =>
        Task.FromResult(Properties);

    public Task SetAsync(string property, object value)
    {
        Properties[property] = value;
        return Task.CompletedTask;
    }

    public Task<IDisposable> WatchPropertiesAsync(Action<PropertyChanges> handler)
    {
        _watchers.Add(handler);
        return Task.FromResult<IDisposable>(
            new Subscription(() => _watchers.Remove(handler)));
    }

    private sealed class Subscription(Action release) : IDisposable
    {
        public void Dispose() => release();
    }
}
