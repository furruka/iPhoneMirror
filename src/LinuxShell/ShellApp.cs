// SPDX-License-Identifier: GPL-3.0-only
//
// The shell application and its single window. Small on purpose: everything that
// matters happens in the preview control and in Core.

using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Threading;
using IPhoneMirror.LinuxShell.Controls;

namespace IPhoneMirror.LinuxShell;

internal sealed class ShellApp : Application
{
    private static string _path = string.Empty;
    private static uint _width;
    private static uint _height;
    private static int _frameBudget;
    private static string? _serial;
    private static Action<int> _report = _ => { };

    internal static void Configure(string path, uint width, uint height,
        int frameBudget, string? serial, Action<int> report)
    {
        _path = path;
        _width = width;
        _height = height;
        _frameBudget = frameBudget;
        _serial = serial;
        _report = report;
    }

    public override void OnFrameworkInitializationCompleted()
    {
        if (ApplicationLifetime is not IClassicDesktopStyleApplicationLifetime desktop)
        {
            _report(70);
            return;
        }
        var preview = new PreviewSurfaceControl
        {
            TargetWidth = _width,
            TargetHeight = _height,
        };
        var status = new TextBlock
        {
            Text = "attaching…",
            Margin = new Thickness(8),
            VerticalAlignment = VerticalAlignment.Bottom,
            Foreground = Brushes.White,
        };
        desktop.MainWindow = new Window
        {
            Title = "iPhoneMirror — Linux preview",
            Width = 420,
            Height = 900,
            Background = Brushes.Black,
            Content = new Panel { Children = { preview, status } },
        };
        if (_serial is null)
        {
            var player = new FramePlayer(_path, _width, _height, _frameBudget);
            preview.Ready += (_, _) => player.Start(preview, status, _report);
            desktop.MainWindow.Closed += (_, _) => player.Stop();
        }
        else
        {
            var player = new DevicePlayer(_serial);
            preview.Ready += (_, _) => player.Start(preview, status, _report);
            desktop.MainWindow.Closed += (_, _) => player.Stop();
            AttachReverseControl(desktop.MainWindow, preview);
        }
        base.OnFrameworkInitializationCompleted();
    }

    // Reverse control is attached only in device mode: pointing at a file would
    // move the real iPad while the window showed something unrelated, which is a
    // good way to tap things by accident.
    private static void AttachReverseControl(Window window, Control preview)
    {
        var hid = new Services.BluezHidService();
        var bridge = new Services.HidInputBridge(hid)
        {
            DisplayedWidth = _width,
            DisplayedHeight = _height,
        };
        // Task.Run, not a bare call: Tmds.DBus captures the synchronization
        // context in effect when the connection is made, and starting this from
        // the UI thread made every incoming BlueZ call — every characteristic
        // read, every notification bookkeeping step — run on the UI thread. That
        // starved the timer presenting frames, so the picture froze a few frames
        // in even with no mouse input at all. A thread-pool thread has no
        // synchronization context, so the D-Bus work stays off the UI thread.
        _ = Task.Run(async () =>
        {
            var ready = await hid.StartAsync("iPhoneMirror Linux");
            Console.WriteLine(
                $"reverse control : {(ready ? "ready" : hid.Diagnostic)}");
        });

        preview.PointerMoved += (_, e) => bridge.PointerMoved(e.GetPosition(preview));
        preview.PointerExited += (_, _) => bridge.PointerLost();
        preview.PointerPressed += (_, e) =>
        {
            preview.Focus();
            bridge.ButtonChanged(ButtonIndex(e.GetCurrentPoint(preview)), true);
        };
        preview.PointerReleased += (_, e) =>
            bridge.ButtonChanged(ButtonIndex(e.GetCurrentPoint(preview)), false);
        preview.PointerWheelChanged += (_, e) => bridge.Wheel(e.Delta.Y);
        window.KeyDown += (_, e) => bridge.KeyChanged(e.Key, true, e.KeyModifiers);
        window.KeyUp += (_, e) => bridge.KeyChanged(e.Key, false, e.KeyModifiers);
        window.Closed += (_, _) =>
        {
            bridge.Dispose();
            _ = hid.DisposeAsync();
        };
    }

    private static int ButtonIndex(PointerPoint point) =>
        point.Properties.PointerUpdateKind switch
        {
            PointerUpdateKind.RightButtonPressed or
                PointerUpdateKind.RightButtonReleased => 1,
            PointerUpdateKind.MiddleButtonPressed or
                PointerUpdateKind.MiddleButtonReleased => 2,
            _ => 0,
        };
}
