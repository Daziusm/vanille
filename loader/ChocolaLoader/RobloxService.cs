using System;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;

namespace Chocola
{
    internal static class RobloxService
    {
        public static bool IsRobloxRunning()
        {
            return Process.GetProcessesByName("RobloxPlayerBeta").Length > 0;
        }

        public static bool IsVanilleRunning()
        {
            return Process.GetProcessesByName("vanille").Length > 0;
        }

        public static int? GetPrimaryPid()
        {
            var process = Process.GetProcessesByName("RobloxPlayerBeta").FirstOrDefault();
            return process?.Id;
        }

        public static string GetRobloxSummary()
        {
            var processes = Process.GetProcessesByName("RobloxPlayerBeta");
            if (processes.Length == 0)
                return "Roblox not running";

            var first = processes[0];
            var title = GetMainWindowTitle(first.Id);
            var version = GetClientVersion();
            var versionNote = string.IsNullOrEmpty(version) ? "" : " [" + version + "]";
            return "Roblox (PID " + first.Id + ")" + versionNote + (string.IsNullOrWhiteSpace(title) ? "" : " - " + title);
        }

        public static string GetClientVersion()
        {
            try
            {
                var proc = Process.GetProcessesByName("RobloxPlayerBeta").FirstOrDefault();
                if (proc == null)
                    return null;

                var path = proc.MainModule?.FileName;
                if (string.IsNullOrEmpty(path))
                    return null;

                var folder = Path.GetFileName(Path.GetDirectoryName(path));
                if (folder != null && folder.StartsWith("version-", StringComparison.OrdinalIgnoreCase))
                    return folder;
            }
            catch
            {
                // MainModule can fail on some systems; offsets step will fall back to cached file.
            }

            return null;
        }

        static string GetMainWindowTitle(int pid)
        {
            var found = string.Empty;
            EnumWindows(
                (hwnd, _) =>
                {
                    int windowPid;
                    GetWindowThreadProcessId(hwnd, out windowPid);
                    if (windowPid != pid || !IsWindowVisible(hwnd))
                        return true;

                    var builder = new StringBuilder(512);
                    if (GetWindowText(hwnd, builder, builder.Capacity) > 0)
                    {
                        found = builder.ToString();
                        return false;
                    }

                    return true;
                },
                IntPtr.Zero);
            return found;
        }

        delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

        [DllImport("user32.dll")]
        static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);

        [DllImport("user32.dll")]
        static extern bool IsWindowVisible(IntPtr hWnd);

        [DllImport("user32.dll", SetLastError = true)]
        static extern int GetWindowText(IntPtr hWnd, StringBuilder lpString, int nMaxCount);

        [DllImport("user32.dll")]
        static extern uint GetWindowThreadProcessId(IntPtr hWnd, out int lpdwProcessId);
    }
}
