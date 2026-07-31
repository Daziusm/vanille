using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Net.Http;
using System.Text;
using System.Text.RegularExpressions;
using System.Web.Script.Serialization;

namespace Chocola
{
    /// Fetches offsets.imtheo.lol when Roblox client version changes.
    internal static class OffsetUpdater
    {
        static readonly HttpClient Http = new HttpClient
        {
            Timeout = TimeSpan.FromSeconds(30)
        };

        static readonly Regex VersionLine = new Regex(
            @"version-[a-f0-9]+",
            RegexOptions.IgnoreCase | RegexOptions.Compiled);

        public static bool TryRefresh(string installDir, Action<string> status, out string error)
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
            if (cached == version && File.Exists(valuesPath))
            {
                status?.Invoke("Offsets OK (" + version + ").");
                return true;
            }

            status?.Invoke("Fetching offsets for " + version + "...");
            try
            {
                var offsets = FetchOffsets(version);
                var text = BuildValuesText(version, offsets);
                Directory.CreateDirectory(installDir);
                File.WriteAllText(valuesPath, text, Encoding.UTF8);
                status?.Invoke("Offsets updated.");
                return true;
            }
            catch (Exception ex)
            {
                if (File.Exists(valuesPath))
                {
                    status?.Invoke("Offset fetch failed — using cached file.");
                    return true;
                }

                error = ex.Message;
                return false;
            }
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

        static Dictionary<string, Dictionary<string, int>> FetchOffsets(string versionFolder)
        {
            var url = "https://offsets.imtheo.lol/" + versionFolder + "/offsets.json";
            var json = Http.GetStringAsync(url).GetAwaiter().GetResult();
            var serializer = new JavaScriptSerializer { MaxJsonLength = int.MaxValue };
            var root = serializer.Deserialize<Dictionary<string, object>>(json);
            if (root == null || !root.ContainsKey("Offsets"))
                throw new InvalidDataException("Invalid offsets JSON.");

            var offsetsObj = root["Offsets"] as Dictionary<string, object>;
            if (offsetsObj == null)
                throw new InvalidDataException("Missing Offsets section.");

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

            return result;
        }

        static string BuildValuesText(string version, Dictionary<string, Dictionary<string, int>> o)
        {
            var lines = new List<string>
            {
                "$offsets = [",
                "    // " + version + " (imtheo)",
            };

            foreach (var entry in Entries(o))
                lines.Add("    '" + entry.Key + "' => '" + entry.Value + "',");

            lines.Add("];");
            return string.Join("\n", lines) + "\n";
        }

        static IEnumerable<KeyValuePair<string, string>> Entries(Dictionary<string, Dictionary<string, int>> o)
        {
            yield return Pair("task_scheduler::pointer", G(o, "TaskScheduler", "Pointer"));
            yield return Pair("task_scheduler::job_start", G(o, "TaskScheduler", "JobStart"));
            yield return Pair("task_scheduler::job_end", G(o, "TaskScheduler", "JobEnd"));
            yield return Pair("task_scheduler::job_name", G(o, "TaskScheduler", "JobName"));
            yield return Pair("task_scheduler::job_stride", "0x10");
            yield return Pair("task_scheduler::render_job_to_fake_datamodel", G(o, "RenderJob", "FakeDataModel"));
            yield return Pair("task_scheduler::fake_datamodel_to_datamodel", G(o, "FakeDataModel", "RealDataModel"));
            yield return Pair("task_scheduler::render_job_to_renderview", G(o, "VisualEngine", "RenderView"));
            yield return Pair("task_scheduler::max_fps", G(o, "TaskScheduler", "MaxFPS"));
            yield return Pair("task_scheduler::target_fps", "0x0");
            yield return Pair("datamodel::datamodel_ptr0", G(o, "FakeDataModel", "Pointer"));
            yield return Pair("datamodel::datamodel_ptr1", G(o, "FakeDataModel", "RealDataModel"));
            yield return Pair("datamodel::place_id", G(o, "DataModel", "PlaceId"));
            yield return Pair("visualengine::visualengine_ptr", G(o, "VisualEngine", "Pointer"));
            yield return Pair("visualengine::view_matrix", G(o, "VisualEngine", "ViewMatrix"));
            yield return Pair("visualengine::dimensions", G(o, "VisualEngine", "Dimensions"));
            yield return Pair("renderview::force_flag_byte", G(o, "RenderView", "LightingValid"));
            yield return Pair("renderview::force_flag_bool", G(o, "RenderView", "SkyValid"));
            yield return Pair("players::local_player", G(o, "Player", "LocalPlayer"));
            yield return Pair("player::display_name", G(o, "Player", "DisplayName"));
            yield return Pair("player::user_id", G(o, "Player", "UserId"));
            yield return Pair("player::team", G(o, "Player", "Team"));
            yield return Pair("player::team_color", G(o, "Player", "TeamColor"));
            yield return Pair("player::character", G(o, "Player", "ModelInstance"));
            yield return Pair("base_part::primitive", G(o, "BasePart", "Primitive"));
            yield return Pair("base_part::material", G(o, "Primitive", "Material", 0x23E));
            yield return Pair("base_part::transparency", G(o, "BasePart", "Transparency"));
            yield return Pair("base_part::color3", G(o, "BasePart", "Color3"));
            yield return Pair("base_part::size", G(o, "Primitive", "Size"));
            yield return Pair("base_part::position", G(o, "Primitive", "Position"));
            yield return Pair("base_part::primitive_properties", "0xA0");
            yield return Pair("base_part::primitive_position", "0x90");
            yield return Pair("base_part::validate", G(o, "Primitive", "Validate"));
            yield return Pair("base_part::cframe_rotation", G(o, "Primitive", "Rotation"));
            yield return Pair("base_part::assembly_linear_velocity", G(o, "Primitive", "AssemblyLinearVelocity"));
            yield return Pair("base_part::assembly_angular_velocity", G(o, "Primitive", "AssemblyAngularVelocity"));
            yield return Pair("base_part::can_collide", G(o, "PrimitiveFlags", "CanCollide"));
            yield return Pair("base_part::can_collide_mask", "0x8");
            yield return Pair("humanoid::humanoid_state_id", G(o, "Humanoid", "HumanoidStateID"));
            yield return Pair("humanoid::move_direction", G(o, "Humanoid", "MoveDirection"));
            yield return Pair("humanoid::floor_material", G(o, "Humanoid", "FloorMaterial"));
            yield return Pair("humanoid::health", G(o, "Humanoid", "Health"));
            yield return Pair("humanoid::hip_height", G(o, "Humanoid", "HipHeight"));
            yield return Pair("humanoid::jump_height", G(o, "Humanoid", "JumpHeight"));
            yield return Pair("humanoid::jump_power", G(o, "Humanoid", "JumpPower"));
            yield return Pair("humanoid::max_health", G(o, "Humanoid", "MaxHealth"));
            yield return Pair("humanoid::max_slope_angle", G(o, "Humanoid", "MaxSlopeAngle"));
            yield return Pair("humanoid::rig_type", G(o, "Humanoid", "RigType"));
            yield return Pair("humanoid::walk_speed", G(o, "Humanoid", "Walkspeed"));
            yield return Pair("humanoid::auto_rotate", G(o, "Humanoid", "AutoRotate"));
            yield return Pair("humanoid::jump", G(o, "Humanoid", "Jump"));
            yield return Pair("humanoid::humanoid_state", G(o, "Humanoid", "HumanoidState"));
            yield return Pair("humanoid::walk_speed_check", G(o, "Humanoid", "WalkspeedCheck"));
            yield return Pair("value_bool::value", G(o, "Misc", "Value"));
            yield return Pair("value_int::value", G(o, "Misc", "Value"));
            yield return Pair("value_number::value", G(o, "Misc", "Value"));
            yield return Pair("instance::attribute_container", "0x48");
            yield return Pair("instance::attribute_list", "0x18");
            yield return Pair("instance::attribute_to_next", G(o, "Attribute", "Size"));
            yield return Pair("instance::attribute_to_value", G(o, "Attribute", "Value"));
            yield return Pair("instance::children_end", G(o, "Instance", "ChildrenEnd"));
            yield return Pair("instance::children_start", G(o, "Instance", "ChildrenStart"));
            yield return Pair("instance::class_base", G(o, "Instance", "ClassBase"));
            yield return Pair("instance::class_descriptor", G(o, "Instance", "ClassDescriptor"));
            yield return Pair("instance::class_name", G(o, "Instance", "ClassName"));
            yield return Pair("instance::name", G(o, "Instance", "Name"));
            yield return Pair("instance::parent", G(o, "Instance", "Parent"));
            yield return Pair("instance::current_camera", G(o, "Workspace", "CurrentCamera"));
            yield return Pair("instance::children_stride", "0x10");
            yield return Pair("gui_object::background_color3", G(o, "GuiObject", "BackgroundColor3"));
            yield return Pair("gui_object::border_color3", G(o, "GuiObject", "BorderColor3"));
            yield return Pair("gui_object::image", G(o, "GuiObject", "Image"));
            yield return Pair("gui_object::layout_order", G(o, "GuiObject", "LayoutOrder"));
            yield return Pair("gui_object::position", G(o, "GuiObject", "Position"));
            yield return Pair("gui_object::frame_position_x", G(o, "GuiObject", "Position"));
            yield return Pair("gui_object::frame_position_y", "0x518");
            yield return Pair("gui_object::rich_text", G(o, "GuiObject", "RichText"));
            yield return Pair("gui_object::rotation", G(o, "GuiObject", "Rotation"));
            yield return Pair("gui_object::screen_gui_enabled", G(o, "GuiObject", "ScreenGui_Enabled"));
            yield return Pair("gui_object::size", G(o, "GuiObject", "Size"));
            yield return Pair("gui_object::text", G(o, "GuiObject", "Text"));
            yield return Pair("gui_object::text_color3", G(o, "GuiObject", "TextColor3"));
            yield return Pair("gui_object::text_color3_fallback", G(o, "GuiObject", "TextColor3"));
            yield return Pair("gui_object::visible", G(o, "GuiObject", "Visible"));
            yield return Pair("mouse_service::input_object", G(o, "MouseService", "InputObject"));
            yield return Pair("mouse_service::mouse_position", G(o, "MouseService", "MousePosition"));
            yield return Pair("camera::position", G(o, "Camera", "Position"));
            yield return Pair("camera::rotation", G(o, "Camera", "Rotation"));
            yield return Pair("camera::subject", G(o, "Camera", "CameraSubject"));
            yield return Pair("camera::position_offset", "0x0");
            yield return Pair("camera::viewport", G(o, "Camera", "Viewport"));
            yield return Pair("camera::viewport_size", G(o, "Camera", "ViewportSize"));
            yield return Pair("chat::is_focused", "0x154");
            yield return Pair("lighting::ambient", G(o, "Lighting", "Ambient"));
            yield return Pair("lighting::brightness", G(o, "Lighting", "Brightness"));
            yield return Pair("lighting::colorshift_bottom", G(o, "Lighting", "ColorShift_Bottom"));
            yield return Pair("lighting::colorshift_top", G(o, "Lighting", "ColorShift_Top"));
            yield return Pair("lighting::exposure_compensation", G(o, "Lighting", "ExposureCompensation"));
            yield return Pair("lighting::fog_color", G(o, "Lighting", "FogColor"));
            yield return Pair("lighting::fog_end", G(o, "Lighting", "FogEnd"));
            yield return Pair("lighting::fog_start", G(o, "Lighting", "FogStart"));
            yield return Pair("lighting::geographic_latitude", G(o, "Lighting", "GeographicLatitude"));
            yield return Pair("lighting::outdoor_ambient", G(o, "Lighting", "OutdoorAmbient"));
            yield return Pair("lighting::sky", G(o, "Lighting", "Sky"));
            yield return Pair("sky::moon_angular_size", G(o, "Sky", "MoonAngularSize"));
            yield return Pair("sky::moon_texture_id", G(o, "Sky", "MoonTextureId"));
            yield return Pair("sky::skybox_bk", G(o, "Sky", "SkyboxBk"));
            yield return Pair("sky::skybox_dn", G(o, "Sky", "SkyboxDn"));
            yield return Pair("sky::skybox_ft", G(o, "Sky", "SkyboxFt"));
            yield return Pair("sky::skybox_lf", G(o, "Sky", "SkyboxLf"));
            yield return Pair("sky::skybox_orientation", G(o, "Sky", "SkyboxOrientation"));
            yield return Pair("sky::skybox_rt", G(o, "Sky", "SkyboxRt"));
            yield return Pair("sky::skybox_up", G(o, "Sky", "SkyboxUp"));
            yield return Pair("sky::star_count", G(o, "Sky", "StarCount"));
            yield return Pair("sky::sun_angular_size", G(o, "Sky", "SunAngularSize"));
            yield return Pair("sky::sun_texture_id", G(o, "Sky", "SunTextureId"));
            yield return Pair("mesh_part::mesh_id", G(o, "MeshPart", "MeshId"));
            yield return Pair("mesh_part::special_mesh_id", G(o, "SpecialMesh", "MeshId"));
            yield return Pair("team::team_color", G(o, "Team", "BrickColor"));
            yield return Pair("rbx_string::length", G(o, "Misc", "StringLength"));
            yield return Pair("workspace::gravity", G(o, "World", "Gravity"));
            yield return Pair("workspace::gravity_container", G(o, "Workspace", "World"));
            yield return Pair("workspace::primitives_pointer1", G(o, "Workspace", "World"));
            yield return Pair("workspace::primitives_pointer2", G(o, "World", "Primitives"));
            yield return Pair("replicator::nextgen_replicator", "0x0");
            yield return Pair("fflags::target_time_delay_facctor_tenths", "0x0");
        }

        static KeyValuePair<string, string> Pair(string key, string value)
        {
            return new KeyValuePair<string, string>(key, value);
        }

        static string G(Dictionary<string, Dictionary<string, int>> o, string ns, string key, int? fallback = null)
        {
            Dictionary<string, int> section;
            if (!o.TryGetValue(ns, out section))
                section = null;

            int value;
            if (section != null && section.TryGetValue(key, out value) && value != 0)
                return Hx(value);

            if (fallback.HasValue)
                return Hx(fallback.Value);

            if (section != null && section.TryGetValue(key, out value))
                return Hx(value);

            throw new KeyNotFoundException(ns + "." + key);
        }

        static string Hx(int value)
        {
            return "0x" + value.ToString("X");
        }
    }
}
