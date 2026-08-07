using System;
using System.Collections.Generic;
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
            return Process.GetProcessesByName("RobloxPlayerBeta").Length > 0
                || Process.GetProcessesByName("ProjectXPlayerBeta").Length > 0;
        }

        public static bool IsVanilleRunning()
        {
            return Process.GetProcessesByName("vanille").Length > 0;
        }

        public static int? GetPrimaryPid()
        {
            var process = Process.GetProcessesByName("RobloxPlayerBeta").FirstOrDefault()
                ?? Process.GetProcessesByName("ProjectXPlayerBeta").FirstOrDefault();
            return process?.Id;
        }

        public static string GetRobloxSummary()
        {
            var process = Process.GetProcessesByName("RobloxPlayerBeta").FirstOrDefault()
                ?? Process.GetProcessesByName("ProjectXPlayerBeta").FirstOrDefault();
            if (process == null)
                return "Roblox not running";

            var title = GetMainWindowTitle(process.Id);
            var version = GetClientVersion();
            var versionNote = string.IsNullOrEmpty(version) ? "" : " [" + version + "]";
            return "Roblox (PID " + process.Id + ")" + versionNote + (string.IsNullOrWhiteSpace(title) ? "" : " - " + title);
        }

        public static string GetClientVersion()
        {
            var fromProcess = GetClientVersionFromProcess();
            if (!string.IsNullOrEmpty(fromProcess))
                return fromProcess;

            return GetClientVersionFromInstallDirs();
        }

        static string GetClientVersionFromProcess()
        {
            foreach (var processName in new[] { "RobloxPlayerBeta", "ProjectXPlayerBeta" })
            {
                var proc = Process.GetProcessesByName(processName).FirstOrDefault();
                if (proc == null)
                    continue;

                var path = TryGetProcessImagePath(proc);
                if (string.IsNullOrEmpty(path))
                    continue;

                var folder = Path.GetFileName(Path.GetDirectoryName(path));
                if (folder != null && folder.StartsWith("version-", StringComparison.OrdinalIgnoreCase))
                    return folder;
            }

            return null;
        }

        static string TryGetProcessImagePath(Process proc)
        {
            try
            {
                var path = proc.MainModule?.FileName;
                if (!string.IsNullOrEmpty(path))
                    return path;
            }
            catch
            {
                // MainModule can fail without elevation.
            }

            return QueryProcessImagePath(proc.Id);
        }

        static string QueryProcessImagePath(int pid)
        {
            const uint processQueryLimitedInformation = 0x1000;
            var handle = OpenProcess(processQueryLimitedInformation, false, pid);
            if (handle == IntPtr.Zero)
                return null;

            try
            {
                var builder = new StringBuilder(1024);
                var size = builder.Capacity;
                if (!QueryFullProcessImageName(handle, 0, builder, ref size))
                    return null;

                return builder.ToString(0, size);
            }
            finally
            {
                CloseHandle(handle);
            }
        }

        static string GetClientVersionFromInstallDirs()
        {
            string bestVersion = null;
            DateTime bestTime = DateTime.MinValue;

            foreach (var versionsRoot in GetClientVersionRoots())
            {
                if (!Directory.Exists(versionsRoot))
                    continue;

                foreach (var versionDir in Directory.GetDirectories(versionsRoot, "version-*"))
                {
                    var version = Path.GetFileName(versionDir);
                    if (string.IsNullOrEmpty(version))
                        continue;

                    var stamp = GetVersionStamp(versionDir);
                    if (stamp > bestTime)
                    {
                        bestTime = stamp;
                        bestVersion = version;
                    }
                }
            }

            return bestVersion;
        }

        static IEnumerable<string> GetClientVersionRoots()
        {
            var localAppData = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
            var programFilesX86 = Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86);

            yield return Path.Combine(localAppData, "Roblox", "Versions");
            yield return Path.Combine(programFilesX86, "Roblox", "Versions");
        }

        static DateTime GetVersionStamp(string versionDir)
        {
            var candidates = new[]
            {
                Path.Combine(versionDir, "RobloxPlayerBeta.exe"),
                Path.Combine(versionDir, "ProjectXPlayerBeta.exe"),
            };

            foreach (var candidate in candidates)
            {
                if (!File.Exists(candidate))
                    continue;

                try
                {
                    return File.GetLastWriteTimeUtc(candidate);
                }
                catch
                {
                    // Ignore unreadable binaries.
                }
            }

            try
            {
                return Directory.GetLastWriteTimeUtc(versionDir);
            }
            catch
            {
                return DateTime.MinValue;
            }
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

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern IntPtr OpenProcess(uint dwDesiredAccess, bool bInheritHandle, int dwProcessId);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool CloseHandle(IntPtr hObject);

        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        static extern bool QueryFullProcessImageName(
            IntPtr hProcess,
            int dwFlags,
            StringBuilder lpExeName,
            ref int lpdwSize);
    }
}
