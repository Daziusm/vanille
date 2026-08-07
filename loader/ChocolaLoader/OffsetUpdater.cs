using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Http;
using System.Text;
using System.Text.RegularExpressions;
using System.Web.Script.Serialization;

namespace Chocola
{
    /// Fetches offsets.imtheo.lol offsets.txt when the Roblox client version changes.
    internal static class OffsetUpdater
    {
        const string ImtheoLatestVersion = "version-d584fb6c717a43d9";

        static readonly Regex ImtheoTxtLine = new Regex(
            @"^(\w+)::(\w+)\s*=\s*(0x[0-9a-fA-F]+)\s*$",
            RegexOptions.Compiled | RegexOptions.IgnoreCase);

        static readonly HttpClient Http = new HttpClient
        {
            Timeout = TimeSpan.FromSeconds(30)
        };

        static readonly Regex VersionLine = new Regex(
            @"version-[a-f0-9]+",
            RegexOptions.IgnoreCase | RegexOptions.Compiled);

        sealed class OffsetPayload
        {
            public Dictionary<string, Dictionary<string, int>> Offsets;
            public string Source;
        }

        public static bool TryRefresh(
            string installDir,
            LoaderSettings settings,
            Action<string> status,
            out string error)
        {
            error = null;
            var version = RobloxService.GetClientVersion();
            if (string.IsNullOrEmpty(version))
            {
                status?.Invoke("Offsets: Roblox version unknown.");
                return true;
            }

            var valuesPath = Path.Combine(installDir, "values.txt");
            var cached = ReadCachedVersion(valuesPath);
            if (cached == version && File.Exists(valuesPath) && !ValuesLookStale(valuesPath))
            {
                status?.Invoke("Offsets OK (" + version + ").");
                return true;
            }

            status?.Invoke("Fetching offsets for " + version + "...");
            try
            {
                var payload = AcquireOffsets(version, installDir, settings, status);
                if (payload == null || !IsUsableOffsets(payload.Offsets))
                {
                    if (TryGenerateValuesFromDumperJson(installDir, settings, version, status, out var generatedPath))
                    {
                        status?.Invoke("Offsets updated (roblox-dumper).");
                        return true;
                    }

                    throw new InvalidDataException(
                        "No usable offsets found for " + version + ". " +
                        "offsets.imtheo.lol may not have this Roblox build yet — wait for Theo, run roblox-dumper, or set offsets_path in loader.ini.");
                }

                var text = BuildValuesText(version, payload.Offsets, payload.Source);
                Directory.CreateDirectory(installDir);
                File.WriteAllText(valuesPath, text, Encoding.UTF8);
                status?.Invoke("Offsets updated (" + payload.Source + ").");
                return true;
            }
            catch (Exception ex)
            {
                if (File.Exists(valuesPath))
                {
                    status?.Invoke("Offset fetch failed — using cached file.");
                    return true;
                }

                error = FormatFetchError(version, ex);
                return false;
            }
        }

        static string FormatFetchError(string version, Exception ex)
        {
            if (ex is WebException webEx && webEx.Response is HttpWebResponse response && response.StatusCode == HttpStatusCode.NotFound)
                return "offsets.imtheo.lol has no offsets for " + version + " yet. " +
                       "Roblox may have updated before Theo posted a dump — try again later or set offsets_path in loader.ini.";

            if (ex is HttpRequestException && ex.Message.IndexOf("404", StringComparison.OrdinalIgnoreCase) >= 0)
                return "offsets.imtheo.lol has no offsets for " + version + " yet. " +
                       "Roblox may have updated before Theo posted a dump — try again later or set offsets_path in loader.ini.";

            if (ex is KeyNotFoundException)
                return "Offset mapping failed: " + ex.Message + ". imtheo JSON may be missing a field — report this version.";

            return ex.Message;
        }

        static OffsetPayload AcquireOffsets(
            string version,
            string installDir,
            LoaderSettings settings,
            Action<string> status)
        {
            OffsetPayload payload = null;

            if (!string.IsNullOrWhiteSpace(settings?.OffsetsUrl))
            {
                status?.Invoke("Trying custom offsets URL...");
                payload = TryFetchUrl(settings.OffsetsUrl.Trim(), "custom");
                if (IsUsableOffsets(payload?.Offsets))
                    return payload;
            }

            status?.Invoke("Trying offsets.imtheo.lol offsets.txt...");
            bool imtheoNotFound;
            payload = TryFetchImtheoTxt(version, out imtheoNotFound);
            if (!IsUsableOffsets(payload?.Offsets) && imtheoNotFound && version != ImtheoLatestVersion)
            {
                status?.Invoke("No offsets.txt for " + version + " — trying latest imtheo offsets...");
                payload = TryFetchImtheoTxt(ImtheoLatestVersion, out _);
                if (IsUsableOffsets(payload?.Offsets))
                    payload.Source = "imtheo-txt-latest";
            }

            if (IsUsableOffsets(payload?.Offsets))
                return payload;

            status?.Invoke("Trying offsets.imtheo.lol offsets.json...");
            payload = TryFetchImtheoJson(version, out imtheoNotFound);
            if (!IsUsableOffsets(payload?.Offsets) && imtheoNotFound && version != ImtheoLatestVersion)
            {
                payload = TryFetchImtheoJson(ImtheoLatestVersion, out _);
                if (IsUsableOffsets(payload?.Offsets))
                    payload.Source = "imtheo-json-latest";
            }

            if (IsUsableOffsets(payload?.Offsets))
                return payload;

            foreach (var candidate in LocalOffsetCandidates(installDir, settings))
            {
                status?.Invoke("Trying " + candidate + "...");
                payload = TryLoadLocalOffsets(candidate);
                if (IsUsableOffsets(payload?.Offsets))
                    return payload;
            }

            return payload;
        }

        static IEnumerable<string> LocalOffsetCandidates(string installDir, LoaderSettings settings)
        {
            if (!string.IsNullOrWhiteSpace(settings?.OffsetsPath))
                yield return settings.OffsetsPath.Trim();

            yield return Path.Combine(installDir, "offsets.txt");
            yield return Path.Combine(installDir, "offsets.json");
        }

        static OffsetPayload TryFetchImtheoTxt(string versionFolder, out bool notFound)
        {
            notFound = false;
            var url = "https://offsets.imtheo.lol/" + versionFolder + "/offsets.txt";
            try
            {
                var txt = Http.GetStringAsync(url).GetAwaiter().GetResult();
                return ParseImtheoOffsetsTxt(txt, "imtheo");
            }
            catch (Exception ex)
            {
                if (IsNotFound(ex))
                    notFound = true;
                return null;
            }
        }

        static OffsetPayload TryFetchImtheoJson(string versionFolder, out bool notFound)
        {
            notFound = false;
            var url = "https://offsets.imtheo.lol/" + versionFolder + "/offsets.json";
            try
            {
                var json = Http.GetStringAsync(url).GetAwaiter().GetResult();
                return ParseOffsetsJson(json, "imtheo");
            }
            catch (Exception ex)
            {
                if (IsNotFound(ex))
                    notFound = true;
                return null;
            }
        }

        static bool IsNotFound(Exception ex)
        {
            if (ex is WebException webEx && webEx.Response is HttpWebResponse response)
                return response.StatusCode == HttpStatusCode.NotFound;

            if (ex is HttpRequestException)
                return ex.Message.IndexOf("404", StringComparison.OrdinalIgnoreCase) >= 0;

            return false;
        }

        static OffsetPayload TryFetchUrl(string url, string sourceLabel)
        {
            try
            {
                var json = Http.GetStringAsync(url).GetAwaiter().GetResult();
                return ParseOffsetsJson(json, sourceLabel);
            }
            catch
            {
                return null;
            }
        }

        static OffsetPayload TryLoadLocalOffsets(string path)
        {
            if (string.IsNullOrWhiteSpace(path) || !File.Exists(path))
                return null;

            try
            {
                var text = File.ReadAllText(path, Encoding.UTF8);
                if (path.EndsWith(".txt", StringComparison.OrdinalIgnoreCase))
                    return ParseImtheoOffsetsTxt(text, "local");

                return ParseOffsetsJson(text, "local");
            }
            catch
            {
                return null;
            }
        }

        static OffsetPayload ParseImtheoOffsetsTxt(string text, string sourceLabel)
        {
            var nested = new Dictionary<string, Dictionary<string, int>>(StringComparer.Ordinal);
            foreach (var rawLine in text.Split(new[] { '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries))
            {
                var line = rawLine.Trim();
                if (string.IsNullOrEmpty(line))
                    continue;

                var match = ImtheoTxtLine.Match(line);
                if (!match.Success)
                    continue;

                var ns = match.Groups[1].Value;
                var key = match.Groups[2].Value;
                var value = Convert.ToInt32(match.Groups[3].Value.Substring(2), 16);

                Dictionary<string, int> section;
                if (!nested.TryGetValue(ns, out section))
                {
                    section = new Dictionary<string, int>(StringComparer.Ordinal);
                    nested[ns] = section;
                }

                section[key] = value;
            }

            if (nested.Count == 0)
                throw new InvalidDataException("offsets.txt contained no parseable offsets.");

            return new OffsetPayload
            {
                Offsets = nested,
                Source = sourceLabel
            };
        }

        static bool IsUsableOffsets(Dictionary<string, Dictionary<string, int>> offsets)
        {
            if (offsets == null || offsets.Count == 0)
                return false;

            Dictionary<string, int> taskScheduler;
            Dictionary<string, int> visualEngine;
            if (!offsets.TryGetValue("TaskScheduler", out taskScheduler) || taskScheduler == null)
                return false;
            if (!offsets.TryGetValue("VisualEngine", out visualEngine) || visualEngine == null)
                return false;

            int pointer;
            return taskScheduler.TryGetValue("Pointer", out pointer) && pointer != 0
                && visualEngine.Count > 0;
        }

        static string ReadCachedVersion(string valuesPath)
        {
            if (!File.Exists(valuesPath))
                return null;

            foreach (var line in File.ReadLines(valuesPath))
            {
                var match = VersionLine.Match(line);
                if (match.Success)
                    return match.Value;
            }

            return null;
        }

        static bool ValuesLookStale(string valuesPath)
        {
            foreach (var line in File.ReadLines(valuesPath))
            {
                if (line.IndexOf("task_scheduler::pointer", StringComparison.OrdinalIgnoreCase) >= 0
                    && (line.IndexOf("878D868", StringComparison.OrdinalIgnoreCase) >= 0
                        || line.IndexOf("878d868", StringComparison.OrdinalIgnoreCase) >= 0))
                {
                    return true;
                }

                if (line.IndexOf("rbx_string::length", StringComparison.OrdinalIgnoreCase) >= 0
                    && (line.IndexOf("'0x0'", StringComparison.OrdinalIgnoreCase) >= 0
                        || line.IndexOf("=> '0x0'", StringComparison.OrdinalIgnoreCase) >= 0))
                {
                    return true;
                }

                if (line.IndexOf("renderview::force_flag_byte", StringComparison.OrdinalIgnoreCase) >= 0
                    && line.IndexOf("0x228", StringComparison.OrdinalIgnoreCase) >= 0)
                {
                    return true;
                }
            }

            return false;
        }

        static bool TryGenerateValuesFromDumperJson(
            string installDir,
            LoaderSettings settings,
            string version,
            Action<string> status,
            out string valuesPath)
        {
            valuesPath = Path.Combine(installDir, "values.txt");
            foreach (var candidate in DumperJsonCandidates(installDir, settings))
            {
                if (!LooksLikeJonahJson(candidate))
                    continue;

                status?.Invoke("Converting roblox-dumper JSON...");
                if (!TryRunGenerateValuesScript(candidate, valuesPath, out var scriptError))
                {
                    status?.Invoke(scriptError ?? "roblox-dumper JSON conversion failed.");
                    continue;
                }

                if (!File.Exists(valuesPath) || ValuesLookStale(valuesPath))
                    continue;

                return true;
            }

            return false;
        }

        static IEnumerable<string> DumperJsonCandidates(string installDir, LoaderSettings settings)
        {
            if (!string.IsNullOrWhiteSpace(settings?.OffsetsPath))
                yield return settings.OffsetsPath.Trim();

            if (!string.IsNullOrWhiteSpace(settings?.DumperPath))
            {
                var dumperPath = settings.DumperPath.Trim();
                yield return Path.Combine(dumperPath, "offsets.json");
                if (dumperPath.EndsWith(".json", StringComparison.OrdinalIgnoreCase))
                    yield return dumperPath;
            }

            yield return Path.Combine(installDir, "offsets.json");
            yield return Path.Combine(installDir, "..", "roblox-dumper-official", "offsets.json");
            yield return Path.Combine(installDir, "..", "offsets.json");
        }

        static bool LooksLikeJonahJson(string path)
        {
            if (string.IsNullOrWhiteSpace(path) || !File.Exists(path))
                return false;

            try
            {
                var text = File.ReadAllText(path, Encoding.UTF8);
                return text.IndexOf("\"metadata\"", StringComparison.OrdinalIgnoreCase) >= 0
                    && text.IndexOf("\"offsets\"", StringComparison.OrdinalIgnoreCase) >= 0
                    && text.IndexOf("\"Offsets\"", StringComparison.OrdinalIgnoreCase) < 0;
            }
            catch
            {
                return false;
            }
        }

        static string FindGenerateValuesScript(string installDir)
        {
            var candidates = new[]
            {
                Path.Combine(installDir, "Vanille", "tools", "generate_values.py"),
                Path.Combine(installDir, "..", "Vanille", "tools", "generate_values.py"),
                Path.Combine(installDir, "..", "..", "Vanille", "tools", "generate_values.py"),
                @"D:\Vanille\Vanille\tools\generate_values.py",
            };

            foreach (var candidate in candidates)
            {
                try
                {
                    var full = Path.GetFullPath(candidate);
                    if (File.Exists(full))
                        return full;
                }
                catch
                {
                }
            }

            return null;
        }

        static bool TryRunGenerateValuesScript(string jsonPath, string outputPath, out string error)
        {
            error = null;
            var script = FindGenerateValuesScript(Path.GetDirectoryName(outputPath) ?? "");
            if (script == null)
            {
                error = "generate_values.py not found beside the install.";
                return false;
            }

            var generatedPath = Path.GetFullPath(Path.Combine(Path.GetDirectoryName(script) ?? "", "..", "values.txt"));
            try
            {
                var start = new ProcessStartInfo
                {
                    FileName = "python",
                    Arguments = "\"" + script + "\" \"" + jsonPath + "\"",
                    UseShellExecute = false,
                    CreateNoWindow = true,
                    RedirectStandardOutput = true,
                    RedirectStandardError = true,
                };

                using (var process = Process.Start(start))
                {
                    if (process == null)
                    {
                        error = "Failed to start python.";
                        return false;
                    }

                    var stderr = process.StandardError.ReadToEnd();
                    process.WaitForExit();
                    if (process.ExitCode != 0)
                    {
                        error = string.IsNullOrWhiteSpace(stderr) ? "generate_values.py failed." : stderr.Trim();
                        return false;
                    }
                }

                if (!File.Exists(generatedPath))
                {
                    error = "generate_values.py did not produce values.txt.";
                    return false;
                }

                Directory.CreateDirectory(Path.GetDirectoryName(outputPath) ?? installFallbackDir(outputPath));
                File.Copy(generatedPath, outputPath, true);
                return true;
            }
            catch (Exception ex)
            {
                error = ex.Message;
                return false;
            }
        }

        static string installFallbackDir(string outputPath)
        {
            return string.IsNullOrWhiteSpace(outputPath)
                ? LoaderSettings.DefaultInstallPath()
                : Path.GetDirectoryName(outputPath);
        }

        static OffsetPayload ParseOffsetsJson(string json, string sourceLabel)
        {
            var serializer = new JavaScriptSerializer { MaxJsonLength = int.MaxValue };
            var root = serializer.Deserialize<Dictionary<string, object>>(json);
            if (root == null)
                throw new InvalidDataException("Invalid offsets JSON.");

            Dictionary<string, object> offsetsObj = null;
            if (root.ContainsKey("Offsets"))
                offsetsObj = root["Offsets"] as Dictionary<string, object>;
            else if (root.ContainsKey("offsets"))
                offsetsObj = root["offsets"] as Dictionary<string, object>;

            if (offsetsObj == null)
                throw new InvalidDataException("Missing offsets section.");

            var result = new Dictionary<string, Dictionary<string, int>>(StringComparer.Ordinal);
            foreach (var pair in offsetsObj)
            {
                var section = pair.Value as Dictionary<string, object>;
                if (section == null)
                    continue;

                var mapped = new Dictionary<string, int>(StringComparer.Ordinal);
                foreach (var field in section)
                    mapped[field.Key] = Convert.ToInt32(field.Value);
                result[pair.Key] = mapped;
            }

            return new OffsetPayload
            {
                Offsets = result,
                Source = sourceLabel
            };
        }

        static string BuildValuesText(
            string version,
            Dictionary<string, Dictionary<string, int>> o,
            string source)
        {
            var strict = source == "imtheo" || source == "imtheo-txt-latest" || source == "imtheo-json-latest";
            var lines = new List<string>
            {
                "$offsets = [",
                "    // " + version + " (" + source + ")",
            };

            foreach (var entry in Entries(o, strict))
                lines.Add("    '" + entry.Key + "' => '" + entry.Value + "',");

            lines.Add("];");
            return string.Join("\n", lines) + "\n";
        }

        static IEnumerable<KeyValuePair<string, string>> Entries(
            Dictionary<string, Dictionary<string, int>> o,
            bool strict)
        {
            yield return Pair("task_scheduler::pointer", G(o, "TaskScheduler", "Pointer", strict));
            yield return Pair("task_scheduler::job_start", G(o, "TaskScheduler", "JobStart", strict));
            yield return Pair("task_scheduler::job_end", G(o, "TaskScheduler", "JobEnd", strict));
            yield return Pair("task_scheduler::job_name", G(o, "TaskScheduler", "JobName", strict));
            yield return Pair("task_scheduler::job_stride", "0x8");
            yield return Pair("task_scheduler::render_job_to_fake_datamodel", G(o, "RenderJob", "FakeDataModel", strict));
            yield return Pair("task_scheduler::fake_datamodel_to_datamodel", G(o, "FakeDataModel", "RealDataModel", strict));
            yield return Pair("task_scheduler::render_job_to_renderview", G(o, "RenderJob", "RenderView", strict));
            yield return Pair("task_scheduler::max_fps", G(o, "TaskScheduler", "MaxFPS", strict));
            yield return Pair("task_scheduler::target_fps", "0x0");
            yield return Pair("datamodel::datamodel_ptr0", G(o, "FakeDataModel", "Pointer", strict));
            yield return Pair("datamodel::datamodel_ptr1", G(o, "FakeDataModel", "RealDataModel", strict));
            yield return Pair("datamodel::place_id", G(o, "DataModel", "PlaceId", strict));
            yield return Pair("visualengine::visualengine_ptr", G(o, "VisualEngine", "Pointer", strict));
            yield return Pair("visualengine::view_matrix", G(o, "VisualEngine", "ViewMatrix", strict));
            yield return Pair("visualengine::dimensions", G(o, "VisualEngine", "Dimensions", strict));
            yield return Pair("renderview::force_flag_byte", G(o, "RenderView", "LightingValid", strict, 0x150));
            yield return Pair("renderview::force_flag_bool", G(o, "RenderView", "SkyValid", strict));
            yield return Pair("players::local_player", G(o, "Player", "LocalPlayer", strict));
            yield return Pair("player::display_name", G(o, "Player", "DisplayName", strict));
            yield return Pair("player::user_id", G(o, "Player", "UserId", strict));
            yield return Pair("player::team", G(o, "Player", "Team", strict));
            yield return Pair("player::team_color", G(o, "Player", "TeamColor", strict));
            yield return Pair("player::character", G(o, "Player", "ModelInstance", strict));
            yield return Pair("base_part::primitive", G(o, "BasePart", "Primitive", strict));
            yield return Pair("base_part::material", G(o, "Primitive", "Material", strict, 0x23E));
            yield return Pair("base_part::transparency", G(o, "BasePart", "Transparency", strict));
            yield return Pair("base_part::color3", G(o, "BasePart", "Color3", strict));
            yield return Pair("base_part::size", G(o, "Primitive", "Size", strict));
            yield return Pair("base_part::position", G(o, "Primitive", "Position", strict));
            yield return Pair("base_part::primitive_properties", "0xA0");
            yield return Pair("base_part::primitive_position", "0x90");
            yield return Pair("base_part::validate", G(o, "Primitive", "Validate", strict));
            yield return Pair("base_part::cframe_rotation", G(o, "Primitive", "Rotation", strict));
            yield return Pair("base_part::assembly_linear_velocity", G(o, "Primitive", "AssemblyLinearVelocity", strict));
            yield return Pair("base_part::assembly_angular_velocity", G(o, "Primitive", "AssemblyAngularVelocity", strict));
            yield return Pair("base_part::can_collide", G(o, "PrimitiveFlags", "CanCollide", strict));
            yield return Pair("base_part::can_collide_mask", "0x8");
            yield return Pair("humanoid::humanoid_state_id", G(o, "Humanoid", "HumanoidStateID", strict));
            yield return Pair("humanoid::move_direction", G(o, "Humanoid", "MoveDirection", strict));
            yield return Pair("humanoid::floor_material", G(o, "Humanoid", "FloorMaterial", strict));
            yield return Pair("humanoid::health", G(o, "Humanoid", "Health", strict));
            yield return Pair("humanoid::hip_height", G(o, "Humanoid", "HipHeight", strict));
            yield return Pair("humanoid::jump_height", G(o, "Humanoid", "JumpHeight", strict));
            yield return Pair("humanoid::jump_power", G(o, "Humanoid", "JumpPower", strict));
            yield return Pair("humanoid::max_health", G(o, "Humanoid", "MaxHealth", strict));
            yield return Pair("humanoid::max_slope_angle", G(o, "Humanoid", "MaxSlopeAngle", strict));
            yield return Pair("humanoid::rig_type", G(o, "Humanoid", "RigType", strict));
            yield return Pair("humanoid::walk_speed", G(o, "Humanoid", "Walkspeed", strict));
            yield return Pair("humanoid::auto_rotate", G(o, "Humanoid", "AutoRotate", strict));
            yield return Pair("humanoid::jump", G(o, "Humanoid", "Jump", strict));
            yield return Pair("humanoid::humanoid_state", G(o, "Humanoid", "HumanoidState", strict));
            yield return Pair("humanoid::walk_speed_check", G(o, "Humanoid", "WalkspeedCheck", strict));
            yield return Pair("value_bool::value", G(o, "Misc", "Value", strict));
            yield return Pair("value_int::value", G(o, "Misc", "Value", strict));
            yield return Pair("value_number::value", G(o, "Misc", "Value", strict));
            yield return Pair("instance::attribute_container", "0x48");
            yield return Pair("instance::attribute_list", "0x18");
            yield return Pair("instance::attribute_to_next", G(o, "Attribute", "Size", strict, 0x58));
            yield return Pair("instance::attribute_to_value", G(o, "Attribute", "Value", strict, 0x18));
            yield return Pair("instance::children_end", G(o, "Instance", "ChildrenEnd", strict));
            yield return Pair("instance::children_start", G(o, "Instance", "ChildrenStart", strict));
            yield return Pair("instance::class_base", G(o, "Instance", "ClassBase", strict));
            yield return Pair("instance::class_descriptor", G(o, "Instance", "ClassDescriptor", strict));
            yield return Pair("instance::class_name", G(o, "Instance", "ClassName", strict));
            yield return Pair("instance::name", G(o, "Instance", "NameContainer", strict, 0x70));
            yield return Pair("instance::parent", G(o, "Instance", "Parent", strict));
            yield return Pair("instance::current_camera", G(o, "Workspace", "CurrentCamera", strict));
            yield return Pair("instance::children_stride", "0x10");
            yield return Pair("gui_object::background_color3", G(o, "GuiObject", "BackgroundColor3", strict));
            yield return Pair("gui_object::border_color3", G(o, "GuiObject", "BorderColor3", strict));
            yield return Pair("gui_object::image", G(o, "GuiObject", "Image", strict));
            yield return Pair("gui_object::layout_order", G(o, "GuiObject", "LayoutOrder", strict));
            yield return Pair("gui_object::position", G(o, "GuiObject", "Position", strict));
            yield return Pair("gui_object::frame_position_x", G(o, "GuiObject", "Position", strict));
            yield return Pair("gui_object::frame_position_y", "0x518");
            yield return Pair("gui_object::rich_text", G(o, "GuiObject", "RichText", strict));
            yield return Pair("gui_object::rotation", G(o, "GuiObject", "Rotation", strict));
            yield return Pair("gui_object::screen_gui_enabled", G(o, "GuiObject", "ScreenGui_Enabled", strict));
            yield return Pair("gui_object::size", G(o, "GuiObject", "Size", strict));
            yield return Pair("gui_object::text", G(o, "GuiObject", "Text", strict));
            yield return Pair("gui_object::text_color3", G(o, "GuiObject", "TextColor3", strict));
            yield return Pair("gui_object::text_color3_fallback", G(o, "GuiObject", "TextColor3", strict));
            yield return Pair("gui_object::visible", G(o, "GuiObject", "Visible", strict));
            yield return Pair("mouse_service::input_object", G(o, "MouseService", "InputObject", strict));
            yield return Pair("mouse_service::mouse_position", G(o, "MouseService", "MousePosition", strict));
            yield return Pair("camera::position", G(o, "Camera", "Position", strict));
            yield return Pair("camera::rotation", G(o, "Camera", "Rotation", strict));
            yield return Pair("camera::subject", G(o, "Camera", "CameraSubject", strict));
            yield return Pair("camera::position_offset", "0x0");
            yield return Pair("camera::viewport", G(o, "Camera", "Viewport", strict));
            yield return Pair("camera::viewport_size", G(o, "Camera", "ViewportSize", strict));
            yield return Pair("chat::is_focused", "0x154");
            yield return Pair("lighting::ambient", G(o, "Lighting", "Ambient", strict));
            yield return Pair("lighting::brightness", G(o, "Lighting", "Brightness", strict));
            yield return Pair("lighting::colorshift_bottom", G(o, "Lighting", "ColorShift_Bottom", strict));
            yield return Pair("lighting::colorshift_top", G(o, "Lighting", "ColorShift_Top", strict));
            yield return Pair("lighting::exposure_compensation", G(o, "Lighting", "ExposureCompensation", strict));
            yield return Pair("lighting::fog_color", G(o, "Lighting", "FogColor", strict));
            yield return Pair("lighting::fog_end", G(o, "Lighting", "FogEnd", strict));
            yield return Pair("lighting::fog_start", G(o, "Lighting", "FogStart", strict));
            yield return Pair("lighting::geographic_latitude", G(o, "Lighting", "GeographicLatitude", strict));
            yield return Pair("lighting::outdoor_ambient", G(o, "Lighting", "OutdoorAmbient", strict));
            yield return Pair("sky::moon_angular_size", G(o, "Sky", "MoonAngularSize", strict));
            yield return Pair("sky::moon_texture_id", G(o, "Sky", "MoonTextureId", strict));
            yield return Pair("sky::skybox_bk", G(o, "Sky", "SkyboxBk", strict));
            yield return Pair("sky::skybox_dn", G(o, "Sky", "SkyboxDn", strict));
            yield return Pair("sky::skybox_ft", G(o, "Sky", "SkyboxFt", strict));
            yield return Pair("sky::skybox_lf", G(o, "Sky", "SkyboxLf", strict));
            yield return Pair("sky::skybox_orientation", G(o, "Sky", "SkyboxOrientation", strict));
            yield return Pair("sky::skybox_rt", G(o, "Sky", "SkyboxRt", strict));
            yield return Pair("sky::skybox_up", G(o, "Sky", "SkyboxUp", strict));
            yield return Pair("sky::star_count", G(o, "Sky", "StarCount", strict));
            yield return Pair("sky::sun_angular_size", G(o, "Sky", "SunAngularSize", strict));
            yield return Pair("sky::sun_texture_id", G(o, "Sky", "SunTextureId", strict));
            yield return Pair("mesh_part::mesh_id", G(o, "MeshPart", "MeshId", strict));
            yield return Pair("mesh_part::special_mesh_id", G(o, "SpecialMesh", "MeshId", strict));
            yield return Pair("team::team_color", G(o, "Team", "BrickColor", strict));
            yield return Pair("rbx_string::length", G(o, "Misc", "StringLength", strict, 0x10));
            yield return Pair("workspace::gravity", G(o, "World", "Gravity", strict));
            yield return Pair("workspace::gravity_container", G(o, "Workspace", "World", strict));
            yield return Pair("workspace::primitives_pointer1", G(o, "Workspace", "World", strict));
            yield return Pair("workspace::primitives_pointer2", G(o, "World", "Primitives", strict));
            yield return Pair("replicator::nextgen_replicator", "0x0");
            yield return Pair("fflags::target_time_delay_facctor_tenths", "0x0");
        }

        static KeyValuePair<string, string> Pair(string key, string value)
        {
            return new KeyValuePair<string, string>(key, value);
        }

        static string InstanceNameOffset(Dictionary<string, Dictionary<string, int>> o, bool strict)
        {
            Dictionary<string, int> instance;
            if (!o.TryGetValue("Instance", out instance) || instance == null)
                instance = null;

            int nameContainer = 0x70;
            int nameInner = 0x8;
            if (instance != null)
            {
                int container;
                if (instance.TryGetValue("NameContainer", out container) && container != 0)
                    nameContainer = container;

                int name;
                if (instance.TryGetValue("Name", out name) && name != 0)
                    nameInner = name;
            }

            return Hx(nameContainer + nameInner);
        }

        static string G(
            Dictionary<string, Dictionary<string, int>> o,
            string ns,
            string key,
            bool strict,
            int? fallback = null)
        {
            Dictionary<string, int> section;
            if (!o.TryGetValue(ns, out section))
                section = null;

            int value;
            if (section != null && section.TryGetValue(key, out value))
            {
                if (value != 0)
                    return Hx(value);

                if (fallback.HasValue)
                    return Hx(fallback.Value);

                return Hx(value);
            }

            if (fallback.HasValue)
                return Hx(fallback.Value);

            if (!strict)
                return "0x0";

            throw new KeyNotFoundException(ns + "." + key);
        }

        static string Hx(int value)
        {
            return "0x" + value.ToString("X");
        }
    }
}
