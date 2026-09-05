// SPDX-License-Identifier: GPL-3.0-only
//
// P/Invoke for the preview C ABI in iPhoneMirror.Core.
//
// The struct mirrors iPhoneMirror::LinuxPreviewSurface field for field, and
// CheckAbi compares sizes before anything is called. That check exists because
// the S3 spike shipped a managed struct 144 bytes smaller than its native
// counterpart and the native side wrote past the end of it without complaint.

using System.Runtime.InteropServices;

namespace IPhoneMirror.LinuxShell.Interop;

[StructLayout(LayoutKind.Sequential)]
internal struct PreviewSurface
{
    public int Valid;
    public int MemoryFd;
    public int RenderCompletedFd;
    public int AvailableFd;
    public uint Width;
    public uint Height;
    public uint VkFormat;
    public uint Reserved;
    public ulong AllocationSize;
    public ulong AllocationOffset;
}

internal static partial class CorePreview
{
    private const string Library = "iPhoneMirror.Core";

    [LibraryImport(Library, EntryPoint = "im_linux_preview_abi_size")]
    internal static partial uint AbiSize();

    // deviceUuid must be the compositor's 16-byte Vulkan device UUID, or null.
    // Not optional on a multi-GPU machine: an image exported from one physical
    // device cannot be imported by a compositor on another, and Avalonia only
    // notices when the first frame is presented.
    [LibraryImport(Library, EntryPoint = "im_linux_preview_open")]
    internal static unsafe partial int Open(uint width, uint height,
        byte* deviceUuid);

    [LibraryImport(Library, EntryPoint = "im_linux_preview_close")]
    internal static partial void Close();

    [LibraryImport(Library, EntryPoint = "im_linux_preview_describe")]
    internal static partial int Describe(out PreviewSurface surface);

    [LibraryImport(Library, EntryPoint = "im_linux_preview_present_nv12")]
    internal static unsafe partial int PresentNv12(byte* data, ulong size,
        uint width, uint height);

    // Throws rather than returning a flag: a layout mismatch has no safe
    // continuation, and the numbers belong in the message.
    internal static void CheckAbi()
    {
        var library = AbiSize();
        var managed = (uint)Marshal.SizeOf<PreviewSurface>();
        if (library != managed)
        {
            throw new InvalidOperationException(
                $"PreviewSurface is {managed} bytes here and {library} bytes in "
                + "iPhoneMirror.Core; the layouts have diverged");
        }
    }
}
