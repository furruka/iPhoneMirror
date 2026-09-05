// SPDX-License-Identifier: GPL-3.0-only
//
// The shell application and its single window. Small on purpose: everything that
// matters happens in the preview control and in Core.

using Avalonia;
using Avalonia.Controls;
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
    private static Action<int> _report = _ => { };

    internal static void Configure(string path, uint width, uint height,
        int frameBudget, Action<int> report)
    {
        _path = path;
        _width = width;
        _height = height;
        _frameBudget = frameBudget;
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
        var player = new FramePlayer(_path, _width, _height, _frameBudget);
        preview.Ready += (_, _) => player.Start(preview, status, _report);
        desktop.MainWindow.Closed += (_, _) => player.Stop();
        base.OnFrameworkInitializationCompleted();
    }
}
