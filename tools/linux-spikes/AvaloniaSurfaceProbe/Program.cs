// SPDX-License-Identifier: GPL-3.0-only
//
// Linux port spike S3/S4 managed consumer: presents a libplacebo-rendered
// Vulkan image through Avalonia, optionally decoding a real video stream.
//
// B1 (Avalonia instead of Qt/GTK) only holds if the answer is yes, because the
// mirroring preview cannot go through Avalonia's own drawing APIs at 60 fps.
// The Windows path is D3D11 -> shared texture -> DirectComposition; this probe
// exercises the proposed Linux path, FFmpeg -> libplacebo -> exported opaque FD
// -> ICompositionGpuInterop -> CompositionDrawingSurface.
//
// Without --input the probe is headless-friendly: it presents a fixed number of
// gradient frames and exits with a non-zero status if any step fails. With
// --input it plays the stream in a window until it is closed, which is how the
// decode/render pipeline is eyeballed on a real compositor.

using System.Globalization;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Threading;

namespace IPhoneMirror.Linux.AvaloniaSurfaceProbe;

internal static class Program
{
    private static int _exitCode = 70;

    private static int Main(string[] arguments)
    {
        var options = ProbeOptions.Parse(arguments);
        if (options is null)
        {
            Console.Error.WriteLine(
                "usage: iPhoneMirror.Linux.AvaloniaSurfaceProbe [--input <path>] "
                + "[--software] [--loop] [--frames N] "
                + "[--rendering-mode vulkan|egl|glx] [--verbose]");
            return 64;
        }

        Console.WriteLine(
            "Linux port spike S3/S4: libplacebo image presented through Avalonia");
        Console.WriteLine($"rendering mode        : {options.RenderingModeName}");
        Console.WriteLine($"input                 : {options.InputPath ?? "(gradient)"}");
        if (options.InputPath is not null)
        {
            Console.WriteLine(
                $"decode                : {(options.ForceSoftwareDecode ? "forced software" : "VAAPI with software fallback")}"
                + (options.Loop ? ", looping" : string.Empty));
        }
        else
        {
            Console.WriteLine($"frame budget          : {options.FrameBudget}");
        }

        AppBuilder.Configure(() => new ProbeApplication(options))
            .UsePlatformDetect()
            .With(new X11PlatformOptions { RenderingMode = options.RenderingModes })
            .StartWithClassicDesktopLifetime(Array.Empty<string>());

        return _exitCode;
    }

    internal static void Report(PlaceboSurfaceControl.ProbeResult result)
    {
        Console.Write(result.Summary);
        Console.WriteLine($"result                : {(result.Success ? "PASS" : "FAIL")}");
        _exitCode = result.Success ? 0 : 1;

        Dispatcher.UIThread.Post(() =>
            (Application.Current?.ApplicationLifetime
                as IClassicDesktopStyleApplicationLifetime)?.Shutdown());
    }
}

internal sealed record ProbeOptions(int FrameBudget, string? InputPath,
    bool ForceSoftwareDecode, bool Loop, string RenderingModeName,
    IReadOnlyList<X11RenderingMode> RenderingModes, bool Verbose)
{
    internal static ProbeOptions? Parse(IReadOnlyList<string> arguments)
    {
        var frames = 120;
        string? inputPath = null;
        var forceSoftwareDecode = false;
        var loop = false;
        var modeName = "vulkan";
        var verbose = false;

        for (var index = 0; index < arguments.Count; index++)
        {
            switch (arguments[index])
            {
                case "--frames" when index + 1 < arguments.Count:
                    if (!int.TryParse(arguments[++index],
                            NumberStyles.Integer, CultureInfo.InvariantCulture,
                            out frames) || frames <= 0)
                        return null;
                    break;
                case "--input" when index + 1 < arguments.Count:
                    inputPath = arguments[++index];
                    break;
                case "--software":
                    forceSoftwareDecode = true;
                    break;
                case "--loop":
                    loop = true;
                    break;
                case "--rendering-mode" when index + 1 < arguments.Count:
                    modeName = arguments[++index];
                    break;
                case "--verbose":
                    verbose = true;
                    break;
                default:
                    return null;
            }
        }

        IReadOnlyList<X11RenderingMode> modes = modeName switch
        {
            "vulkan" => new[] { X11RenderingMode.Vulkan },
            "egl" => new[] { X11RenderingMode.Egl },
            "glx" => new[] { X11RenderingMode.Glx },
            _ => Array.Empty<X11RenderingMode>(),
        };
        return modes.Count == 0
            ? null
            : new ProbeOptions(frames, inputPath, forceSoftwareDecode, loop,
                modeName, modes, verbose);
    }
}

internal sealed class ProbeApplication : Application
{
    private readonly ProbeOptions _options;

    internal ProbeApplication(ProbeOptions options) => _options = options;

    public override void OnFrameworkInitializationCompleted()
    {
        var control = new PlaceboSurfaceControl(_options.FrameBudget,
            _options.InputPath, _options.ForceSoftwareDecode, _options.Loop,
            _options.Verbose);
        _ = control.Completion.Task.ContinueWith(
            task => Program.Report(task.Result), TaskScheduler.Default);

        // The exported image is 1170x2532; a window at half scale keeps the
        // phone aspect ratio while fitting a 1080p screen.
        new Window
        {
            Width = 585,
            Height = 1266,
            Title = "iPhoneMirror Linux spike S3/S4",
            Content = control,
        }.Show();

        base.OnFrameworkInitializationCompleted();
    }
}
