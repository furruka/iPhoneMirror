// SPDX-License-Identifier: GPL-3.0-only

using System.Diagnostics;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Text;
using Avalonia;
using Avalonia.Controls;
using Avalonia.LogicalTree;
using Avalonia.Platform;
using Avalonia.Rendering.Composition;

namespace IPhoneMirror.Linux.AvaloniaSurfaceProbe;

/// <summary>
/// Hosts a libplacebo-rendered Vulkan image inside an Avalonia visual tree by
/// exporting it as an opaque POSIX file descriptor and importing it through
/// <see cref="ICompositionGpuInterop"/>.
/// </summary>
/// <remarks>
/// This is the Linux counterpart of the Windows preview path, where D3D11
/// renders into a shared texture that DirectComposition presents. It exists to
/// answer one question before any production renderer is written: whether the
/// compositor accepts a libplacebo image at all, and what the per-frame cost of
/// the handshake is.
///
/// With an input path the control plays the stream continuously and the frame
/// budget is ignored, which is how the video pipeline is eyeballed; without one
/// it runs the animated gradient for exactly the frame budget and reports
/// PASS/FAIL, which is how the numbers in docs/LINUX_PORT.md stay reproducible.
/// </remarks>
internal sealed class PlaceboSurfaceControl : Control
{
    private readonly int _frameBudget;
    private readonly string? _inputPath;
    private readonly bool _forceSoftwareDecode;
    private readonly bool _loop;
    private readonly bool _verbose;
    private readonly Action _renderNextFrame;
    // One shared builder so every exit path reports the whole handshake, not
    // just the reason it ended.
    private readonly StringBuilder _report = new();
    private readonly List<double> _presentSamples = new();
    private readonly List<double> _producerSamples = new();
    private readonly Stopwatch _sessionClock = new();

    private Compositor? _compositor;
    private ICompositionGpuInterop? _interop;
    private CompositionDrawingSurface? _surface;
    private CompositionSurfaceVisual? _visual;
    private ICompositionImportedGpuImage? _importedImage;
    private ICompositionImportedGpuSemaphore? _renderCompletedSemaphore;
    private ICompositionImportedGpuSemaphore? _availableSemaphore;
    private Task? _pendingPresent;

    private nint _producer;
    private Size _surfaceSize;
    private int _framesPresented;
    private bool _updateQueued;
    private bool _running;
    private bool _reported;

    internal PlaceboSurfaceControl(int frameBudget, string? inputPath,
        bool forceSoftwareDecode, bool loop, bool verbose)
    {
        _frameBudget = frameBudget;
        _inputPath = inputPath;
        _forceSoftwareDecode = forceSoftwareDecode;
        _loop = loop;
        _verbose = verbose;
        _renderNextFrame = RenderNextFrame;
    }

    /// <summary>Completes once the probe has finished or failed.</summary>
    internal TaskCompletionSource<ProbeResult> Completion { get; } = new();

    internal sealed record ProbeResult(bool Success, string Summary);

    protected override void OnAttachedToVisualTree(VisualTreeAttachmentEventArgs e)
    {
        base.OnAttachedToVisualTree(e);
        _ = InitializeAsync();
    }

    protected override void OnDetachedFromLogicalTree(LogicalTreeAttachmentEventArgs e)
    {
        _running = false;
        base.OnDetachedFromLogicalTree(e);
    }

    private async Task InitializeAsync()
    {
        var report = _report;
        try
        {
            var abiMismatch = PlaceboSurface.DescribeAbiMismatch();
            if (abiMismatch is not null)
            {
                Fail(report, $"the managed and native ABI disagree: {abiMismatch}");
                return;
            }

            var selfVisual = ElementComposition.GetElementVisual(this);
            if (selfVisual is null)
            {
                Fail(report, "the control has no composition visual");
                return;
            }

            _compositor = selfVisual.Compositor;
            _interop = await _compositor.TryGetCompositionGpuInterop();
            if (_interop is null)
            {
                Fail(report,
                    "the compositor exposes no GPU interop for this backend");
                return;
            }

            report.AppendLine(CultureInfo.InvariantCulture,
                $"image handle types    : {string.Join(", ", _interop.SupportedImageHandleTypes)}");
            report.AppendLine(CultureInfo.InvariantCulture,
                $"semaphore handle types: {string.Join(", ", _interop.SupportedSemaphoreTypes)}");

            if (!_interop.SupportedImageHandleTypes.Contains(
                    KnownPlatformGraphicsExternalImageHandleTypes.VulkanOpaquePosixFileDescriptor))
            {
                Fail(report,
                    "the compositor does not accept opaque POSIX file descriptors, "
                    + "so a libplacebo image cannot be imported");
                return;
            }

            var synchronization = _interop.GetSynchronizationCapabilities(
                KnownPlatformGraphicsExternalImageHandleTypes.VulkanOpaquePosixFileDescriptor);
            report.AppendLine(CultureInfo.InvariantCulture,
                $"synchronization       : {synchronization}");
            if ((synchronization & CompositionGpuImportedImageSynchronizationCapabilities.Semaphores) == 0)
            {
                Fail(report,
                    "the compositor cannot synchronise imported images with binary "
                    + "semaphores, which is the only mode libplacebo can drive here");
                return;
            }

            var deviceUuid = _interop.DeviceUuid;
            report.AppendLine(CultureInfo.InvariantCulture,
                $"compositor device uuid: {(deviceUuid is null ? "unavailable" : Convert.ToHexString(deviceUuid))}");

            if (!CreateProducer(report, deviceUuid, out var surfaceInfo))
                return;

            _surfaceSize = new Size(surfaceInfo.Width, surfaceInfo.Height);
            _surface = _compositor.CreateDrawingSurface();
            _visual = _compositor.CreateSurfaceVisual();
            _visual.Surface = _surface;
            // The exported image is larger than the window can show; scale the
            // visual instead of presenting a second distortion on top of the
            // letterboxing the producer already does.
            FitVisualToControl(surfaceInfo.Width, surfaceInfo.Height);
            ElementComposition.SetElementChildVisual(this, _visual);

            var imageProperties = new PlatformGraphicsExternalImageProperties
            {
                Format = PlatformGraphicsExternalImageFormat.R8G8B8A8UNorm,
                Width = surfaceInfo.Width,
                Height = surfaceInfo.Height,
                MemorySize = surfaceInfo.MemorySize,
                MemoryOffset = surfaceInfo.MemoryOffset,
            };

            _importedImage = _interop.ImportImage(
                new PlatformHandle(surfaceInfo.ImageFd,
                    KnownPlatformGraphicsExternalImageHandleTypes.VulkanOpaquePosixFileDescriptor),
                imageProperties);
            _renderCompletedSemaphore = _interop.ImportSemaphore(
                new PlatformHandle(surfaceInfo.RenderCompletedSemaphoreFd,
                    KnownPlatformGraphicsExternalSemaphoreHandleTypes.VulkanOpaquePosixFileDescriptor));
            _availableSemaphore = _interop.ImportSemaphore(
                new PlatformHandle(surfaceInfo.AvailableSemaphoreFd,
                    KnownPlatformGraphicsExternalSemaphoreHandleTypes.VulkanOpaquePosixFileDescriptor));

            report.AppendLine("import image          : ok");
            report.AppendLine("import semaphores     : ok");

            _running = true;
            _sessionClock.Restart();
            QueueNextFrame();
            _ = MonitorAsync(report);
        }
        catch (Exception exception)
        {
            Fail(report, $"{exception.GetType().Name}: {exception.Message}");
        }
    }

    private unsafe bool CreateProducer(StringBuilder report, byte[]? deviceUuid,
        out PlaceboSurface.SurfaceInfo surfaceInfo)
    {
        surfaceInfo = default;

        var config = new PlaceboSurface.Config
        {
            Width = 1170,
            Height = 2532,
            ForceSoftwareDecode = _forceSoftwareDecode ? 1 : 0,
            Loop = _loop ? 1 : 0,
            Verbose = _verbose ? 1 : 0,
        };

        if (_inputPath is not null)
        {
            // pms_create only reads the string during that call, but the pointer
            // must be valid until it returns.
            var pathPointer = Marshal.StringToCoTaskMemUTF8(_inputPath);
            try
            {
                config.InputPath = pathPointer;
                return FinishCreateProducer(report, deviceUuid, config,
                    out surfaceInfo);
            }
            finally
            {
                Marshal.FreeCoTaskMem(pathPointer);
            }
        }

        return FinishCreateProducer(report, deviceUuid, config, out surfaceInfo);
    }

    private unsafe bool FinishCreateProducer(StringBuilder report, byte[]? deviceUuid,
        PlaceboSurface.Config config, out PlaceboSurface.SurfaceInfo surfaceInfo)
    {
        surfaceInfo = default;

        // Opaque FD memory cannot be shared across physical devices, so the
        // producer has to be pinned to the device the compositor picked rather
        // than to libplacebo's own default of the discrete GPU.
        if (deviceUuid is { Length: 16 })
        {
            config.HasDeviceUuid = 1;
            for (var index = 0; index < 16; index++)
                config.DeviceUuid[index] = deviceUuid[index];
        }

        _producer = PlaceboSurface.Create(config);
        if (_producer == nint.Zero)
        {
            Fail(report, "the native producer could not be allocated");
            return false;
        }

        var error = PlaceboSurface.LastError(_producer);
        if (!string.IsNullOrEmpty(error))
        {
            Fail(report, $"native producer: {error}");
            return false;
        }

        if (PlaceboSurface.Describe(_producer, out surfaceInfo!) != 0)
        {
            Fail(report,
                $"native producer: {PlaceboSurface.LastError(_producer)}");
            return false;
        }

        string deviceName;
        fixed (byte* name = surfaceInfo.DeviceName)
            deviceName = Marshal.PtrToStringUTF8((nint)name) ?? "unknown";
        string renderNode;
        fixed (byte* node = surfaceInfo.RenderNode)
            renderNode = Marshal.PtrToStringUTF8((nint)node) ?? "unknown";
        string decoderName;
        fixed (byte* decoder = surfaceInfo.DecoderName)
            decoderName = Marshal.PtrToStringUTF8((nint)decoder) ?? "unknown";

        report.AppendLine(CultureInfo.InvariantCulture,
            $"producer device       : {deviceName}");
        report.AppendLine(CultureInfo.InvariantCulture,
            $"render node           : {renderNode}");
        report.AppendLine(CultureInfo.InvariantCulture,
            $"decoder               : {decoderName}");
        if (surfaceInfo.VideoWidth > 0)
        {
            var copyMode = surfaceInfo.ZeroCopy != 0
                ? "zero-copy dmabuf"
                : "host copy";
            report.Append(string.Create(CultureInfo.InvariantCulture,
                $"stream                : {surfaceInfo.VideoWidth}x{surfaceInfo.VideoHeight}, {copyMode}"))
                .AppendLine();
        }
        report.AppendLine(CultureInfo.InvariantCulture,
            $"exported image        : {surfaceInfo.Width}x{surfaceInfo.Height} rgba8, "
            + $"{surfaceInfo.MemorySize} bytes at offset {surfaceInfo.MemoryOffset}");
        return true;
    }

    private void FitVisualToControl(double imageWidth, double imageHeight)
    {
        if (_visual is null)
            return;
        var bounds = Bounds;
        var scale = double.IsNaN(bounds.Width) || bounds.Width <= 0
            ? 1.0
            : Math.Min(bounds.Width / imageWidth, bounds.Height / imageHeight);
        _visual.Size = new Vector(imageWidth * scale, imageHeight * scale);
    }

    protected override void OnSizeChanged(SizeChangedEventArgs e)
    {
        base.OnSizeChanged(e);
        FitVisualToControl(_surfaceSize.Width, _surfaceSize.Height);
    }

    private void QueueNextFrame()
    {
        if (!_running || _updateQueued || _compositor is null)
            return;
        _updateQueued = true;
        _compositor.RequestCompositionUpdate(_renderNextFrame);
    }

    private void RenderNextFrame()
    {
        _updateQueued = false;
        if (!_running || _surface is null || _importedImage is null)
            return;

        // The previous present must land before the producer is allowed to
        // touch the shared image again, otherwise the semaphore wait ordering
        // is undefined.
        if (_pendingPresent is { IsCompleted: false })
        {
            QueueNextFrame();
            return;
        }
        if (_pendingPresent is { IsFaulted: true })
        {
            Fail(_report,
                $"present failed: {_pendingPresent.Exception?.GetBaseException().Message}");
            return;
        }

        var started = _sessionClock.Elapsed.TotalMilliseconds;
        var producerTiming = new PlaceboSurface.FrameTiming();
        var state = PlaceboSurface.RenderFrame(_producer, out producerTiming);
        if (state == PlaceboSurface.FrameState.NotReady)
        {
            // The decoder holds the frame until its presentation time; requeue
            // without counting a frame so the statistics keep meaning presented.
            QueueNextFrame();
            return;
        }
        if (state == PlaceboSurface.FrameState.Error)
        {
            Fail(_report,
                $"native producer: {PlaceboSurface.LastError(_producer)}");
            return;
        }

        if (state == PlaceboSurface.FrameState.EndOfStream)
        {
            // Not an error: the budget path reports the frames it got, and the
            // playback path simply stops presenting.
            if (_inputPath is null)
            {
                Fail(_report, "the gradient producer reported end of stream");
                return;
            }
            Complete(_report, $"end of stream after {_framesPresented} frames");
            return;
        }

        _producerSamples.Add(producerTiming.ReleaseMilliseconds
            + producerTiming.DecodeMilliseconds + producerTiming.MapMilliseconds
            + producerTiming.RenderMilliseconds + producerTiming.HoldMilliseconds);

        _pendingPresent = _surface.UpdateWithSemaphoresAsync(_importedImage,
            _renderCompletedSemaphore!, _availableSemaphore!);
        var startedForSample = started;
        _pendingPresent.ContinueWith(task =>
        {
            lock (_presentSamples)
                _presentSamples.Add(_sessionClock.Elapsed.TotalMilliseconds
                    - startedForSample);
        }, TaskScheduler.Default);

        _framesPresented++;
        if (_inputPath is null && _framesPresented >= _frameBudget)
        {
            _running = false;
            return;
        }
        QueueNextFrame();
    }

    private async Task MonitorAsync(StringBuilder report)
    {
        var deadline = _inputPath is null
            ? DateTime.UtcNow.AddSeconds(30)
            : DateTime.MaxValue;
        while (_running && DateTime.UtcNow < deadline)
            await Task.Delay(50);

        if (_running)
        {
            // Continuous playback never ends on its own; the window was closed
            // and that is a successful run.
            Complete(report, $"playback stopped after {_framesPresented} frames");
            return;
        }

        if (_pendingPresent is not null)
        {
            try
            {
                await _pendingPresent;
            }
            catch (Exception exception)
            {
                Fail(report, $"present failed: {exception.Message}");
                return;
            }
        }

        PlaceboSurface.Finish(_producer);

        if (_framesPresented < _frameBudget)
        {
            Fail(report,
                $"only {_framesPresented} of {_frameBudget} frames were presented "
                + "before the 30 s deadline");
            return;
        }

        Complete(report, null);
    }

    private void Complete(StringBuilder? report, string? note)
    {
        if (_reported)
            return;
        _reported = true;
        _running = false;

        var summary = report is null
            ? new StringBuilder()
            : report;
        try
        {
            PlaceboSurface.Finish(_producer);
        }
        catch (Exception exception)
        {
            summary.AppendLine(CultureInfo.InvariantCulture,
                $"finish failed: {exception.Message}");
        }
        if (note is not null)
            summary.Append(note).AppendLine();
        summary.AppendLine(CultureInfo.InvariantCulture,
            $"frames presented      : {_framesPresented}");
        if (_framesPresented > 0)
        {
            // The producer cost is what the Linux renderer would actually add to
            // the mirroring pipeline. The end-to-end figure also contains the
            // compositor's vsync wait, so the two must not be conflated.
            Summarize(summary, "producer (gpu)", _producerSamples);
            Summarize(summary, "end to end", _presentSamples);
        }
        summary.AppendLine(CultureInfo.InvariantCulture,
            $"wall clock            : {_sessionClock.Elapsed.TotalMilliseconds:F1} ms");

        Cleanup();
        Completion.TrySetResult(new ProbeResult(true, summary.ToString()));
    }

    private static void Summarize(StringBuilder report, string label,
        IReadOnlyCollection<double> values)
    {
        if (values.Count == 0)
            return;
        List<double> samples;
        lock (values)
            samples = values.OrderBy(value => value).ToList();
        var p95 = samples[Math.Min(samples.Count - 1, (int)(samples.Count * 0.95))];
        report.AppendLine(CultureInfo.InvariantCulture,
            $"{label,-22}: mean={samples.Average():F3} ms  p95={p95:F3} ms  "
            + $"max={samples[^1]:F3} ms  n={samples.Count}");
    }

    private void Fail(StringBuilder report, string reason)
    {
        if (_reported)
            return;
        _reported = true;
        _running = false;
        report.AppendLine(CultureInfo.InvariantCulture, $"FAILED: {reason}");
        Cleanup();
        Completion.TrySetResult(new ProbeResult(false, report.ToString()));
    }

    private void Cleanup()
    {
        if (_producer != nint.Zero)
        {
            PlaceboSurface.Destroy(_producer);
            _producer = nint.Zero;
        }
    }
}
