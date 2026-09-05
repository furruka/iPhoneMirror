// SPDX-License-Identifier: GPL-3.0-only
//
// Feeds NV12 frames from a file into the preview control on a timer.
//
// Reads one frame at a time rather than slurping the file: a minute of 1170x2532
// is several gigabytes, and the streaming path will hand over one frame at a
// time anyway, so this matches the shape the real source will have.

using System.Globalization;
using Avalonia.Controls;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Threading;
using IPhoneMirror.LinuxShell.Controls;

namespace IPhoneMirror.LinuxShell;

internal sealed class FramePlayer(string path, uint width, uint height,
    int frameBudget)
{
    private readonly byte[] _frame =
        new byte[checked((int)(width * (long)height * 3 / 2))];
    private FileStream? _input;
    private DispatcherTimer? _timer;
    private int _presented;
    private bool _stopped;

    internal void Start(PreviewSurfaceControl preview, TextBlock status,
        Action<int> report)
    {
        status.Text = preview.Diagnostic;
        try
        {
            _input = File.OpenRead(path);
        }
        catch (Exception exception)
        {
            status.Text = $"cannot open {path}: {exception.Message}";
            report(1);
            return;
        }

        _timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(16) };
        _timer.Tick += async (_, _) =>
        {
            if (_stopped) return;
            if (!ReadFrame())
            {
                Finish(status, report);
                return;
            }
            if (!await preview.PresentAsync(_frame, width, height))
            {
                status.Text = preview.Diagnostic;
                Finish(status, report, failed: true);
                return;
            }
            ++_presented;
            status.Text = string.Create(CultureInfo.InvariantCulture,
                $"presented {_presented} frames of {width}x{height}");
            if (frameBudget > 0 && _presented >= frameBudget)
                Finish(status, report);
        };
        _timer.Start();
    }

    private bool ReadFrame()
    {
        if (_input is null) return false;
        var offset = 0;
        while (offset < _frame.Length)
        {
            var read = _input.Read(_frame, offset, _frame.Length - offset);
            if (read <= 0)
            {
                // Loop so a short clip keeps the window alive for inspection.
                if (offset == 0 && _input.Position > 0)
                {
                    _input.Position = 0;
                    continue;
                }
                return false;
            }
            offset += read;
        }
        return true;
    }

    private void Finish(TextBlock status, Action<int> report, bool failed = false)
    {
        Stop();
        report(failed || _presented == 0 ? 1 : 0);
        if (!failed)
        {
            status.Text = string.Create(CultureInfo.InvariantCulture,
                $"done: {_presented} frames presented");
        }
        if (frameBudget > 0)
        {
            Dispatcher.UIThread.Post(() =>
                (Avalonia.Application.Current?.ApplicationLifetime
                    as IClassicDesktopStyleApplicationLifetime)?.Shutdown());
        }
    }

    internal void Stop()
    {
        _stopped = true;
        _timer?.Stop();
        _timer = null;
        _input?.Dispose();
        _input = null;
    }
}
