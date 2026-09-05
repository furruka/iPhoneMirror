// SPDX-License-Identifier: GPL-3.0-only
//
// Drives the preview from a live capture session instead of a file.
//
// The pump is deliberately dumb: poll the newest decoded frame on a timer and
// present it. The mailbox behind im_linux_preview_present_latest hands out an
// immutable frame, so there is no queue to drain here and a slow tick drops
// frames rather than accumulating latency — which is the right trade for a
// mirror.

using System.Globalization;
using Avalonia.Controls;
using Avalonia.Threading;
using IPhoneMirror.LinuxShell.Controls;
using IPhoneMirror.LinuxShell.Interop;

namespace IPhoneMirror.LinuxShell;

internal sealed class DevicePlayer(string serial)
{
    private DispatcherTimer? _timer;
    private int _presented;
    private int _waiting;
    private bool _started;

    internal void Start(PreviewSurfaceControl preview, TextBlock status,
        Action<int> report)
    {
        Console.WriteLine(preview.Diagnostic);
        if (CorePreview.Initialize() != 0)
        {
            Report(status, report, $"im_initialize: {CorePreview.LastError()}", 1);
            return;
        }
        var start = CorePreview.StartCapture(serial, 1);
        if (start != 0)
        {
            Report(status, report,
                $"im_start_capture_ex returned {start}: {CorePreview.LastError()}",
                1);
            return;
        }
        _started = true;
        status.Text = $"capturing {serial}…";
        Console.WriteLine(status.Text);

        _timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(8) };
        _timer.Tick += async (_, _) => await TickAsync(preview, status);
        _timer.Start();
    }

    private async Task TickAsync(PreviewSurfaceControl preview, TextBlock status)
    {
        var result = CorePreview.PresentLatest();
        if (result == -6)
        {
            // DeviceNotFound: no frame yet. Normal while the handshake runs.
            if (++_waiting % 125 == 0)
                Console.WriteLine($"waiting for the first frame ({_waiting} polls)");
            return;
        }
        if (result != 0)
        {
            status.Text = $"present failed: {result} {CorePreview.LastError()}";
            Console.WriteLine(status.Text);
            return;
        }
        if (!await preview.CommitAsync())
        {
            status.Text = preview.Diagnostic;
            Console.WriteLine(status.Text);
            return;
        }
        ++_presented;
        status.Text = string.Create(CultureInfo.InvariantCulture,
            $"presented {_presented} frames from {serial}");
        if (_presented <= 3 || _presented % 60 == 0)
            Console.WriteLine(status.Text);
    }

    private void Report(TextBlock status, Action<int> report, string message,
        int code)
    {
        status.Text = message;
        Console.WriteLine(message);
        report(code);
    }

    internal void Stop()
    {
        _timer?.Stop();
        _timer = null;
        if (_started) CorePreview.StopCapture();
        _started = false;
    }
}
