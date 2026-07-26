using System;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Net.Http;
using System.Text.RegularExpressions;
using System.Threading.Tasks;

namespace Chocola
{
    internal static class SourceDownloader
    {
        static readonly HttpClient Http = new HttpClient
        {
            Timeout = TimeSpan.FromMinutes(5)
        };

        public static async Task<Tuple<bool, string>> InstallAsync(
            string repoUrl,
            string branch,
            string destination,
            Action<string> status)
        {
            try
            {
                if (!TryParseGithubUrl(repoUrl, out var owner, out var repo))
                {
                    return Tuple.Create(false, "Invalid GitHub URL. Use https://github.com/owner/repo");
                }

                Directory.CreateDirectory(destination);
                status?.Invoke("Downloading sources...");

                var branchesToTry = await BuildBranchCandidatesAsync(owner, repo, branch).ConfigureAwait(false);
                byte[] bytes = null;
                string usedBranch = null;
                string lastError = null;

                foreach (var candidate in branchesToTry)
                {
                    var zipUrl = BuildZipUrl(owner, repo, candidate);
                    try
                    {
                        bytes = await Http.GetByteArrayAsync(zipUrl).ConfigureAwait(false);
                        usedBranch = candidate;
                        break;
                    }
                    catch (HttpRequestException ex)
                    {
                        lastError = ex.Message;
                    }
                }

                if (bytes == null || usedBranch == null)
                {
                    return Tuple.Create(
                        false,
                        "Could not download sources from GitHub. Verify that "
                        + owner + "/" + repo
                        + " exists and that branch "
                        + (string.IsNullOrWhiteSpace(branch) ? "master or main" : branch.Trim())
                        + " is available."
                        + (string.IsNullOrWhiteSpace(lastError) ? "" : " " + lastError));
                }

                var tempZip = Path.Combine(
                    Path.GetTempPath(),
                    "chocola_source_" + Guid.NewGuid().ToString("N") + ".zip");
                File.WriteAllBytes(tempZip, bytes);

                status?.Invoke("Extracting sources...");
                var staging = Path.Combine(destination, ".download_staging");
                if (Directory.Exists(staging))
                    Directory.Delete(staging, true);

                Directory.CreateDirectory(staging);
                ZipFile.ExtractToDirectory(tempZip, staging);
                File.Delete(tempZip);

                var extractedRoot = Directory.GetDirectories(staging).FirstOrDefault();
                if (extractedRoot == null)
                {
                    Directory.Delete(staging, true);
                    return Tuple.Create(false, "Downloaded archive did not contain a source folder.");
                }

                foreach (var entry in Directory.GetFileSystemEntries(extractedRoot))
                {
                    var name = Path.GetFileName(entry);
                    var target = Path.Combine(destination, name);
                    if (Directory.Exists(entry))
                    {
                        if (Directory.Exists(target))
                            Directory.Delete(target, true);
                        Directory.Move(entry, target);
                    }
                    else
                    {
                        if (File.Exists(target))
                            File.Delete(target);
                        File.Move(entry, target);
                    }
                }

                Directory.Delete(staging, true);
                status?.Invoke("Sources installed from " + owner + "/" + repo + " (" + usedBranch + ").");
                return Tuple.Create(true, (string)null);
            }
            catch (Exception ex)
            {
                return Tuple.Create(false, ex.Message);
            }
        }

        static async Task<string[]> BuildBranchCandidatesAsync(string owner, string repo, string branch)
        {
            var requested = string.IsNullOrWhiteSpace(branch) ? null : branch.Trim();
            var defaultBranch = await TryGetDefaultBranchAsync(owner, repo).ConfigureAwait(false);

            if (!string.IsNullOrEmpty(requested))
            {
                return new[]
                {
                    requested,
                    defaultBranch,
                    "master",
                    "main"
                }
                .Where(value => !string.IsNullOrWhiteSpace(value))
                .Distinct(StringComparer.OrdinalIgnoreCase)
                .ToArray();
            }

            return new[]
            {
                defaultBranch,
                "master",
                "main"
            }
            .Where(value => !string.IsNullOrWhiteSpace(value))
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .ToArray();
        }

        static async Task<string> TryGetDefaultBranchAsync(string owner, string repo)
        {
            try
            {
                var response = await Http.GetStringAsync(
                    "https://api.github.com/repos/" + owner + "/" + repo).ConfigureAwait(false);

                var match = Regex.Match(response, "\"default_branch\"\\s*:\\s*\"([^\"]+)\"");
                return match.Success ? match.Groups[1].Value : null;
            }
            catch
            {
                return null;
            }
        }

        static string BuildZipUrl(string owner, string repo, string branch)
        {
            return string.Format(
                "https://github.com/{0}/{1}/archive/refs/heads/{2}.zip",
                owner,
                repo,
                Uri.EscapeDataString(branch));
        }

        static bool TryParseGithubUrl(string url, out string owner, out string repo)
        {
            owner = null;
            repo = null;
            if (string.IsNullOrWhiteSpace(url))
                return false;

            var match = Regex.Match(
                url.Trim().TrimEnd('/'),
                @"^https?://github\.com/([^/]+)/([^/]+?)(?:\.git)?(?:/.*)?$",
                RegexOptions.IgnoreCase);
            if (!match.Success)
                return false;

            owner = match.Groups[1].Value;
            repo = match.Groups[2].Value;
            return !string.IsNullOrEmpty(owner) && !string.IsNullOrEmpty(repo);
        }
    }
}
