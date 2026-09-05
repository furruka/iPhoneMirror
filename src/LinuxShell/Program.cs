// SPDX-License-Identifier: GPL-3.0-only
//
// P5a shell entry point. Opens a window with the preview control and drives it
// from a raw NV12 file, which is the same input LinuxDecodeProbe writes.
//
// Playing a file rather than a device is deliberate for this step: it proves the
// window binds and shows what Core rendered, without needing an iPhone attached.
// Wiring the capture session's decoder output to the same renderer is the next
// increment and changes nothing on this side.

using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Threading;
using IPhoneMirror.LinuxShell.Controls;

namespace IPhoneMirror.LinuxShell;

internal static class Program
{
    private static int _exitCode = 70;

    private static int Main(string[] arguments)
    {
        if (arguments.Length >= 2 && arguments[0] == "--device")
        {
            return RunDevice(arguments[1],
                arguments.Length >= 4 ? arguments[2..4] : null);
        }
        if (arguments.Length < 3)
        {
            Console.Error.WriteLine(
                "usage: iPhoneMirror.LinuxShell <frames.nv12> <width> <height> "
                + "[--frames N]\n"
                + "       iPhoneMirror.LinuxShell --device <udid> "
                + "[<width> <height>]");
            return 64;
        }
        var path = arguments[0];
        if (!uint.TryParse(arguments[1], out var width) ||
            !uint.TryParse(arguments[2], out var height) ||
            width == 0 || height == 0)
        {
            Console.Error.WriteLine("the geometry must be positive integers");
            return 64;
        }
        var budget = 0;
        for (var index = 3; index < arguments.Length - 1; ++index)
        {
            if (arguments[index] == "--frames")
                _ = int.TryParse(arguments[index + 1], out budget);
        }

        // Vulkan: the exported image can only be imported by a Vulkan-backed
        // compositor, and the spike measured this as the one working combination.
        AppBuilder.Configure<ShellApp>()
            .UsePlatformDetect()
            .With(new X11PlatformOptions
            {
                RenderingMode = new[] { X11RenderingMode.Vulkan },
            })
            .AfterSetup(_ => ShellApp.Configure(path, width, height, budget, null,
                code => _exitCode = code))
            .StartWithClassicDesktopLifetime(Array.Empty<string>());
        return _exitCode;
    }

    // The preview target defaults to the highest encoder tier verified on both
    // test devices; the device decides the source geometry and Core letterboxes
    // into whatever target it is given.
    private static int RunDevice(string serial, string[]? geometry)
    {
        uint width = 1206;
        uint height = 2622;
        if (geometry is { Length: 2 })
        {
            _ = uint.TryParse(geometry[0], out width);
            _ = uint.TryParse(geometry[1], out height);
        }
        AppBuilder.Configure<ShellApp>()
            .UsePlatformDetect()
            .With(new X11PlatformOptions
            {
                RenderingMode = new[] { X11RenderingMode.Vulkan },
            })
            .AfterSetup(_ => ShellApp.Configure(string.Empty, width, height, 0,
                serial, code => _exitCode = code))
            .StartWithClassicDesktopLifetime(Array.Empty<string>());
        return _exitCode;
    }
}
