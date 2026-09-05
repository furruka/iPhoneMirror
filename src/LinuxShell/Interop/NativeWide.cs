// SPDX-License-Identifier: GPL-3.0-only
//
// wchar_t marshalling for the Linux build.
//
// The port kept wchar_t in the shared C ABI rather than diverging it, and on
// Linux wchar_t is 32 bits. .NET's built-in wide-string marshalling is UTF-16,
// so using it here silently truncates at the first character whose high half is
// zero: an error message came back as "l" instead of "libusb…", and a serial
// passed the same way arrived as one character, which is why the capture start
// could not find the device.

using System.Runtime.InteropServices;
using System.Text;

namespace IPhoneMirror.LinuxShell.Interop;

internal static class NativeWide
{
    // Caller frees with Marshal.FreeHGlobal.
    internal static IntPtr Allocate(string value)
    {
        var bytes = Encoding.UTF32.GetBytes(value);
        var buffer = Marshal.AllocHGlobal(bytes.Length + 4);
        Marshal.Copy(bytes, 0, buffer, bytes.Length);
        Marshal.WriteInt32(buffer, bytes.Length, 0);
        return buffer;
    }

    internal static string Read(IntPtr pointer)
    {
        if (pointer == IntPtr.Zero) return string.Empty;
        var length = 0;
        while (Marshal.ReadInt32(pointer, length) != 0)
        {
            length += 4;
            // A runaway pointer should surface as a short string, not as a hang.
            if (length >= 64 * 1024) break;
        }
        if (length == 0) return string.Empty;
        var bytes = new byte[length];
        Marshal.Copy(pointer, bytes, 0, length);
        return Encoding.UTF32.GetString(bytes);
    }
}
