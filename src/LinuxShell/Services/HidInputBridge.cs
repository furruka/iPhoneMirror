// SPDX-License-Identifier: GPL-3.0-only
//
// Turns Avalonia pointer and key events into HID reports.
//
// The mouse report is relative, not absolute — the descriptor marks X and Y as
// Data|Variable|Relative (0x81, 0x06) — so this sends deltas and never needs to
// know where the pointer is on the device. That is also why grabbing an absolute
// position from the window would be wrong: iOS moves its own cursor.
//
// Direction mapping goes through BluetoothMouseOrientationMapper, the same file
// the Windows build uses, so a rotated preview behaves identically on both.

using Avalonia;
using Avalonia.Input;
using IPhoneMirror.App.Services;

namespace IPhoneMirror.LinuxShell.Services;

internal sealed class HidInputBridge(BluezHidService hid)
{
    // Mouse report body: buttons, X and Y as 16-bit signed, wheel.
    private readonly byte[] _mouse = new byte[6];
    // Keyboard report body: modifiers, reserved, six key slots.
    private readonly byte[] _keyboard = new byte[8];
    private Point? _last;
    private byte _buttons;

    internal int DisplayRotation { get; set; }
    internal uint DisplayedWidth { get; set; } = 1206;
    internal uint DisplayedHeight { get; set; } = 2622;

    internal void PointerMoved(Point position)
    {
        if (_last is { } previous)
        {
            var (dx, dy) = BluetoothMouseOrientationMapper.Map(
                position.X - previous.X, position.Y - previous.Y,
                DisplayedWidth, DisplayedHeight, DisplayRotation,
                BluetoothMouseDirection.Up, BluetoothMouseDirection.Up,
                reverseHorizontal: false, reverseVertical: false);
            SendMouse(Clamp(dx), Clamp(dy), 0);
        }
        _last = position;
    }

    internal void PointerLost() => _last = null;

    internal void ButtonChanged(int index, bool pressed)
    {
        if (index is < 0 or > 4) return;
        var mask = (byte)(1 << index);
        _buttons = pressed ? (byte)(_buttons | mask) : (byte)(_buttons & ~mask);
        SendMouse(0, 0, 0);
    }

    internal void Wheel(double delta)
    {
        var clicks = (sbyte)Math.Clamp(Math.Round(delta), -127, 127);
        if (clicks != 0) SendMouse(0, 0, clicks);
    }

    // Keyboard: HID usage IDs, not scan codes. Only the keys a mirror actually
    // needs are mapped; an unmapped key sends nothing rather than sending a wrong
    // usage, because a wrong usage is worse than a missing one.
    internal void KeyChanged(Key key, bool pressed, KeyModifiers modifiers)
    {
        var usage = UsageFor(key);
        if (usage == 0) return;
        _keyboard[0] = (byte)(
            (modifiers.HasFlag(KeyModifiers.Control) ? 0x01 : 0) |
            (modifiers.HasFlag(KeyModifiers.Shift) ? 0x02 : 0) |
            (modifiers.HasFlag(KeyModifiers.Alt) ? 0x04 : 0) |
            (modifiers.HasFlag(KeyModifiers.Meta) ? 0x08 : 0));
        _keyboard[1] = 0;
        // One key slot is enough for typing; the report has six so chording is
        // possible later without changing the descriptor.
        _keyboard[2] = pressed ? usage : (byte)0;
        for (var slot = 3; slot < _keyboard.Length; ++slot) _keyboard[slot] = 0;
        hid.SendReport(HidReportMap.KeyboardReportId, _keyboard);
    }

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

    private static short Clamp(double value) =>
        (short)Math.Clamp(Math.Round(value), short.MinValue, short.MaxValue);

    private void SendMouse(short dx, short dy, sbyte wheel)
    {
        _mouse[0] = _buttons;
        BitConverter.TryWriteBytes(_mouse.AsSpan(1, 2), dx);
        BitConverter.TryWriteBytes(_mouse.AsSpan(3, 2), dy);
        _mouse[5] = (byte)wheel;
        hid.SendReport(HidReportMap.MouseReportId, _mouse);
    }
}
