// SPDX-License-Identifier: GPL-3.0-only
//
// Turns Avalonia pointer and key events into HID reports.
//
// The mouse report is relative, not absolute — the descriptor marks X and Y as
// Data|Variable|Relative (0x81, 0x06) — so this sends deltas and never needs to
// know where the pointer is on the device. Taking an absolute position from the
// window would be wrong: iOS moves its own cursor.
//
// Reports leave on a dedicated thread, coalesced, and never from the UI thread.
// That is not tidiness. Sending inline from PointerMoved froze the window: each
// move published a D-Bus properties-changed signal synchronously, pointer moves
// arrive hundreds of times a second, and the UI thread saturated so the timer
// that presents frames stopped running. The picture stopped while the process sat
// alive at 8% CPU, which reads as a hang rather than as a backlog.
//
// Motion is merged rather than dropped, through the same
// BluetoothMouseReportCoalescer the Windows build uses, so a fast drag does not
// lose travel.

using Avalonia;
using Avalonia.Input;
using IPhoneMirror.App.Services;

namespace IPhoneMirror.LinuxShell.Services;

internal sealed class HidInputBridge : IDisposable
{
    private const int MotionIntervalMs = 8;

    private readonly BluezHidService _hid;
    private readonly Thread _pump;
    private readonly object _sync = new();
    private readonly Queue<(byte Report, byte[] Payload)> _discrete = new();
    private byte[]? _pendingMotion;
    private byte _buttons;
    private Point? _last;
    private volatile bool _running = true;

    internal HidInputBridge(BluezHidService hid)
    {
        _hid = hid;
        _pump = new Thread(Pump)
        {
            IsBackground = true,
            Name = "iPhoneMirror HID pump",
        };
        _pump.Start();
    }

    internal int DisplayRotation { get; set; }
    internal uint DisplayedWidth { get; set; } = 1206;
    internal uint DisplayedHeight { get; set; } = 2622;
    internal long Sent { get; private set; }

    internal void PointerMoved(Point position)
    {
        if (_last is { } previous)
        {
            var (dx, dy) = BluetoothMouseOrientationMapper.Map(
                position.X - previous.X, position.Y - previous.Y,
                DisplayedWidth, DisplayedHeight, DisplayRotation,
                BluetoothMouseDirection.Up, BluetoothMouseDirection.Up,
                reverseHorizontal: false, reverseVertical: false);
            QueueMotion(Clamp(dx), Clamp(dy), 0);
        }
        _last = position;
    }

    internal void PointerLost() => _last = null;

    internal void ButtonChanged(int index, bool pressed)
    {
        if (index is < 0 or > 4) return;
        var mask = (byte)(1 << index);
        lock (_sync)
        {
            _buttons = pressed
                ? (byte)(_buttons | mask)
                : (byte)(_buttons & ~mask);
        }
        // A button change must not be merged into pending motion, or a click
        // could be swallowed by the next move; it goes through the discrete
        // queue, which is always flushed.
        QueueDiscrete(HidReportMap.MouseReportId, BuildMouse(0, 0, 0));
    }

    internal void Wheel(double delta)
    {
        var clicks = (sbyte)Math.Clamp(Math.Round(delta), -127, 127);
        if (clicks != 0)
            QueueDiscrete(HidReportMap.MouseReportId, BuildMouse(0, 0, clicks));
    }

    // Keyboard: HID usage IDs, not scan codes. Only the keys a mirror actually
    // needs are mapped; an unmapped key sends nothing rather than a wrong usage,
    // because wrong is worse than missing.
    internal void KeyChanged(Key key, bool pressed, KeyModifiers modifiers)
    {
        var usage = UsageFor(key);
        if (usage == 0) return;
        var report = new byte[8];
        report[0] = (byte)(
            (modifiers.HasFlag(KeyModifiers.Control) ? 0x01 : 0) |
            (modifiers.HasFlag(KeyModifiers.Shift) ? 0x02 : 0) |
            (modifiers.HasFlag(KeyModifiers.Alt) ? 0x04 : 0) |
            (modifiers.HasFlag(KeyModifiers.Meta) ? 0x08 : 0));
        report[2] = pressed ? usage : (byte)0;
        QueueDiscrete(HidReportMap.KeyboardReportId, report);
    }

    private void Pump()
    {
        while (_running)
        {
            (byte Report, byte[] Payload)? discrete = null;
            byte[]? motion = null;
            lock (_sync)
            {
                if (_discrete.Count > 0) discrete = _discrete.Dequeue();
                else if (_pendingMotion is { } pending)
                {
                    motion = pending;
                    _pendingMotion = null;
                }
            }
            if (discrete is { } item)
            {
                if (_hid.SendReport(item.Report, item.Payload)) ++Sent;
                // Discrete events are drained without waiting so a press and its
                // release cannot end up a frame apart.
                continue;
            }
            if (motion is not null)
            {
                if (_hid.SendReport(HidReportMap.MouseReportId, motion)) ++Sent;
            }
            Thread.Sleep(MotionIntervalMs);
        }
    }

    private void QueueMotion(short dx, short dy, sbyte wheel)
    {
        if (dx == 0 && dy == 0 && wheel == 0) return;
        var incoming = BuildMouse(dx, dy, wheel);
        lock (_sync)
        {
            _pendingMotion = BluetoothMouseReportCoalescer.MergePendingMotion(
                _pendingMotion, incoming);
        }
    }

    private void QueueDiscrete(byte report, byte[] payload)
    {
        lock (_sync)
        {
            // Bounded: a stuck link must not grow the queue without limit. Losing
            // the oldest queued event is better than losing the newest, because
            // the newest is what the user just did.
            if (_discrete.Count >= 256) _discrete.Dequeue();
            _discrete.Enqueue((report, payload));
        }
    }

    private byte[] BuildMouse(short dx, short dy, sbyte wheel)
    {
        var report = new byte[BluetoothMouseReportCoalescer.ReportLength];
        lock (_sync) report[0] = _buttons;
        BitConverter.TryWriteBytes(report.AsSpan(1, 2), dx);
        BitConverter.TryWriteBytes(report.AsSpan(3, 2), dy);
        report[5] = (byte)wheel;
        return report;
    }

    private static short Clamp(double value) =>
        (short)Math.Clamp(Math.Round(value), short.MinValue + 1, short.MaxValue);

    private static byte UsageFor(Key key) => key switch
    {
        >= Key.A and <= Key.Z => (byte)(0x04 + (key - Key.A)),
        >= Key.D1 and <= Key.D9 => (byte)(0x1E + (key - Key.D1)),
        Key.D0 => 0x27,
        Key.Enter => 0x28,
        Key.Escape => 0x29,
        Key.Back => 0x2A,
        Key.Tab => 0x2B,
        Key.Space => 0x2C,
        Key.Right => 0x4F,
        Key.Left => 0x50,
        Key.Down => 0x51,
        Key.Up => 0x52,
        _ => 0,
    };

    public void Dispose()
    {
        _running = false;
        _pump.Join(TimeSpan.FromMilliseconds(200));
    }
}
