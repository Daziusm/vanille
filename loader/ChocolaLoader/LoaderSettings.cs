using System;
using System.IO;

namespace Chocola
{
    internal sealed class LoaderSettings
    {
        public string InstallPath { get; set; } = DefaultInstallPath();
        public string SourcePath { get; set; } = DefaultSourcePath();
        public string RepoUrl { get; set; } = "https://github.com/Daziusm/vanille";
        public string Branch { get; set; } = "master";
        public string OffsetsUrl { get; set; } = "";
        public string OffsetsPath { get; set; } = "";
        public string DumperPath { get; set; } = "";

        static string SettingsFile
        {
            get
            {
                var dir = Path.Combine(
                    Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
                    "Chocola");
                Directory.CreateDirectory(dir);
                return Path.Combine(dir, "loader.ini");
            }
        }

        public static string DefaultInstallPath()
        {
            return Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "Chocola");
        }

        public static string DefaultSourcePath()
        {
            return Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments),
                "Vanille-src");
        }

        public static LoaderSettings Load()
        {
            var settings = new LoaderSettings();
            var file = SettingsFile;
            if (!File.Exists(file))
                return settings;

            foreach (var line in File.ReadAllLines(file))
            {
                var parts = line.Split(new[] { '=' }, 2);
                if (parts.Length != 2)
                    continue;

                var key = parts[0].Trim();
                var value = parts[1].Trim();
                switch (key)
                {
                    case "install_path":
                        settings.InstallPath = value;
                        break;
                    case "source_path":
                        settings.SourcePath = value;
                        break;
                    case "repo_url":
                        settings.RepoUrl = value;
                        break;
                    case "branch":
                        settings.Branch = value;
                        break;
                    case "offsets_url":
                        settings.OffsetsUrl = value;
                        break;
                    case "offsets_path":
                        settings.OffsetsPath = value;
                        break;
                    case "dumper_path":
                        settings.DumperPath = value;
                        break;
                }
            }

            if (settings.RepoUrl == "https://github.com/0x1408/vanille"
                || settings.RepoUrl == "https://github.com/your-org/vanille")
            {
                settings.RepoUrl = "https://github.com/Daziusm/vanille";
                settings.Save();
            }

            if (settings.Branch == "main" && settings.RepoUrl == "https://github.com/Daziusm/vanille")
            {
                settings.Branch = "master";
                settings.Save();
            }

            return settings;
        }

        public void Save()
        {
            var lines = new[]
            {
                "[loader]",
                "install_path=" + InstallPath,
                "source_path=" + SourcePath,
                "repo_url=" + RepoUrl,
                "branch=" + Branch,
                "offsets_url=" + OffsetsUrl,
                "offsets_path=" + OffsetsPath,
                "dumper_path=" + DumperPath
            };
            File.WriteAllLines(SettingsFile, lines);
        }
    }
}
