// SPDX-License-Identifier: GPL-3.0-only

using System.Runtime.InteropServices;
using System.Text;

namespace IPhoneMirror.Linux.AvaloniaSurfaceProbe;

/// <summary>
/// P/Invoke surface for the native libplacebo producer built by
/// tools/linux-spikes/PlaceboSurfaceShim.c. The struct layouts mirror
/// PlaceboSurfaceShim.h exactly; both sides must be changed together, and
/// <see cref="DescribeAbiMismatch"/> turns a mistake there into a message
/// instead of a corrupted heap.
/// </summary>
internal static unsafe partial class PlaceboSurface
{
    private const string Library = "iPhoneMirror.Linux.PlaceboSurfaceShim";

    internal const int DeviceNameCapacity = 256;
    internal const int RenderNodeCapacity = 64;
    internal const int DecoderNameCapacity = 64;

    /// <summary>Return codes of <see cref="RenderFrame"/>.</summary>
    internal static class FrameState
    {
        internal const int Presented = 0;
        internal const int NotReady = 1;
        internal const int EndOfStream = 2;
        internal const int Error = -1;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct Config
    {
        public fixed byte DeviceUuid[16];
        public int HasDeviceUuid;
        public int Width;
        public int Height;
        // UTF-8, owned by the caller. Only read during pms_create.
        public nint InputPath;
        public int ForceSoftwareDecode;
        public int Loop;
        public int Verbose;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct SurfaceInfo
    {
        public int ImageFd;
        public int RenderCompletedSemaphoreFd;
        public int AvailableSemaphoreFd;
        public ulong MemorySize;
        public ulong MemoryOffset;
        public int Width;
        public int Height;
        public int VideoWidth;
        public int VideoHeight;
        public int ZeroCopy;
        public fixed byte DeviceName[DeviceNameCapacity];
        public fixed byte RenderNode[RenderNodeCapacity];
        public fixed byte DecoderName[DecoderNameCapacity];
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct FrameTiming
    {
        public double ReleaseMilliseconds;
        public double DecodeMilliseconds;
        public double MapMilliseconds;
        public double RenderMilliseconds;
        public double HoldMilliseconds;
        public long FramesRendered;
        public long MapFailures;
        public int EndOfStream;
    }

    [LibraryImport(Library, EntryPoint = "pms_abi_sizes")]
    private static partial void AbiSizes(out uint config, out uint surfaceInfo,
        out uint frameTiming);

    [LibraryImport(Library, EntryPoint = "pms_create")]
    internal static partial nint Create(in Config config);

    [LibraryImport(Library, EntryPoint = "pms_describe")]
    internal static partial int Describe(nint context, out SurfaceInfo info);

    [LibraryImport(Library, EntryPoint = "pms_render_frame")]
    internal static partial int RenderFrame(nint context, out FrameTiming timing);

    [LibraryImport(Library, EntryPoint = "pms_finish")]
    internal static partial void Finish(nint context);

    // Returned as a raw pointer on purpose: the string lives inside the native
    // context, so the generated string marshaller must not try to free it.
    [LibraryImport(Library, EntryPoint = "pms_last_error")]
    private static partial nint LastErrorPointer(nint context);

    [LibraryImport(Library, EntryPoint = "pms_destroy")]
    internal static partial void Destroy(nint context);

    internal static string LastError(nint context) =>
        Marshal.PtrToStringUTF8(LastErrorPointer(context)) ?? string.Empty;

    /// <summary>
    /// Compares the managed struct sizes against the native ones. Returns null
    /// when the two sides agree, otherwise a description of every mismatch.
    /// </summary>
    /// <remarks>
    /// pms_describe writes the whole of pms_surface_info into the caller's
    /// buffer and pms_render_frame does the same for pms_frame_timing, so a
    /// managed struct that is even one field short overwrites unrelated memory
    /// with no diagnostic at all.
    /// </remarks>
    internal static string? DescribeAbiMismatch()
    {
        AbiSizes(out var nativeConfig, out var nativeSurfaceInfo,
            out var nativeFrameTiming);

        var mismatches = new List<string>();
        Compare("pms_config", sizeof(Config), nativeConfig);
        Compare("pms_surface_info", sizeof(SurfaceInfo), nativeSurfaceInfo);
        Compare("pms_frame_timing", sizeof(FrameTiming), nativeFrameTiming);
        return mismatches.Count == 0 ? null : string.Join("; ", mismatches);

        void Compare(string name, int managed, uint native)
        {
            if (managed != native)
                mismatches.Add(
                    $"{name} is {native} bytes native but {managed} managed");
        }
    }

    /// <summary>Reads a NUL-terminated UTF-8 field without running past it.</summary>
    internal static string ReadText(byte* text, int capacity)
    {
        var bytes = new ReadOnlySpan<byte>(text, capacity);
        var terminator = bytes.IndexOf((byte)0);
        return Encoding.UTF8.GetString(terminator < 0 ? bytes : bytes[..terminator]);
    }
}
