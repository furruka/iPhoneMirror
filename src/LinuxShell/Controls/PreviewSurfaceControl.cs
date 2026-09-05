// SPDX-License-Identifier: GPL-3.0-only
//
// The preview control: imports the image Core exports and shows it.
//
// This is the Linux counterpart of attaching a D3D11 preview to an HWND. The
// window never draws the frame itself; it binds the exported Vulkan image and
// lets the compositor sample it, with a semaphore in each direction so it never
// samples a half-drawn one.

using System.Globalization;
using System.Text;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Platform;
using Avalonia.Rendering.Composition;
using IPhoneMirror.LinuxShell.Interop;

namespace IPhoneMirror.LinuxShell.Controls;

internal sealed class PreviewSurfaceControl : Control
{
    private Compositor? _compositor;
    private ICompositionGpuInterop? _interop;
    private CompositionDrawingSurface? _surface;
    private CompositionSurfaceVisual? _visual;
    private ICompositionImportedGpuImage? _importedImage;
    private ICompositionImportedGpuSemaphore? _renderCompleted;
    private ICompositionImportedGpuSemaphore? _available;
    private PreviewSurface _described;
    private bool _ready;

    internal string Diagnostic { get; private set; } = "not attached yet";
    internal event EventHandler? Ready;

    protected override void OnAttachedToVisualTree(VisualTreeAttachmentEventArgs e)
    {
        base.OnAttachedToVisualTree(e);
        _ = InitializeAsync();
    }

    protected override void OnDetachedFromVisualTree(VisualTreeAttachmentEventArgs e)
    {
        base.OnDetachedFromVisualTree(e);
        Teardown();
    }

    private async Task InitializeAsync()
    {
        var report = new StringBuilder();
        try
        {
            CorePreview.CheckAbi();
            report.Append("abi size: match").AppendLine();

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
                Fail(report, "this backend exposes no GPU interop");
                return;
            }

            const string ImageType =
                KnownPlatformGraphicsExternalImageHandleTypes
                    .VulkanOpaquePosixFileDescriptor;
            if (!_interop.SupportedImageHandleTypes.Contains(ImageType))
            {
                Fail(report, "the compositor will not accept an opaque POSIX "
                    + "file descriptor, so an exported Vulkan image cannot be bound");
                return;
            }
            var synchronization = _interop.GetSynchronizationCapabilities(ImageType);
            if ((synchronization &
                CompositionGpuImportedImageSynchronizationCapabilities.Semaphores) == 0)
            {
                Fail(report, "the compositor cannot synchronise an imported image "
                    + "with binary semaphores, which is the only mode Core drives");
                return;
            }

            if (!OpenPreview(report, _interop.DeviceUuid)) return;

            _surface = _compositor.CreateDrawingSurface();
            _visual = _compositor.CreateSurfaceVisual();
            _visual.Surface = _surface;
            FitVisual();
            ElementComposition.SetElementChildVisual(this, _visual);

            var properties = new PlatformGraphicsExternalImageProperties
            {
                Format = PlatformGraphicsExternalImageFormat.R8G8B8A8UNorm,
                Width = (int)_described.Width,
                Height = (int)_described.Height,
                MemorySize = _described.AllocationSize,
                MemoryOffset = _described.AllocationOffset,
            };
            _importedImage = _interop.ImportImage(
                new PlatformHandle(_described.MemoryFd, ImageType), properties);
            _renderCompleted = _interop.ImportSemaphore(new PlatformHandle(
                _described.RenderCompletedFd,
                KnownPlatformGraphicsExternalSemaphoreHandleTypes
                    .VulkanOpaquePosixFileDescriptor));
            _available = _interop.ImportSemaphore(new PlatformHandle(
                _described.AvailableFd,
                KnownPlatformGraphicsExternalSemaphoreHandleTypes
                    .VulkanOpaquePosixFileDescriptor));
            report.Append("import image and semaphores: ok").AppendLine();

            _ready = true;
            Diagnostic = report.ToString();
            Ready?.Invoke(this, EventArgs.Empty);
        }
        catch (Exception exception)
        {
            Fail(report, $"{exception.GetType().Name}: {exception.Message}");
        }
    }

    private unsafe bool OpenPreview(StringBuilder report, byte[]? deviceUuid)
    {
        report.Append(deviceUuid is null
            ? "compositor device uuid: unavailable"
            : $"compositor device uuid: {Convert.ToHexString(deviceUuid)}")
            .AppendLine();
        int status;
        if (deviceUuid is { Length: 16 })
        {
            fixed (byte* uuid = deviceUuid)
                status = CorePreview.Open(TargetWidth, TargetHeight, uuid);
        }
        else
        {
            status = CorePreview.Open(TargetWidth, TargetHeight, null);
        }
        if (status != 0)
        {
            Fail(report, $"im_linux_preview_open returned {status}");
            return false;
        }
        status = CorePreview.Describe(out _described);
        if (status != 0)
        {
            Fail(report, $"im_linux_preview_describe returned {status}");
            return false;
        }
        if (_described.Valid == 0)
        {
            Fail(report, "Core rendered but could not export the image on this "
                + "platform, so there is nothing to import");
            return false;
        }
        report.Append(string.Create(CultureInfo.InvariantCulture,
            $"surface: {_described.Width}x{_described.Height} vk_format="
            + $"{_described.VkFormat} size={_described.AllocationSize}"))
            .AppendLine();
        return true;
    }

    // Scales the visual instead of stretching the image: Core already
    // letterboxed into the target, so a second fit here would distort twice.
    private void FitVisual()
    {
        if (_visual is null || _described.Width == 0 || _described.Height == 0)
            return;
        var available = Bounds.Size;
        if (available.Width <= 0 || available.Height <= 0)
            available = new Size(_described.Width, _described.Height);
        var scale = Math.Min(available.Width / _described.Width,
            available.Height / _described.Height);
        if (scale <= 0 || double.IsNaN(scale)) scale = 1.0;
        _visual.Size = new Vector(_described.Width, _described.Height);
        _visual.Scale = new Vector3D(scale, scale, 1);
        _visual.Offset = new Vector3D(
            (available.Width - _described.Width * scale) / 2,
            (available.Height - _described.Height * scale) / 2, 0);
    }

    protected override Size ArrangeOverride(Size finalSize)
    {
        var result = base.ArrangeOverride(finalSize);
        FitVisual();
        return result;
    }

    // Renders one NV12 frame through Core and hands the result to the compositor.
    internal async Task<bool> PresentAsync(byte[] frame, uint width, uint height)
    {
        if (!_ready || _surface is null || _importedImage is null ||
            _renderCompleted is null || _available is null) {
            return false;
        }
        int status;
        unsafe
        {
            fixed (byte* data = frame)
                status = CorePreview.PresentNv12(data, (ulong)frame.LongLength,
                    width, height);
        }
        if (status != 0)
        {
            Diagnostic = $"im_linux_preview_present_nv12 returned {status}";
            return false;
        }
        await _surface.UpdateWithSemaphoresAsync(_importedImage, _renderCompleted,
            _available);
        return true;
    }

    private void Fail(StringBuilder report, string reason)
    {
        report.Append(reason).AppendLine();
        Diagnostic = report.ToString();
        Teardown();
    }

    private void Teardown()
    {
        _ready = false;
        _importedImage?.DisposeAsync();
        _renderCompleted?.DisposeAsync();
        _available?.DisposeAsync();
        _importedImage = null;
        _renderCompleted = null;
        _available = null;
        CorePreview.Close();
    }

    // Defaults to the highest encoder tier verified on both test devices; the
    // shell overrides it to match the source when it is playing a file, which
    // keeps the acceptance run free of an unrelated letterbox.
    internal uint TargetWidth { get; set; } = 1206;
    internal uint TargetHeight { get; set; } = 2622;
}
