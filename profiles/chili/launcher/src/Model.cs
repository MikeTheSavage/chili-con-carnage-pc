// What the launcher knows: the settings of ChiliNative.ini with their defaults, the launcher's own
// launcher.ini, the game program beside it, the disc and the mods folder.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;

namespace ChiliLauncher
{
    public sealed class Setting
    {
        public readonly string Section, Key;
        /// <summary>Text the game program holds when it knows the key; null when every build does.</summary>
        public readonly string Probe;
        public string Default;
        public Func<Model, string> DefaultFn;
        public bool Raw;
        public Setting(string section, string key, string def, string probe = null)
        {
            Section = section; Key = key; Default = def; Probe = probe;
        }
        public string Id { get { return Section + "/" + Key; } }
    }

    /// <summary>The keys of ChiliNative.ini (RENDERING.md, CONTROLS.md, AUDIO.md, mods/README.md).</summary>
    public static class S
    {
        public static readonly List<Setting> All = new List<Setting>();
        static Setting Add(string section, string key, string def, string probe = null)
        {
            var s = new Setting(section, key, def, probe);
            All.Add(s);
            return s;
        }

        // [Rendering]
        public static readonly Setting Fullscreen = Add("Rendering", "Fullscreen", "false", "Fullscreen must be");
        public static readonly Setting WindowSize = Add("Rendering", "WindowSize", "", "WindowSize");
        public static readonly Setting InternalScale = Add("Rendering", "InternalScale", "auto", "InternalScale must be");
        public static readonly Setting EarlyPresent = Add("Rendering", "EarlyPresent", "true", "EarlyPresent");
        public static readonly Setting Preset = Add("Rendering", "Preset", "high", "Preset must be");
        public static readonly Setting AntiAliasing = Add("Rendering", "AntiAliasing", "fxaa", "AntiAliasing must be");
        public static readonly Setting Msaa = Add("Rendering", "MSAA", "1", "MSAA must be");
        public static readonly Setting TextureFilter = Add("Rendering", "TextureFilter", "game", "TextureFilter must be");
        public static readonly Setting Anisotropy = Add("Rendering", "Anisotropy", "16", "Anisotropy must be");
        public static readonly Setting Ssao = Add("Rendering", "SSAO", "off", "SSAO must be");
        public static readonly Setting Glow = Add("Rendering", "Glow", "smooth", "Glow must be");
        public static readonly Setting Backend = Add("Rendering", "Backend", "d3d12", "Backend must be");
        public static readonly Setting GeThread = Add("Rendering", "GeThread", "true", "GeThread");
        // [Game]
        public static readonly Setting FrameLock = Add("Game", "FrameLock", "1", "FrameLock must be");
        public static readonly Setting Camera = Add("Game", "Camera", "tight", "Camera must be");
        public static readonly Setting Priority = Add("Game", "Priority", "high", "Priority must be");
        // [Audio]
        public static readonly Setting BufferMs = Add("Audio", "BufferMs", "70", "BufferMs");
        // [Mods]
        public static readonly Setting ModsFolder = Add("Mods", "Folder", "mods");
        public static readonly Setting ModsEnabled = Add("Mods", "Enabled", null);
        public static readonly Setting DumpTextures = Add("Mods", "DumpTextures", "false", "DumpTextures");
        public static readonly Setting LogFiles = Add("Mods", "LogFiles", "false", "LogFiles");
        // [Disc]
        public static readonly Setting DiscPath = Add("Disc", "Path", "", "[Disc]");
        // [Input]
        // The control style: tod_pc (Total Overdose PC) | modern | psp (PSP original). Scheme goes with it.
        public static readonly Setting Layout = Add("Input", "Layout", "tod_pc", "Layout must be");
        public static readonly Setting Scheme = Add("Input", "Scheme", "dual_analog");
        public static readonly Setting ExactTurn = Add("Input", "ExactTurn", "true", "ExactTurn");
        public static readonly Setting BackgroundInput = Add("Input", "BackgroundInput", "false", "BackgroundInput");
        public static readonly Setting ShowInput = Add("Input", "ShowInput", "false", "ShowInput");
        public static readonly Setting InputVersion = Add("Input", "Version", "3");
        // What the game draws for its buttons on screen: auto | keyboard | gamepad | psp (CONTROLS.md, "On-screen prompts").
        public static readonly Setting Prompts = Add("Input", "Prompts", "auto", "Prompts must be");
        // [Input.Keyboard]
        public static readonly Setting WalkScale = Add("Input.Keyboard", "WalkScale", "0.6", "WalkScale");
        public static readonly Setting LookKeySpeed = Add("Input.Keyboard", "LookKeySpeed", "0.7", "LookKeySpeed");
        // [Input.Mouse]
        public static readonly Setting MouseEnabled = Add("Input.Mouse", "Enabled", "true");
        public static readonly Setting MouseCapture = Add("Input.Mouse", "Capture", "click");
        public static readonly Setting MenuClicks = Add("Input.Mouse", "MenuClicks", "true", "MenuClicks");
        public static readonly Setting SensitivityX = Add("Input.Mouse", "SensitivityX", "0.08");
        public static readonly Setting SensitivityY = Add("Input.Mouse", "SensitivityY", "0.08", "SensitivityY");
        public static readonly Setting MouseInvertX = Add("Input.Mouse", "InvertX", "false");
        public static readonly Setting MouseInvertY = Add("Input.Mouse", "InvertY", "false");
        public static readonly Setting HoldPitch = Add("Input.Mouse", "HoldPitch", "true", "HoldPitch");
        public static readonly Setting Exact = Add("Input.Mouse", "Exact", "true", "Exact");
        public static readonly Setting Smoothing = Add("Input.Mouse", "Smoothing", "0", "Smoothing");
        // [Input.Gamepad]
        public static readonly Setting PadEnabled = Add("Input.Gamepad", "Enabled", "true");
        public static readonly Setting PadIndex = Add("Input.Gamepad", "Index", "auto");
        public static readonly Setting DeadzoneLeft = Add("Input.Gamepad", "DeadzoneLeft", "0.24");
        public static readonly Setting DeadzoneRight = Add("Input.Gamepad", "DeadzoneRight", "0.24");
        public static readonly Setting TriggerThreshold = Add("Input.Gamepad", "TriggerThreshold", "0.12");
        public static readonly Setting CompensateDeadzone = Add("Input.Gamepad", "CompensateEngineDeadzone", "true", "CompensateEngineDeadzone");
        public static readonly Setting TurnSensitivity = Add("Input.Gamepad", "TurnSensitivity", "1.0", "TurnSensitivity");
        public static readonly Setting LookSensitivity = Add("Input.Gamepad", "LookSensitivity", "1.0", "LookSensitivity");
        public static readonly Setting PadInvertY = Add("Input.Gamepad", "InvertY", "false");
        // auto | xbox | playstation: how a controller's buttons are drawn, in the game's prompts and in the launcher.
        public static readonly Setting PadGlyphs = Add("Input.Gamepad", "Glyphs", "auto", "Glyphs must be");
        // true | false: whether PlayStation controllers are read (through Raw Input) beside the Xbox kind.
        public static readonly Setting PlayStationPads = Add("Input.Gamepad", "PlayStationPads", "true", "PlayStationPads");
        // 0 to 1: how strong the controller shakes; 0 is off.
        public static readonly Setting Rumble = Add("Input.Gamepad", "Rumble", "1", "Rumble");

        public static readonly string[] MouseInputs = { "Mouse1", "Mouse2", "Mouse3", "Mouse4", "Mouse5", "WheelUp", "WheelDown" };
        public static readonly string[] PadButtons = { "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "LS", "RS", "Back", "Start", "DpadUp", "DpadDown", "DpadLeft", "DpadRight" };

        static readonly Dictionary<string, Setting> bindings = new Dictionary<string, Setting>(StringComparer.OrdinalIgnoreCase);
        static Setting Binding(string section, string key, Func<Model, string> def)
        {
            Setting s;
            string id = section + "/" + key;
            if (!bindings.TryGetValue(id, out s))
            {
                s = new Setting(section, key, "") { DefaultFn = def };
                bindings[id] = s;
                All.Add(s);
            }
            return s;
        }
        /// <summary>[Input.Keyboard] Target=key[,key]</summary>
        public static Setting Keys(string target) { return Binding("Input.Keyboard", target, m => Defaults.Keyboard(m.Layout, target)); }
        /// <summary>[Input.Mouse] Mouse1..5, WheelUp, WheelDown = Target</summary>
        public static Setting Mouse(string input) { return Binding("Input.Mouse", input, m => Defaults.Mouse(input)); }
        /// <summary>[Input.Gamepad] Button=Target</summary>
        public static Setting Pad(string button) { return Binding("Input.Gamepad", button, m => Defaults.Pad(m.Layout, button)); }
        /// <summary>Every binding key, in the order of S.All (the game's settings table has them in this order too).</summary>
        public static IEnumerable<Setting> Bindings()
        {
            foreach (var a in Actions.All) yield return Keys(a.Target);
            foreach (var i in MouseInputs) yield return Mouse(i);
            foreach (var b in PadButtons) yield return Pad(b);
        }

        static S()
        {
            Camera.DefaultFn = m => m.Scheme == "psp" ? "original" : "tight";
            Layout.DefaultFn = m => m.Scheme == "psp" ? "psp" : "tod_pc";
            AntiAliasing.DefaultFn = m => m.Game.Knows(AntiAliasing.Probe) ? "fxaa" : "off";
            Anisotropy.DefaultFn = m => m.Game.Knows(Anisotropy.Probe) ? "16" : "1";
            DiscPath.Raw = true;
            foreach (var a in Actions.All) Keys(a.Target);
            foreach (var i in MouseInputs) Mouse(i);
            foreach (var b in PadButtons) Pad(b);
        }
    }

    public sealed class ActionDef
    {
        public string Target, Label, LabelPsp;
        public bool Toggle;        // one key, keyboard only (ToggleMouseCapture, ToggleInputDisplay)
        public bool Look;          // the look keys: not used by the psp scheme
        public string Stick;       // what the gamepad does without a button (dual_analog)
        public string Name(string scheme) { return scheme == "psp" && LabelPsp != null ? LabelPsp : Label; }
    }

    public static class Actions
    {
        public static readonly ActionDef[] All =
        {
            new ActionDef { Target = "MoveForward", Label = "Move forward", Stick = "Left stick" },
            new ActionDef { Target = "MoveBack", Label = "Move back", Stick = "Left stick" },
            new ActionDef { Target = "MoveLeft", Label = "Move left", LabelPsp = "Turn left", Stick = "Left stick" },
            new ActionDef { Target = "MoveRight", Label = "Move right", LabelPsp = "Turn right", Stick = "Left stick" },
            new ActionDef { Target = "Walk", Label = "Walk" },
            new ActionDef { Target = "Square", Label = "Shoot" },
            new ActionDef { Target = "R", Label = "Headshot mode" },
            new ActionDef { Target = "Cross", Label = "Jump / dive / use" },
            new ActionDef { Target = "Triangle", Label = "Reload" },
            new ActionDef { Target = "Circle", Label = "Weapon wheel / back" },
            new ActionDef { Target = "L", Label = "Target objects / exit stunt", LabelPsp = "Strafe / target" },
            new ActionDef { Target = "DpadLeft", Label = "Previous weapon" },
            new ActionDef { Target = "DpadRight", Label = "Next weapon" },
            new ActionDef { Target = "DpadDown", Label = "Rewind" },
            new ActionDef { Target = "DpadUp", Label = "Menu up" },
            new ActionDef { Target = "Start", Label = "Pause" },
            new ActionDef { Target = "Select", Label = "Select" },
            new ActionDef { Target = "LookLeft", Label = "Look left", Look = true, Stick = "Right stick" },
            new ActionDef { Target = "LookRight", Label = "Look right", Look = true, Stick = "Right stick" },
            new ActionDef { Target = "LookUp", Label = "Look up", Look = true, Stick = "Right stick" },
            new ActionDef { Target = "LookDown", Label = "Look down", Look = true, Stick = "Right stick" },
            new ActionDef { Target = "ToggleMouseCapture", Label = "Free the mouse", Toggle = true },
            new ActionDef { Target = "ToggleInputDisplay", Label = "Input test line", Toggle = true },
        };
        public static ActionDef Find(string target)
        {
            foreach (var a in All) if (string.Equals(a.Target, target, StringComparison.OrdinalIgnoreCase)) return a;
            return null;
        }
    }

    /// <summary>
    /// The game's own first-run bindings of each control style (host/pc_input.hpp, default_input_ini;
    /// host/input_layout.hpp): tod_pc (Total Overdose PC, the default), modern (the port's own before
    /// tod_pc), psp (PSP original: Modern's keys without the look keys, the PSP's pad table).
    /// </summary>
    public static class Defaults
    {
        public static readonly string[] Layouts = { "tod_pc", "modern", "psp" };
        /// <summary>What the launcher and the game's page call a style.</summary>
        public static string LayoutName(string layout) { return layout == "modern" ? "Modern" : layout == "psp" ? "PSP original" : "Total Overdose PC"; }
        // Total Overdose PC: W A S D, the mouse, Space dive, Left Shift jump, F use, R reload, Left Ctrl the
        // loco move, Tab rewind, Escape menu, P pause, Page Up and Down the forks, as Total Overdose had them.
        static readonly Dictionary<string, string> keyboardTodPc = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
        {
            { "MoveForward", "W" }, { "MoveBack", "S" }, { "MoveLeft", "A" }, { "MoveRight", "D" },
            { "LookLeft", "J" }, { "LookRight", "L" }, { "LookUp", "I" }, { "LookDown", "K" },
            { "Walk", "X" }, { "Cross", "Space,F,Enter,LShift" }, { "Circle", "E,Backspace" }, { "Square", "G" },
            { "Triangle", "R,LCtrl" }, { "L", "Q,C" }, { "R", "V" }, { "DpadUp", "Up,PageUp" }, { "DpadDown", "Tab,Down,PageDown" },
            { "DpadLeft", "Left" }, { "DpadRight", "Right" }, { "Start", "Escape,P" }, { "Select", "Grave" },
            { "ToggleMouseCapture", "F1" }, { "ToggleInputDisplay", "F3" },
        };
        static readonly Dictionary<string, string> keyboardModern = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
        {
            { "MoveForward", "W" }, { "MoveBack", "S" }, { "MoveLeft", "A" }, { "MoveRight", "D" },
            { "LookLeft", "J" }, { "LookRight", "L" }, { "LookUp", "I" }, { "LookDown", "K" },
            { "Walk", "X" }, { "Cross", "Space,E,Enter" }, { "Circle", "F,Backspace" }, { "Square", "G" },
            { "Triangle", "R" }, { "L", "Q" }, { "R", "V" }, { "DpadUp", "Up" }, { "DpadDown", "Down,Z" },
            { "DpadLeft", "Left,1" }, { "DpadRight", "Right,2" }, { "Start", "Escape" }, { "Select", "Tab" },
            { "ToggleMouseCapture", "F1" }, { "ToggleInputDisplay", "F3" },
        };
        static readonly Dictionary<string, string> mouse = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
        {
            { "Mouse1", "Square" }, { "Mouse2", "R" }, { "Mouse3", "Circle" }, { "Mouse4", "None" }, { "Mouse5", "None" },
            { "WheelUp", "DpadRight" }, { "WheelDown", "DpadLeft" },
        };
        static readonly Dictionary<string, string> padDual = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
        {
            { "A", "Cross" }, { "B", "Circle" }, { "X", "Triangle" }, { "Y", "Cross" }, { "LB", "DpadLeft" }, { "RB", "DpadRight" },
            { "LT", "R" }, { "RT", "Square" }, { "LS", "L" }, { "RS", "None" }, { "Back", "Select" }, { "Start", "Start" },
            { "DpadUp", "DpadUp" }, { "DpadDown", "DpadDown" }, { "DpadLeft", "DpadLeft" }, { "DpadRight", "DpadRight" },
        };
        static readonly Dictionary<string, string> padPsp = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
        {
            { "A", "Cross" }, { "B", "Circle" }, { "X", "Square" }, { "Y", "Triangle" }, { "LB", "L" }, { "RB", "R" },
            { "LT", "R" }, { "RT", "Square" }, { "LS", "None" }, { "RS", "None" }, { "Back", "Select" }, { "Start", "Start" },
            { "DpadUp", "DpadUp" }, { "DpadDown", "DpadDown" }, { "DpadLeft", "DpadLeft" }, { "DpadRight", "DpadRight" },
        };
        public static string Keyboard(string layout, string target)
        {
            if (layout == "psp" && target.StartsWith("Look", StringComparison.OrdinalIgnoreCase)) return "";
            string v;
            return (layout == "modern" || layout == "psp" ? keyboardModern : keyboardTodPc).TryGetValue(target, out v) ? v : "";
        }
        public static string Mouse(string input) { string v; return mouse.TryGetValue(input, out v) ? v : "None"; }
        public static string Pad(string layout, string button)
        {
            string v;
            return (layout == "psp" ? padPsp : padDual).TryGetValue(button, out v) ? v : "None";
        }
        /// <summary>The default of a binding key with a control style.</summary>
        public static string Of(Setting binding, string layout)
        {
            if (binding.Section == "Input.Keyboard") return Keyboard(layout, binding.Key);
            if (binding.Section == "Input.Mouse") return Mouse(binding.Key);
            return Pad(layout, binding.Key);
        }
        /// <summary>
        /// Whether two values of a binding key name the same inputs: for the keys of an action the same key
        /// names (Return is Enter, Esc is Escape; without case, in any order, a name twice counts once), for a
        /// mouse input or a pad button the same target (nothing is None). host/settings_table.hpp,
        /// binding_equivalent.
        /// </summary>
        public static bool SameBinding(Setting binding, string a, string b)
        {
            if (binding.Section == "Input.Keyboard")
            {
                Func<string, List<string>> names = text => (text ?? "").Split(',').Select(k => KeyNames.Canon(k).ToLowerInvariant())
                    .Where(k => k.Length > 0).Distinct().OrderBy(k => k, StringComparer.Ordinal).ToList();
                return names(a).SequenceEqual(names(b));
            }
            Func<string, string> target = text => { string t = (text ?? "").Trim().ToLowerInvariant(); return t.Length == 0 ? "none" : t; };
            return target(a) == target(b);
        }
        /// <summary>
        /// The binding lines of a file that move from control style `from` to `to`: those that hold `from`'s
        /// default take `to`'s; a line the player changed stays, and a binding the file leaves out follows the
        /// style by itself. host/pc_input.hpp, PcInputConfig::layout_binding_changes.
        /// </summary>
        public static List<KeyValuePair<Setting, string>> BindingChanges(Func<Setting, string> held, string from, string to)
        {
            var changes = new List<KeyValuePair<Setting, string>>();
            if (from == to) return changes;
            foreach (var s in S.Bindings())
            {
                string value = held(s);
                if (value == null || !SameBinding(s, value, Of(s, from))) continue;
                string wanted = Of(s, to);
                if (!SameBinding(s, value, wanted)) changes.Add(new KeyValuePair<Setting, string>(s, wanted));
            }
            return changes;
        }
    }

    /// <summary>
    /// A settings file the game wrote with [Input] Version 2 (the builds before the control styles, whose defaults were
    /// Modern with dual_analog and PSP original with psp), brought up to date as the game does at its start
    /// (host/pc_input.hpp, PcInputConfig::brought_up_to_date), to the same bytes: when every binding line holds
    /// that default and the scheme is dual_analog, the bindings take Total Overdose PC's; with psp they stay; a
    /// file with any binding of the player's own keeps every binding. Then [Input] Layout names the style and
    /// Version is 3. Every other line stays, but for the comment lines below. An older file (no Version, or 1)
    /// is left to the game, which gives it the current defaults.
    /// </summary>
    public static class InputMigration
    {
        public sealed class InputComment
        {
            public readonly string Section, Before, After;
            public readonly bool Always;      // in every file brought up to date; else only where the bindings take Total Overdose PC's
            public InputComment(string section, string before, string after, bool always) { Section = section; Before = before; After = after; Always = always; }
        }
        /// <summary>host/pc_input.hpp, input_migration_comments (tools/settings_table_test.cpp holds the two alike).</summary>
        public static readonly InputComment[] Comments =
        {
            new InputComment("Input", "; Written by the game. A file of the game's without it comes from an older build: its input sections are", "; Written by the game. A file of the game's with a lower number or none comes from an older build: once,", true),
            new InputComment("Input", ";   replaced by the current defaults once and the old file is kept as ChiliNative.ini.bak.", ";   its input settings are brought up to date, and the old file is kept as ChiliNative.ini.bak.", true),
            new InputComment("Input", ";   Delete this file after changing the scheme to get that scheme's default bindings.", ";   The control style sets it: dual_analog with tod_pc and modern, psp with psp.", true),
            new InputComment("Input.Keyboard", "; Space jumps and dives, E uses: the game has both on Cross.", "; Jump, dive, use, enter a vehicle, confirm: the game has them all on Cross.", false),
            new InputComment("Input.Keyboard", "; Reload.", "; Reload and the loco move: the game has both on Triangle.", false),
        };
        public const int Version = 3;

        /// <summary>Whether the game wrote the file: it starts with the game's first line.</summary>
        public static bool WrittenByGame(byte[] bytes)
        {
            var marker = System.Text.Encoding.ASCII.GetBytes("; Chili Con Carnage PC input.");
            int at = bytes.Length >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF ? 3 : 0;
            if (bytes.Length - at < marker.Length) return false;
            for (int i = 0; i < marker.Length; i++) if (bytes[at + i] != marker[i]) return false;
            return true;
        }
        /// <summary>Whether this is a file the launcher brings up to date: the game's, with [Input] Version=2.</summary>
        public static bool Wanted(byte[] bytes, IniDocument doc)
        {
            return WrittenByGame(bytes) && (doc.Get("Input", "Version") ?? "").Trim() == "2";
        }
        /// <summary>
        /// Brings the document up to date (Wanted must hold). Returns the style it now names; `changed` is how
        /// many binding lines (and lines of an [Input.Gamepad.PlayStation] section) are the player's own.
        /// </summary>
        public static string Apply(IniDocument doc, out int changed)
        {
            bool psp = (doc.Get("Input", "Scheme") ?? "").Trim().Equals("psp", StringComparison.OrdinalIgnoreCase);
            string was = psp ? "psp" : "modern";
            changed = 0;
            foreach (var s in S.Bindings())
            {
                string held = doc.Get(s.Section, s.Key);
                if (held != null && !Defaults.SameBinding(s, held, Defaults.Of(s, was))) changed++;
            }
            foreach (var line in doc.LiveLines()) if (string.Equals(line.Section, "Input.Gamepad.PlayStation", StringComparison.OrdinalIgnoreCase)) changed++;
            bool toTod = changed == 0 && !psp;
            foreach (var line in doc.Lines)
            {
                if (line.Kind != IniLineKind.Comment || line.Key != null) continue;
                foreach (var c in Comments)
                    if ((c.Always || toTod) && string.Equals(line.Section, c.Section, StringComparison.OrdinalIgnoreCase) && line.Text == c.Before) { line.Text = c.After; break; }
            }
            if (toTod)
                foreach (var change in Defaults.BindingChanges(s => doc.Get(s.Section, s.Key), was, "tod_pc")) doc.Set(change.Key.Section, change.Key.Key, change.Value);
            string now = toTod ? "tod_pc" : was;
            doc.Set("Input", "Layout", now);
            doc.Set("Input", "Version", Version.ToString(CultureInfo.InvariantCulture));
            return now;
        }
    }

    /// <summary>launcher.ini beside the launcher: which program Play starts, and the launcher's own switches.</summary>
    public sealed class LauncherConfig
    {
        public string File;
        IniDocument doc = new IniDocument();
        public const string DefaultText =
            "; Chili Con Carnage launcher. Delete this file to get the defaults back.\r\n" +
            "\r\n" +
            "[Launcher]\r\n" +
            "; The game program, beside the launcher unless a full path is given.\r\n" +
            "Game=ChiliConCarnage.exe\r\n" +
            "; true: start the game at once next time, without this window. Hold Shift while starting, or\r\n" +
            "; start the launcher with --settings, to see the settings again.\r\n" +
            "StartGameDirectly=false\r\n" +
            "; When the game ends: return (show the settings again) or close.\r\n" +
            "AfterGame=return\r\n" +
            "\r\n" +
            "; For developers. The folder of ChiliNative.ini when it is not the launcher's own:\r\n" +
            "; IniFolder=bin\r\n" +
            "; Arguments for the game; {boot} {disc} {saves} {ini} are filled in. With arguments the game's\r\n" +
            "; output is written to last_run.log in the logs folder.\r\n" +
            "; Arguments=\"{boot}\" \"{disc}\" --ms0 \"{saves}\"\r\n" +
            "; The disc for {disc} and {boot} when ChiliNative.ini names none, the save folder, the log folder:\r\n" +
            "; Disc=C:\\path\\to\\disc0\r\n" +
            "; Saves=saves\r\n" +
            "; Logs=logs\r\n";

        public static LauncherConfig Load(string baseDir)
        {
            var c = new LauncherConfig { File = Path.Combine(baseDir, "launcher.ini") };
            try { if (System.IO.File.Exists(c.File)) c.doc = IniDocument.Load(c.File); else c.doc = IniDocument.FromText(DefaultText); }
            catch { c.doc = IniDocument.FromText(DefaultText); }
            return c;
        }
        readonly object gate = new object();
        public string Get(string key, string def = "", string section = "Launcher")
        {
            lock (gate)
            {
                string v = doc.GetRaw(section, key);
                return string.IsNullOrEmpty(v) ? def : v;
            }
        }
        public bool GetBool(string key, bool def)
        {
            lock (gate)
            {
                string v = doc.Get("Launcher", key);
                return v == null ? def : Model.IsTrue(v);
            }
        }
        public void Set(string key, string value, string section = "Launcher") { lock (gate) doc.Set(section, key, value, true); }
        /// <summary>Writes the file when it differs from what is on disk.</summary>
        public bool Save()
        {
            lock (gate)
            {
                try
                {
                    byte[] bytes = doc.ToBytes();
                    if (System.IO.File.Exists(File) && System.IO.File.ReadAllBytes(File).SequenceEqual(bytes)) return true;
                    System.IO.File.WriteAllBytes(File, bytes);
                    return true;
                }
                catch { return false; }
            }
        }
    }

    /// <summary>The game program: where it is, its version, and which settings it understands.</summary>
    public sealed class GameInfo
    {
        public string ExePath = "";
        public bool Exists;
        public string Version = "";
        public bool Probed;
        readonly HashSet<string> known = new HashSet<string>();

        /// <summary>False only when the program was read and does not hold the text.</summary>
        public bool Knows(string probe) { return probe == null || !Probed || known.Contains(probe); }

        public static string Find(string baseDir, LauncherConfig config)
        {
            string named = config.Get("Game", "");
            var tries = new List<string>();
            if (named.Length > 0) tries.Add(named);
            tries.Add("ChiliConCarnage.exe");
            tries.Add("chili_recomp.exe");
            tries.Add(@"bin\chili_recomp.exe");
            foreach (var t in tries)
            {
                try
                {
                    string p = Path.IsPathRooted(t) ? t : Path.Combine(baseDir, t);
                    if (System.IO.File.Exists(p)) return Path.GetFullPath(p);
                }
                catch { }
            }
            string first = tries[0];
            try { return Path.IsPathRooted(first) ? first : Path.GetFullPath(Path.Combine(baseDir, first)); }
            catch { return first; }
        }

        /// <summary>Tests: a game that knows these texts and no others.</summary>
        public static GameInfo Knowing(string exePath, IEnumerable<string> probes)
        {
            var info = new GameInfo { ExePath = exePath, Exists = true, Probed = true };
            foreach (var p in probes) info.known.Add(p);
            return info;
        }
        /// <summary>The texts the program was found to hold (tests).</summary>
        public IEnumerable<string> Known { get { return known; } }

        static string Stamp(FileInfo file, string exePath, int probes)
        {
            return file.Length + ":" + file.LastWriteTimeUtc.Ticks + ":" + probes + ":" + exePath.ToLowerInvariant().GetHashCode();
        }
        static List<string> Probes() { return S.All.Where(s => s.Probe != null).Select(s => s.Probe).Distinct().ToList(); }

        /// <summary>
        /// What launcher.ini remembers of this program from the last time it was read, without reading it
        /// again: at once, so that the rows a game build lacks do not show for a moment at every start.
        /// A program not seen before (or changed since) is not probed here: Inspect does that.
        /// </summary>
        public static GameInfo Remembered(string exePath, LauncherConfig cache)
        {
            var info = new GameInfo { ExePath = exePath };
            try
            {
                var file = new FileInfo(exePath);
                info.Exists = file.Exists;
                if (!info.Exists || cache == null) return info;
                if (cache.Get("Stamp", "", "GameProbe") != Stamp(file, exePath, Probes().Count)) return info;
                foreach (var p in cache.Get("Known", "", "GameProbe").Split('|')) if (p.Length > 0) info.known.Add(p);
                info.Probed = true;
            }
            catch { }
            return info;
        }

        public static GameInfo Inspect(string exePath, LauncherConfig cache)
        {
            var info = new GameInfo { ExePath = exePath };
            try
            {
                var file = new FileInfo(exePath);
                info.Exists = file.Exists;
                if (!info.Exists) return info;
                try
                {
                    var v = FileVersionInfo.GetVersionInfo(exePath);
                    info.Version = (v.ProductVersion ?? v.FileVersion ?? "").Trim();
                    if (info.Version.Replace("0", "").Replace(".", "").Length == 0) info.Version = "";
                }
                catch { }
                var probes = Probes();
                string stamp = Stamp(file, exePath, probes.Count);
                if (cache != null && cache.Get("Stamp", "", "GameProbe") == stamp)
                {
                    foreach (var p in cache.Get("Known", "", "GameProbe").Split('|')) if (p.Length > 0) info.known.Add(p);
                    info.Probed = true;
                    return info;
                }
                foreach (var p in Scan(System.IO.File.ReadAllBytes(exePath), probes)) info.known.Add(p);
                info.Probed = true;
                if (cache != null)
                {
                    cache.Set("Stamp", stamp, "GameProbe");
                    cache.Set("Known", string.Join("|", info.known), "GameProbe");
                    cache.Save();
                }
            }
            catch { }
            return info;
        }

        static bool IsWordByte(byte b) { return (b >= '0' && b <= '9') || (b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') || b == '_'; }

        /// <summary>The probes found in the bytes as whole words.</summary>
        public static List<string> Scan(byte[] data, List<string> probes)
        {
            var byFirst = new List<byte[]>[256];
            var names = new Dictionary<byte[], string>();
            foreach (var p in probes)
            {
                var b = Encoding.ASCII.GetBytes(p);
                if (b.Length < 2) continue;
                if (byFirst[b[0]] == null) byFirst[b[0]] = new List<byte[]>();
                byFirst[b[0]].Add(b);
                names[b] = p;
            }
            var found = new HashSet<string>();
            int n = data.Length;
            for (int i = 0; i < n - 1; i++)
            {
                var list = byFirst[data[i]];
                if (list == null) continue;
                for (int k = 0; k < list.Count; k++)
                {
                    var b = list[k];
                    if (data[i + 1] != b[1] || i + b.Length > n) continue;
                    int j = 2;
                    while (j < b.Length && data[i + j] == b[j]) j++;
                    if (j < b.Length) continue;
                    bool before = i == 0 || !IsWordByte(data[i - 1]) || !IsWordByte(b[0]);
                    bool after = i + b.Length >= n || !IsWordByte(data[i + b.Length]) || !IsWordByte(b[b.Length - 1]);
                    if (before && after) found.Add(names[b]);
                }
            }
            return found.ToList();
        }
    }

    public enum DiscState { Missing, Found, WrongDisc, Unreadable, NotADisc }

    public sealed class DiscInfo
    {
        public string Path = "";
        public DiscState State = DiscState.Missing;
        public string Message = "";
        public bool IsImage;
        public string Boot = "";     // BOOT.BIN of an unpacked disc
        public bool Ok { get { return State == DiscState.Found; } }
    }

    /// <summary>Is this the player's own Chili Con Carnage (USA)? The same test the game makes, without the hash.</summary>
    public static class DiscCheck
    {
        public const string DiscId = "ULUS-10216";
        // One short sentence each: they stand in red under the disc's path and in the message line.
        public const string WrongText = "This is not Chili Con Carnage (USA).";
        public const string UnreadableText = "This file cannot be read as an .iso disc image.";
        public const string NotADiscText = "This folder is not a PSP game disc.";

        public static DiscInfo Check(string path)
        {
            var info = new DiscInfo { Path = path ?? "" };
            if (string.IsNullOrEmpty(path)) return info;
            try
            {
                if (Directory.Exists(path))
                {
                    string root = path.TrimEnd('\\', '/');
                    for (int up = 0; up < 2 && !Directory.Exists(System.IO.Path.Combine(root, "PSP_GAME")); up++)
                    {
                        string leaf = System.IO.Path.GetFileName(root).ToLowerInvariant();
                        if (leaf != "psp_game" && leaf != "sysdir" && leaf != "usrdir") break;
                        root = System.IO.Path.GetDirectoryName(root);
                    }
                    info.Path = root;
                    string umd = System.IO.Path.Combine(root, "UMD_DATA.BIN");
                    string boot = System.IO.Path.Combine(root, "PSP_GAME", "SYSDIR", "BOOT.BIN");
                    if (!File.Exists(umd) || !File.Exists(boot)) { info.State = DiscState.NotADisc; info.Message = NotADiscText; return info; }
                    var head = new byte[64];
                    int got;
                    using (var f = File.OpenRead(umd)) got = f.Read(head, 0, head.Length);
                    info.Boot = boot;
                    return Judge(info, head, got);
                }
                if (!File.Exists(path)) { info.Message = path + " was not found."; return info; }
                info.IsImage = true;
                if (System.IO.Path.GetExtension(path).ToLowerInvariant() == ".cso") { info.State = DiscState.Unreadable; info.Message = UnreadableText; return info; }
                using (var f = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
                {
                    var iso = new IsoReader(f);
                    if (!iso.Open()) { info.State = DiscState.Unreadable; info.Message = UnreadableText; return info; }
                    byte[] head = iso.ReadFile("UMD_DATA.BIN", 64);
                    bool boot = iso.Exists("PSP_GAME/SYSDIR/BOOT.BIN");
                    if (head == null || !boot)
                    {
                        info.State = DiscState.NotADisc;
                        info.Message = "This file is not a PSP game disc.";
                        return info;
                    }
                    return Judge(info, head, head.Length);
                }
            }
            catch (Exception)
            {
                info.State = DiscState.Unreadable;
                info.Message = info.IsImage ? UnreadableText : "This folder cannot be read.";
                return info;
            }
        }

        static DiscInfo Judge(DiscInfo info, byte[] head, int length)
        {
            int bar = Array.IndexOf(head, (byte)'|', 0, length);
            string id = Encoding.ASCII.GetString(head, 0, bar < 0 ? length : bar);
            if (id == DiscId) { info.State = DiscState.Found; return info; }
            info.State = DiscState.WrongDisc;
            info.Message = WrongText;
            return info;
        }

        /// <summary>The few pieces of ISO 9660 needed to find two files.</summary>
        sealed class IsoReader
        {
            readonly Stream s;
            uint rootLba, rootSize;
            public IsoReader(Stream stream) { s = stream; }
            byte[] Read(long offset, int count)
            {
                var b = new byte[count];
                s.Position = offset;
                int done = 0;
                while (done < count) { int r = s.Read(b, done, count - done); if (r <= 0) return null; done += r; }
                return b;
            }
            public bool Open()
            {
                if (s.Length < 17 * 2048) return false;
                var pvd = Read(16 * 2048, 2048);
                if (pvd == null || pvd[0] != 1 || Encoding.ASCII.GetString(pvd, 1, 5) != "CD001") return false;
                rootLba = BitConverter.ToUInt32(pvd, 156 + 2);
                rootSize = BitConverter.ToUInt32(pvd, 156 + 10);
                return rootSize > 0 && rootSize < (16u << 20);
            }
            bool Find(uint lba, uint size, string name, out uint outLba, out uint outSize, out bool dir)
            {
                outLba = 0; outSize = 0; dir = false;
                if (size > (16u << 20)) return false;
                var data = Read((long)lba * 2048, (int)size);
                if (data == null) return false;
                int p = 0;
                while (p < data.Length)
                {
                    int len = data[p];
                    if (len == 0) { p = (p / 2048 + 1) * 2048; continue; }
                    if (p + len > data.Length || len < 34) break;
                    int nameLen = data[p + 32];
                    string entry = Encoding.ASCII.GetString(data, p + 33, Math.Min(nameLen, len - 33));
                    int semi = entry.IndexOf(';');
                    if (semi >= 0) entry = entry.Substring(0, semi);
                    entry = entry.TrimEnd('.');
                    if (string.Equals(entry, name, StringComparison.OrdinalIgnoreCase))
                    {
                        outLba = BitConverter.ToUInt32(data, p + 2);
                        outSize = BitConverter.ToUInt32(data, p + 10);
                        dir = (data[p + 25] & 2) != 0;
                        return true;
                    }
                    p += len;
                }
                return false;
            }
            bool Locate(string path, out uint lba, out uint size)
            {
                lba = rootLba; size = rootSize;
                var parts = path.Split('/');
                for (int i = 0; i < parts.Length; i++)
                {
                    bool dir;
                    uint nl, ns;
                    if (!Find(lba, size, parts[i].TrimEnd('.'), out nl, out ns, out dir)) return false;
                    if (dir != (i < parts.Length - 1)) return false;
                    lba = nl; size = ns;
                }
                return true;
            }
            public bool Exists(string path) { uint l, n; return Locate(path, out l, out n); }
            public byte[] ReadFile(string path, int max)
            {
                uint l, n;
                if (!Locate(path, out l, out n)) return null;
                return Read((long)l * 2048, (int)Math.Min((uint)max, n));
            }
        }
    }

    public sealed class ModInfo
    {
        public string Name = "", Dir = "";
        public bool HasTextures, Empty, Unreadable, Enabled;
    }

    public sealed class Model
    {
        public string BaseDir;           // the launcher's folder
        public string IniDir;            // where ChiliNative.ini is
        public string IniPath;
        public LauncherConfig Config;
        public GameInfo Game = new GameInfo();
        public IniDocument Doc = new IniDocument();
        public bool IniExists;
        public DiscInfo Disc = new DiscInfo();
        public List<ModInfo> Mods = new List<ModInfo>();
        public bool ModsFolderExists;
        public event Action Changed;

        const string UnsetMark = "\u0001unset";
        readonly Dictionary<string, string> pending = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        readonly Dictionary<string, Setting> pendingSettings = new Dictionary<string, Setting>(StringComparer.OrdinalIgnoreCase);

        public static bool IsTrue(string v) { return v != null && (v.Trim() == "1" || v.Trim().Equals("true", StringComparison.OrdinalIgnoreCase)); }
        public static string Num(double v, string format = "0.###") { return v.ToString(format, CultureInfo.InvariantCulture); }
        public static bool TryNum(string s, out double v) { return double.TryParse(s, NumberStyles.Float, CultureInfo.InvariantCulture, out v); }

        public Model(string baseDir, string iniArg)
        {
            BaseDir = baseDir;
            Config = LauncherConfig.Load(baseDir);
            string dir = iniArg;
            if (string.IsNullOrEmpty(dir)) dir = Config.Get("IniFolder", "");
            if (string.IsNullOrEmpty(dir)) dir = baseDir;
            if (!Path.IsPathRooted(dir)) dir = Path.Combine(baseDir, dir);
            if (dir.EndsWith(".ini", StringComparison.OrdinalIgnoreCase)) { IniPath = Path.GetFullPath(dir); IniDir = Path.GetDirectoryName(IniPath); }
            else { IniDir = Path.GetFullPath(dir); IniPath = Path.Combine(IniDir, "ChiliNative.ini"); }
            Game = GameInfo.Remembered(GameInfo.Find(baseDir, Config), Config);
            Reload();
        }

        public void InspectGame() { Game = GameInfo.Inspect(GameInfo.Find(BaseDir, Config), Config); }

        /// <summary>
        /// Reads the file again. Changes not yet saved stay on top of it. A file that was read before and is
        /// not there, or cannot be opened, at this moment (the game's writer swapping its own text in, a
        /// scanner holding it) is waited for; if it stays away the text read last stands.
        /// </summary>
        public void Reload()
        {
            bool missing;
            byte[] bytes = null;
            try { bytes = ReadIni(out missing); }
            catch { missing = false; }
            if (bytes != null) { Doc = IniDocument.Parse(bytes); IniExists = true; known = bytes; }
            else if (missing && known == null) { Doc = new IniDocument(); IniExists = false; }
            // Otherwise: gone or closed for a moment. Doc and IniExists stay what they were.
            RefreshDisc();
            ScanMods();
        }

        // The file's bytes as this launcher last read or wrote them; null while it has seen no file.
        byte[] known;
        /// <summary>How long a settings file that should be there is waited for, and how often it is looked for (ms).</summary>
        public const int IniPatience = 500, IniPatienceStep = 20;
        /// <summary>Checks: called between a save's read of the file and its swap, and before a save's read.</summary>
        public static Action SaveBetween, SaveBeforeRead;

        /// <summary>
        /// The file's bytes as it is on disk now, opened so that the game's writer and anything else can go on
        /// reading, writing and replacing it meanwhile (File.ReadAllBytes shares for reading only, which makes
        /// another writer's swap fail). Null with <paramref name="missing"/> set when there is no file. A file
        /// this launcher has seen (<see cref="known"/>) is waited for when it is not there, and any file when
        /// another program has it to itself: both happen for a few milliseconds while a writer swaps a new
        /// text in.
        /// </summary>
        byte[] ReadIni(out bool missing)
        {
            int started = Environment.TickCount;
            for (;;)
            {
                bool late = unchecked(Environment.TickCount - started) >= IniPatience;
                try
                {
                    using (var stream = new FileStream(IniPath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
                    {
                        var bytes = new byte[stream.Length];
                        int got = 0;
                        while (got < bytes.Length)
                        {
                            int n = stream.Read(bytes, got, bytes.Length - got);
                            if (n <= 0) break;
                            got += n;
                        }
                        if (got != bytes.Length) Array.Resize(ref bytes, got);
                        missing = false;
                        return bytes;
                    }
                }
                catch (FileNotFoundException) { if (known == null || late) { missing = true; return null; } }
                catch (DirectoryNotFoundException) { if (known == null || late) { missing = true; return null; } }
                catch (IOException) { if (late) throw; }
                catch (UnauthorizedAccessException) { if (late) throw; }
                System.Threading.Thread.Sleep(IniPatienceStep);
            }
        }
        // Writes the file in place, tried again for a quarter of a second when another program holds it.
        void WriteIniInPlace(byte[] bytes)
        {
            int started = Environment.TickCount;
            for (;;)
            {
                try
                {
                    using (var stream = new FileStream(IniPath, FileMode.Create, FileAccess.Write, FileShare.Read)) { stream.Write(bytes, 0, bytes.Length); stream.Flush(true); }
                    return;
                }
                catch (IOException) { if (unchecked(Environment.TickCount - started) >= IniPatience / 2) throw; }
                System.Threading.Thread.Sleep(IniPatienceStep);
            }
        }

        static string Id(Setting s) { return s.Id; }

        public string DefaultOf(Setting s) { return s.DefaultFn != null ? s.DefaultFn(this) : s.Default; }

        string FileValue(Setting s) { return s.Raw ? Doc.GetRaw(s.Section, s.Key) : Doc.Get(s.Section, s.Key); }

        /// <summary>The value in force: what was changed here, else the file's, else the game's default.</summary>
        public string Get(Setting s)
        {
            string v;
            if (pending.TryGetValue(Id(s), out v)) return v == UnsetMark ? DefaultOf(s) : v;
            v = FileValue(s);
            return v ?? DefaultOf(s);
        }
        /// <summary>True when a value is set (here or in the file), false when the game's default applies.</summary>
        public bool IsSet(Setting s)
        {
            string v;
            if (pending.TryGetValue(Id(s), out v)) return v != UnsetMark;
            return FileValue(s) != null;
        }
        public bool GetBool(Setting s) { return IsTrue(Get(s)); }
        public double GetNum(Setting s, double fallback)
        {
            double v;
            return TryNum(Get(s), out v) ? v : fallback;
        }
        public string Scheme { get { return Get(S.Scheme).Trim().ToLowerInvariant() == "psp" ? "psp" : "dual_analog"; } }
        /// <summary>The control style: the file's Layout when it names one, else the one its scheme goes with.</summary>
        public string Layout
        {
            get
            {
                string v = (Get(S.Layout) ?? "").Trim().ToLowerInvariant();
                return Defaults.Layouts.Contains(v) ? v : Scheme == "psp" ? "psp" : "tod_pc";
            }
        }

        public static bool Equivalent(string a, string b)
        {
            if (a == null || b == null) return a == b;
            a = a.Trim(); b = b.Trim();
            if (string.Equals(a, b, StringComparison.OrdinalIgnoreCase)) return true;
            double x, y;
            if (TryNum(a, out x) && TryNum(b, out y)) return Math.Abs(x - y) < 1e-9;
            bool ta = IsTrue(a), tb = IsTrue(b);
            bool fa = a == "0" || a.Equals("false", StringComparison.OrdinalIgnoreCase), fb = b == "0" || b.Equals("false", StringComparison.OrdinalIgnoreCase);
            return (ta && tb) || (fa && fb);
        }

        public void Set(Setting s, string value)
        {
            string file = FileValue(s);
            string effective = file ?? DefaultOf(s);
            if (Equivalent(effective, value))
            {
                pending.Remove(Id(s));
                pendingSettings.Remove(Id(s));
            }
            else { pending[Id(s)] = value; pendingSettings[Id(s)] = s; }
            Raise();
        }
        public void Set(Setting s, bool value) { Set(s, value ? "true" : "false"); }

        /// <summary>Takes the key out of force (the line is switched off with ';'), so the game's default applies.</summary>
        public void Unset(Setting s)
        {
            if (FileValue(s) == null) { pending.Remove(Id(s)); pendingSettings.Remove(Id(s)); }
            else { pending[Id(s)] = UnsetMark; pendingSettings[Id(s)] = s; }
            Raise();
        }

        /// <summary>
        /// Has the next Save write the value the launcher shows even though it is the default: for a key
        /// whose default differs between game builds, so that what is shown is what runs. Nothing happens
        /// when the file has a live line, when the game beside the launcher does not know the key, or when
        /// there is no settings file yet and nothing else to write (the game writes its own file, with its
        /// comments, on its first start: the launcher does not put a one-line file in its place).
        /// </summary>
        public void Pin(Setting s)
        {
            if (!Game.Knows(s.Probe) || FileValue(s) != null || pending.ContainsKey(Id(s))) return;
            if (!IniExists && pending.Count == 0) return;
            pending[Id(s)] = DefaultOf(s);
            pendingSettings[Id(s)] = s;
        }

        public bool IsChanged(Setting s) { return pending.ContainsKey(Id(s)); }
        public bool AnyChanged { get { return pending.Count > 0; } }
        public void Raise() { var h = Changed; if (h != null) h(); }

        /// <summary>
        /// The mutex the launcher and the game take around read, change and swap of ChiliNative.ini: the game
        /// writes the same file from its own settings page (host/ini_document.hpp, ini_gate_name).
        /// </summary>
        public const string IniGateName = @"Local\ChiliConCarnage.PC.Settings";
        /// <summary>
        /// Checks (--every-value): told of every save that has something to write: the file before, the
        /// changes in the order they were made (section, key, "set" | "raw" | "unset", value), the file after.
        /// </summary>
        public static Action<byte[], List<string[]>, byte[]> SaveObserver;

        /// <summary>Writes the changes into the file as it is on disk now. Returns null, or why it failed.</summary>
        public string Save()
        {
            if (pending.Count == 0) return null;
            System.Threading.Mutex gate = null;
            bool held = false;
            try
            {
                // One writer at a time. Without the mutex (it cannot be made, or the wait runs out) the save
                // goes on: it still changes only its own keys on the file as it is now.
                try
                {
                    gate = new System.Threading.Mutex(false, IniGateName);
                    try { held = gate.WaitOne(2000); } catch (System.Threading.AbandonedMutexException) { held = true; }
                }
                catch { }
                IniDocument doc;
                if (SaveBeforeRead != null) SaveBeforeRead();
                // The file as it is on disk now. A file this launcher has seen and that is gone at this moment
                // is never taken for "no file yet" (that would put the few changed lines in the place of the
                // whole file): it is waited for, and if it stays away it is written again whole from the
                // bytes seen last, with the changes.
                bool missing;
                byte[] before = ReadIni(out missing);
                bool remade = before == null && known != null;
                if (before == null) before = known ?? new byte[0];
                doc = IniDocument.Parse(before);
                var made = SaveObserver != null ? new List<string[]>() : null;
                foreach (var pair in pending)
                {
                    var s = pendingSettings[pair.Key];
                    if (pair.Value == UnsetMark) doc.Unset(s.Section, s.Key);
                    else doc.Set(s.Section, s.Key, pair.Value, s.Raw);
                    if (made != null) made.Add(new[] { s.Section, s.Key, pair.Value == UnsetMark ? "unset" : s.Raw ? "raw" : "set", pair.Value == UnsetMark ? "" : pair.Value });
                }
                byte[] after = doc.ToBytes();
                if (made != null) SaveObserver(before, made, after);
                if (remade || !after.SequenceEqual(before)) SwapIn(after, SaveBetween);
                Doc = doc;
                known = after;
                IniExists = true;
                pending.Clear();
                pendingSettings.Clear();
                Raise();
                return null;
            }
            catch (Exception e)
            {
                try { File.Delete(IniPath + ".launcher.tmp"); } catch { }
                return e.Message.TrimEnd('.');
            }
            finally
            {
                if (gate != null)
                {
                    if (held) try { gate.ReleaseMutex(); } catch { }
                    gate.Dispose();
                }
            }
        }

        // Writes `after` beside the file and swaps it in, so a crash cannot leave half a settings file; in place
        // where the folder refuses the swap. `between` runs once the new text is beside the file (checks).
        void SwapIn(byte[] after, Action between)
        {
            Directory.CreateDirectory(IniDir);
            string temp = IniPath + ".launcher.tmp";
            File.WriteAllBytes(temp, after);
            if (between != null) between();
            bool swapped = false;
            // A program that holds the file for a moment makes the swap fail: tried again before anything else.
            for (int turn = 0; turn < 4 && !swapped && File.Exists(IniPath); turn++)
            {
                if (turn > 0) System.Threading.Thread.Sleep(IniPatienceStep);
                try { File.Replace(temp, IniPath, null); swapped = true; }
                catch (IOException) { }
                catch (UnauthorizedAccessException) { }
            }
            // No file under the name (there was none, or a swap failed after it had moved the old one
            // away): the new text is moved into its place.
            if (!swapped && !File.Exists(IniPath) && File.Exists(temp))
            {
                try { File.Move(temp, IniPath); swapped = true; }
                catch (IOException) { }
                catch (UnauthorizedAccessException) { }
            }
            if (!swapped)
            {
                // Some folders (network shares, synced drives) refuse the swap: in place. `after` is the whole file.
                WriteIniInPlace(after);
                try { File.Delete(temp); } catch { }
            }
        }

        /// <summary>
        /// A settings file the game wrote with [Input] Version 2 is brought up to date as the game would at its next
        /// start (InputMigration), so that the Controls page shows what the game will run with: under the mutex the
        /// two share, with the file as it was kept as ChiliNative.ini.bak (over an older one), as the game keeps it.
        /// Any other file is left alone. Returns the control style the file now names, or null when nothing was done.
        /// </summary>
        public string MigrateInput()
        {
            if (!IniExists || pending.Count > 0) return null;
            System.Threading.Mutex gate = null;
            bool held = false;
            try
            {
                try
                {
                    gate = new System.Threading.Mutex(false, IniGateName);
                    try { held = gate.WaitOne(2000); } catch (System.Threading.AbandonedMutexException) { held = true; }
                }
                catch { }
                bool missing;
                byte[] before = ReadIni(out missing);
                if (before == null) return null;
                var doc = IniDocument.Parse(before);
                if (!InputMigration.Wanted(before, doc)) return null;
                int changed;
                string now = InputMigration.Apply(doc, out changed);
                byte[] after = doc.ToBytes();
                File.WriteAllBytes(IniPath + ".bak", before);
                SwapIn(after, null);
                Doc = doc;
                known = after;
                IniExists = true;
                Raise();
                return now;
            }
            catch { return null; }
            finally
            {
                if (gate != null)
                {
                    if (held) try { gate.ReleaseMutex(); } catch { }
                    gate.Dispose();
                }
            }
        }
        /// <summary>Live lines of the file the launcher has no control for.</summary>
        public int UnknownLines()
        {
            var known = new HashSet<string>(S.All.Select(s => s.Id), StringComparer.OrdinalIgnoreCase);
            int n = 0;
            foreach (var line in Doc.LiveLines()) if (!known.Contains(line.Section + "/" + line.Key)) n++;
            return n;
        }

        // ---- folders ---------------------------------------------------------------------------

        public string GameDir { get { try { return Path.GetDirectoryName(Game.ExePath) ?? BaseDir; } catch { return BaseDir; } } }
        string Resolve(string value, string root)
        {
            try { return Path.GetFullPath(Path.IsPathRooted(value) ? value : Path.Combine(root, value)); }
            catch { return value; }
        }
        // The game keeps saves\ and logs\ beside its own program; launcher.ini may name other folders (relative to the launcher).
        public string SavesDir { get { return Resolve(Config.Get("Saves", "saves"), Config.Get("Saves", "").Length > 0 ? BaseDir : GameDir); } }
        public string LogsDir { get { return Resolve(Config.Get("Logs", "logs"), Config.Get("Logs", "").Length > 0 ? BaseDir : GameDir); } }
        public string ModsDir { get { string f = Get(S.ModsFolder); return Resolve(string.IsNullOrWhiteSpace(f) ? "mods" : f.Trim(), IniDir); } }
        public bool DeveloperStart { get { return Config.Get("Arguments", "").Length > 0; } }

        /// <summary>The newest log of the game, or null.</summary>
        public string LatestLog()
        {
            try
            {
                var names = new[] { "ChiliConCarnage.log", "last_run.log", "last_run.err.log" };
                string best = null;
                DateTime when = DateTime.MinValue;
                foreach (var dir in new[] { LogsDir, IniDir, BaseDir }.Distinct())
                    foreach (var n in names)
                    {
                        string p = Path.Combine(dir, n);
                        if (!File.Exists(p)) continue;
                        var t = File.GetLastWriteTime(p);
                        if (t > when && new FileInfo(p).Length > 0) { when = t; best = p; }
                    }
                return best;
            }
            catch { return null; }
        }

        // ---- the disc --------------------------------------------------------------------------

        public void RefreshDisc()
        {
            string named = Get(S.DiscPath);
            if (!string.IsNullOrWhiteSpace(named))
            {
                Disc = DiscCheck.Check(Resolve(named.Trim(), IniDir));
                if (Disc.State == DiscState.Missing && Disc.Message.Length == 0) Disc.Message = Disc.Path + " was not found.";
                return;
            }
            string dev = Config.Get("Disc", "");
            if (dev.Length > 0) { Disc = DiscCheck.Check(Resolve(dev, BaseDir)); return; }
            // What the game looks for by itself: a disc beside the program.
            try
            {
                foreach (var dir in new[] { IniDir, GameDir }.Distinct())
                {
                    foreach (var candidate in new[] { Path.Combine(dir, "disc0"), dir })
                    {
                        if (!Directory.Exists(Path.Combine(candidate, "PSP_GAME"))) continue;
                        var d = DiscCheck.Check(candidate);
                        if (d.Ok) { Disc = d; return; }
                    }
                    if (!Directory.Exists(dir)) continue;
                    foreach (var iso in Directory.GetFiles(dir, "*.iso"))
                    {
                        var d = DiscCheck.Check(iso);
                        if (d.Ok) { Disc = d; return; }
                    }
                }
            }
            catch { }
            Disc = new DiscInfo();
        }

        /// <summary>The disc the player picked: written relative to the ini's folder when it is inside it, as the game does.</summary>
        public DiscInfo ChooseDisc(string path)
        {
            var d = DiscCheck.Check(path);
            if (!d.Ok) return d;
            string shown = d.Path;
            try
            {
                string root = IniDir.TrimEnd('\\') + "\\";
                if (shown.StartsWith(root, StringComparison.OrdinalIgnoreCase)) shown = shown.Substring(root.Length);
            }
            catch { }
            Set(S.DiscPath, shown);
            Disc = d;
            Raise();
            return d;
        }

        // ---- mods ------------------------------------------------------------------------------

        public void ScanMods()
        {
            var found = new List<ModInfo>();
            string folder = ModsDir;
            ModsFolderExists = Directory.Exists(folder);
            if (ModsFolderExists)
            {
                try
                {
                    foreach (var dir in Directory.GetDirectories(folder))
                    {
                        string name = Path.GetFileName(dir);
                        if (name.StartsWith(".")) continue;
                        var mod = new ModInfo { Name = name, Dir = dir };
                        try
                        {
                            mod.HasTextures = Directory.Exists(Path.Combine(dir, "textures"));
                            mod.Empty = !Directory.EnumerateFileSystemEntries(dir).Any();
                        }
                        catch { mod.Unreadable = true; }
                        found.Add(mod);
                    }
                }
                catch { }
            }
            found.Sort((a, b) => string.Compare(a.Name, b.Name, StringComparison.OrdinalIgnoreCase));
            Mods = OrderMods(found);
        }

        List<ModInfo> OrderMods(List<ModInfo> found)
        {
            if (!IsSet(S.ModsEnabled))
            {
                foreach (var m in found) m.Enabled = true;
                return found;
            }
            var listed = (Get(S.ModsEnabled) ?? "").Split(',').Select(n => n.Trim()).Where(n => n.Length > 0).ToList();
            var result = new List<ModInfo>();
            foreach (var name in listed)
            {
                var mod = found.FirstOrDefault(m => string.Equals(m.Name, name, StringComparison.OrdinalIgnoreCase));
                if (mod != null && !result.Contains(mod)) { mod.Enabled = true; result.Add(mod); }
            }
            // Mods that are off keep the place the player gave them (remembered in launcher.ini), else alphabetical.
            var remembered = Config.Get("ModOrder", "").Split(',').Select(n => n.Trim()).Where(n => n.Length > 0).ToList();
            var off = found.Where(m => !result.Contains(m)).ToList();
            foreach (var m in off) m.Enabled = false;
            var ordered = new List<ModInfo>();
            if (remembered.Count > 0)
            {
                foreach (var name in remembered)
                {
                    var mod = found.FirstOrDefault(m => string.Equals(m.Name, name, StringComparison.OrdinalIgnoreCase));
                    if (mod != null && !ordered.Contains(mod)) ordered.Add(mod);
                }
                foreach (var m in found) if (!ordered.Contains(m)) ordered.Add(m);
                // The enabled ones must keep the ini's order among themselves.
                var queue = new Queue<ModInfo>(result);
                for (int i = 0; i < ordered.Count; i++) if (ordered[i].Enabled) ordered[i] = queue.Dequeue();
                return ordered;
            }
            result.AddRange(off);
            return result;
        }

        /// <summary>Writes the list's order and switches into [Mods] Enabled.</summary>
        public void CommitMods()
        {
            bool allOn = Mods.All(m => m.Enabled);
            bool alphabetical = true;
            for (int i = 1; i < Mods.Count; i++)
                if (string.Compare(Mods[i - 1].Name, Mods[i].Name, StringComparison.OrdinalIgnoreCase) > 0) alphabetical = false;
            if (allOn && alphabetical && Mods.Count > 0) Unset(S.ModsEnabled);
            else if (Mods.Count == 0) Unset(S.ModsEnabled);
            else
            {
                string list = string.Join(",", Mods.Where(m => m.Enabled).Select(m => m.Name));
                string file = Doc.Get("Mods", "Enabled");
                if (file != null && file.Replace(" ", "") == list) { pending.Remove(S.ModsEnabled.Id); pendingSettings.Remove(S.ModsEnabled.Id); Raise(); }
                else { pending[S.ModsEnabled.Id] = list; pendingSettings[S.ModsEnabled.Id] = S.ModsEnabled; Raise(); }
            }
            Config.Set("ModOrder", string.Join(",", Mods.Select(m => m.Name)));
        }

        public List<ModInfo> TexturePacks { get { return Mods.Where(m => m.HasTextures).ToList(); } }
        /// <summary>The mods that will load: switched on, and with something in them (an empty or unreadable folder does not count).</summary>
        public int EnabledModCount { get { return Mods.Count(m => m.Enabled && !m.Empty && !m.Unreadable); } }

        // ---- bindings --------------------------------------------------------------------------

        public List<string> KeysOf(string target)
        {
            string v = Get(S.Keys(target)) ?? "";
            return v.Split(',').Select(k => k.Trim()).Where(k => k.Length > 0).ToList();
        }
        public void SetKeys(string target, IEnumerable<string> keys) { Set(S.Keys(target), string.Join(",", keys)); }
        public string MouseTarget(string input) { return Canon(Get(S.Mouse(input))); }
        public string PadTarget(string button) { return Canon(Get(S.Pad(button))); }
        static string Canon(string target)
        {
            if (string.IsNullOrWhiteSpace(target)) return "None";
            var a = Actions.Find(target.Trim());
            return a != null ? a.Target : target.Trim();
        }

        /// <summary>The keyboard action that holds the key, or null.</summary>
        public ActionDef KeyOwner(string key, string except)
        {
            foreach (var a in Actions.All)
            {
                if (string.Equals(a.Target, except, StringComparison.OrdinalIgnoreCase)) continue;
                foreach (var k in KeysOf(a.Target)) if (KeyNames.Same(k, key)) return a;
            }
            return null;
        }

        public void ResetKeyboardMouse()
        {
            foreach (var a in Actions.All) Set(S.Keys(a.Target), Defaults.Keyboard(Layout, a.Target));
            foreach (var i in S.MouseInputs) Set(S.Mouse(i), Defaults.Mouse(i));
        }
        public void ResetPadBindings()
        {
            foreach (var b in S.PadButtons) Set(S.Pad(b), Defaults.Pad(Layout, b));
        }
        /// <summary>
        /// Another control style: [Input] Layout and the Scheme that goes with it, and every binding that still
        /// holds the old style's default takes the new style's (the bindings the player changed stay). The
        /// game's Settings page does the same (host/settings_apply.hpp).
        /// </summary>
        public void SwitchLayout(string to)
        {
            string from = Layout;
            if (to == from || Array.IndexOf(Defaults.Layouts, to) < 0) return;
            var moved = Defaults.BindingChanges(s => IsSet(s) ? Get(s) : null, from, to);
            Set(S.Layout, to);
            Set(S.Scheme, to == "psp" ? "psp" : "dual_analog");
            foreach (var change in moved) Set(change.Key, change.Value);
        }
        public void ResetTo(params Setting[] settings) { foreach (var s in settings) Set(s, DefaultOf(s)); }
    }

    /// <summary>Key names as ChiliNative.ini spells them.</summary>
    public static class KeyNames
    {
        static readonly Dictionary<string, string> aliases = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
        {
            { "Return", "Enter" }, { "Esc", "Escape" },
        };
        public static string Canon(string name)
        {
            string a;
            name = (name ?? "").Trim();
            if (aliases.TryGetValue(name, out a)) return a;
            return name;
        }
        public static bool Same(string a, string b) { return string.Equals(Canon(a), Canon(b), StringComparison.OrdinalIgnoreCase); }

        /// <summary>
        /// What a chip shows for a gamepad button. The ini names buttons by position, in Xbox letters; on a
        /// PlayStation controller the same places are Cross, Circle, Square, Triangle, L1 ... (`create`: a
        /// DualSense, whose Share button is called Create).
        /// </summary>
        public static string PadDisplay(string name, PadKind look = PadKind.Xbox, bool create = false)
        {
            if (look == PadKind.PlayStation)
                switch ((name ?? "").ToLowerInvariant())
                {
                    case "a": return "Cross";
                    case "b": return "Circle";
                    case "x": return "Square";
                    case "y": return "Triangle";
                    case "lb": return "L1";
                    case "rb": return "R1";
                    case "lt": return "L2";
                    case "rt": return "R2";
                    case "ls": return "L3";
                    case "rs": return "R3";
                    case "back": return create ? "Create" : "Share";
                    case "start": return "Options";
                }
            switch ((name ?? "").ToLowerInvariant())
            {
                case "lt": return "Left trigger";
                case "rt": return "Right trigger";
                case "lb": return "Left bumper";
                case "rb": return "Right bumper";
                case "ls": return "Left stick click";
                case "rs": return "Right stick click";
            }
            return Display(name);
        }

        /// <summary>What a chip shows for a key, mouse input or pad button.</summary>
        public static string Display(string name)
        {
            switch (Canon(name).ToLowerInvariant())
            {
                case "mouse1": return "Left click";
                case "mouse2": return "Right click";
                case "mouse3": return "Middle click";
                case "mouse4": return "Mouse 4";
                case "mouse5": return "Mouse 5";
                case "wheelup": return "Wheel up";
                case "wheeldown": return "Wheel down";
                case "dpadup": return "D-pad up";
                case "dpaddown": return "D-pad down";
                case "dpadleft": return "D-pad left";
                case "dpadright": return "D-pad right";
                case "lshift": return "Left Shift";
                case "rshift": return "Right Shift";
                case "lctrl": return "Left Ctrl";
                case "rctrl": return "Right Ctrl";
                case "lalt": return "Left Alt";
                case "ralt": return "Right Alt";
                case "pageup": return "Page Up";
                case "pagedown": return "Page Down";
                case "capslock": return "Caps Lock";
                case "lbracket": return "[";
                case "rbracket": return "]";
                case "comma": return ",";
                case "period": return ".";
                case "slash": return "/";
                case "semicolon": return ";";
                case "quote": return "'";
                case "backslash": return "\\";
                case "minus": return "-";
                case "equals": return "=";
                case "grave": return "`";
            }
            string n = Canon(name);
            if (n.StartsWith("Numpad", StringComparison.OrdinalIgnoreCase) && n.Length == 7) return "Numpad " + n[6];
            if (n.Length == 1) return n.ToUpperInvariant();
            return n;
        }
    }
}
