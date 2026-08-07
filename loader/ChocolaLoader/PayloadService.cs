using System;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;

namespace Chocola
{
    internal static class PayloadService
    {
        const string PayloadResourceName = "payload.zip";
        const string VersionMarker = ".payload_version";
        const string PayloadEpoch = "8";

        public static bool EnsureInstalled(string installDir, Action<string> status, out string error)
        {
            error = null;
            try
            {
                Directory.CreateDirectory(installDir);
                status?.Invoke("Checking Vanille payload...");

                var zipBytes = LoadEmbeddedZip();
                if (zipBytes == null || zipBytes.Length == 0)
                {
                    error = "Embedded Vanille payload is missing from the loader.";
                    return false;
                }

                var hash = ComputeSha256(zipBytes);
                if (IsCurrent(installDir, hash))
                {
                    status?.Invoke("Payload is up to date.");
                    return true;
                }

                status?.Invoke("Installing embedded Vanille payload...");
                var staging = Path.Combine(installDir, ".staging");
                if (Directory.Exists(staging))
                    Directory.Delete(staging, true);

                Directory.CreateDirectory(staging);
                var tempZip = Path.Combine(Path.GetTempPath(), "chocola_payload_" + Guid.NewGuid().ToString("N") + ".zip");
                try
                {
                    File.WriteAllBytes(tempZip, zipBytes);
                    ZipFile.ExtractToDirectory(tempZip, staging);
                }
                finally
                {
                    if (File.Exists(tempZip))
                        File.Delete(tempZip);
                }

                foreach (var entry in Directory.GetFileSystemEntries(staging))
                {
                    var name = Path.GetFileName(entry);
                    var dest = Path.Combine(installDir, name);
                    if (Directory.Exists(entry))
                    {
                        if (Directory.Exists(dest))
                            Directory.Delete(dest, true);
                        Directory.Move(entry, dest);
                    }
                    else
                    {
                        if (File.Exists(dest))
                            File.Delete(dest);
                        File.Move(entry, dest);
                    }
                }

                if (Directory.Exists(staging))
                    Directory.Delete(staging, true);

                File.WriteAllText(Path.Combine(installDir, VersionMarker), hash + "|" + PayloadEpoch, Encoding.ASCII);
                status?.Invoke("Payload installed.");
                return File.Exists(Path.Combine(installDir, "vanille.exe"));
            }
            catch (Exception ex)
            {
                error = ex.Message;
                return false;
            }
        }

        public static bool LaunchVanille(string installDir, Action<string> status, out string error)
        {
            error = null;
            var vanillePath = Path.Combine(installDir, "vanille.exe");
            if (!File.Exists(vanillePath))
            {
                error = "vanille.exe is missing from the install directory.";
                return false;
            }

            status?.Invoke("Launching Vanille...");
            var startInfo = new ProcessStartInfo
            {
                FileName = vanillePath,
                WorkingDirectory = installDir,
                UseShellExecute = true
            };

            Process.Start(startInfo);
            status?.Invoke("Vanille launched.");
            return true;
        }

        static byte[] LoadEmbeddedZip()
        {
            using (var stream = Assembly.GetExecutingAssembly().GetManifestResourceStream(PayloadResourceName))
            {
                if (stream == null)
                    return null;

                using (var ms = new MemoryStream())
                {
                    stream.CopyTo(ms);
                    return ms.ToArray();
                }
            }
        }

        static bool IsCurrent(string installDir, string hash)
        {
            var vanille = Path.Combine(installDir, "vanille.exe");
            var marker = Path.Combine(installDir, VersionMarker);
            if (!File.Exists(vanille) || !File.Exists(marker))
                return false;

            var markerText = File.ReadAllText(marker).Trim();
            return markerText == (hash + "|" + PayloadEpoch);
        }

        static string ComputeSha256(byte[] data)
        {
            using (var sha = SHA256.Create())
            {
                var hash = sha.ComputeHash(data);
                var builder = new StringBuilder(hash.Length * 2);
                foreach (var b in hash)
                    builder.Append(b.ToString("x2"));
                return builder.ToString();
            }
        }
    }
}
