// --every-value <folder>: the launcher's half of the check that the launcher and the game agree
// (profiles\chili\scripts\check_launcher_settings.ps1 is the other half).
//
// The folder holds a ChiliNative.ini (the game's own, written with --write-default-ini) and, for the rows
// of the Textures & mods page, a mods folder. Every row of every page is put to each of its values
// through the row's own control: a click on a choice or a switch, a drag and the arrow keys on a slider, a
// capture on a binding chip, the page's Defaults button. Then the settings are saved as PLAY saves them,
// and the file the launcher wrote is kept under a number (001.ini, 002.ini, ...). list.tsv says what each
// file is; skipped.txt names what could not be set and why. Nothing is shown: the pages are built off
// screen, as for --screenshot. The script then starts the game on each file and reads what it says.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Windows;
using System.Windows.Input;

namespace ChiliLauncher
{
    public sealed partial class Surface
    {
        /// <summary>Checks: the rows of the page that is shown.</summary>
        public IList<Row> RowsNow { get { return rows; } }
        /// <summary>Checks: the question that is open, or null.</summary>
        public DialogState Question { get { return dialog; } }
        /// <summary>Checks: presses a button of the open question (0 is the first).</summary>
        public void Answer(int choice) { if (dialog != null) CloseDialog(choice); }
        /// <summary>Checks: a mouse button or the wheel on the waiting chip, as PointerDown and PointerWheel hand it on.</summary>
        public void CaptureMouseInput(string name)
        {
            if (Capture != null && !Capture.Control.PadView && !Capture.Control.Action.Toggle) Captured(name, true, false);
        }
        /// <summary>Checks: the Defaults button of the page that is shown.</summary>
        public void PressDefaults() { AskDefaults(); }
    }

    public static class EveryValue
    {
        sealed class Place
        {
            public string Page, Id;
            public bool Pad;
            public string Name { get { return Page == "Controls" ? (Pad ? "Controls, Gamepad" : "Controls, Keyboard & mouse") : Page; } }
        }

        static string baseDir, folder, iniPath, launcherIni;
        static byte[] startIni, startLauncherIni;
        // Every save, for the check that the game's own writer (host/ini_document.hpp) makes the same bytes:
        // transitions.tsv, with the file before and after each save in transitions\. `state` is the save the
        // file on disk came from, 0 for the file the folder began with.
        static readonly List<string> transitions = new List<string>();
        static int saves, state;
        static string Escape(string text) { return (text ?? "").Replace("\\", "\\\\").Replace("\t", "\\t").Replace("\r", "\\r").Replace("\n", "\\n"); }
        static void Saved(byte[] before, List<string[]> made, byte[] after)
        {
            saves++;
            string stem = Path.Combine(folder, "transitions", saves.ToString("0000"));
            File.WriteAllBytes(stem + ".before.ini", before);
            File.WriteAllBytes(stem + ".after.ini", after);
            transitions.Add("T\t" + saves + "\t" + state + "\t" + made.Count);
            foreach (var change in made) transitions.Add("C\t" + change[0] + "\t" + change[1] + "\t" + change[2] + "\t" + Escape(change[3]));
            state = saves;
        }
        static string[] startLines;
        static int count;
        static readonly List<string> list = new List<string>();
        static readonly List<string> skipped = new List<string>();
        static readonly List<string> faults = new List<string>();

        static void Arrange(Surface ui)
        {
            ui.InvalidateVisual();
            ui.Measure(new Size(1280, 720));
            ui.Arrange(new Rect(0, 0, 1280, 720));
            ui.UpdateLayout();
        }

        /// <summary>The launcher as it is right after its start, on the settings file the folder began with.</summary>
        static Surface Fresh()
        {
            File.WriteAllBytes(iniPath, startIni);
            File.WriteAllBytes(launcherIni, startLauncherIni);
            state = 0;
            var m = new Model(baseDir, folder);
            m.InspectGame();
            m.Reload();
            var ui = new Surface(m, new ArtFolder(baseDir, false)) { Static = true };
            Arrange(ui);
            return ui;
        }

        /// <summary>Shows the page (and the tab, and what is under Advanced) and gives the row, laid out, or null.</summary>
        static Row Go(Surface ui, Place at) { return Go(ui, at.Page, at.Pad, at.Id); }
        static Row Go(Surface ui, string page, bool pad, string id)
        {
            if (ui.PageName != page) ui.ShowPage(page);
            if (page == "Controls") ui.SetPadView(pad);
            ui.OpenAdvanced(page);
            var row = ui.FindRow(id);
            if (row != null) row.Layout(ui, 0);
            return row;
        }

        static Point Middle(Rect r) { return new Point(r.X + r.Width / 2, r.Y + r.Height / 2); }
        static void Click(Surface ui, Row row, int part, bool right = false)
        {
            var parts = row.Control.Parts;
            row.Control.OnClick(ui, part, part >= 0 && part < parts.Count && !parts[part].IsEmpty ? Middle(parts[part]) : new Point(), right);
        }
        // One field of list.tsv: no tab, no line end, and no double quote (a reader of the list takes one for the field's fence).
        static string Clean(string text) { return (text ?? "").Replace("\t", " ").Replace("\r", "").Replace("\n", " / ").Replace('"', '\''); }

        /// <summary>The live lines of the saved file that the file at the start did not have.</summary>
        static string Written()
        {
            var had = new HashSet<string>(startLines);
            var now = System.Text.Encoding.UTF8.GetString(File.ReadAllBytes(iniPath)).Replace("\r", "").Split('\n');
            var fresh = now.Where(l => l.Trim().Length > 0 && !l.TrimStart().StartsWith(";") && !had.Contains(l)).Select(l => l.Trim()).ToList();
            return fresh.Count == 0 ? "(the file as it was)" : string.Join(" | ", fresh);
        }

        /// <summary>Saves as PLAY does and keeps the file. `expect`: the setting whose live line the file must now hold.</summary>
        static void Keep(Surface ui, string page, string row, string value, string how, Setting expect = null)
        {
            if (ui.Capture != null) { faults.Add(page + " / " + row + " / " + value + ": the chip was still waiting"); ui.EndCapture(); }
            if (ui.Question != null) { faults.Add(page + " / " + row + " / " + value + ": a question was left open: " + Clean(ui.Question.Text)); ui.Answer(ui.Question.Cancel); }
            if (!ui.Save()) { faults.Add(page + " / " + row + " / " + value + ": the settings file could not be written"); return; }
            count++;
            string name = count.ToString("000") + ".ini";
            File.Copy(iniPath, Path.Combine(folder, name), true);
            transitions.Add("K\t" + name + "\t" + state);
            string line = "";
            if (expect != null && ui.Model.IsSet(expect)) line = expect.Section + "/" + expect.Key + "=" + ui.Model.Get(expect);
            list.Add(string.Join("\t", name, Clean(page), Clean(row), Clean(value), Clean(how), Clean(line), Clean(Written())));
        }
        static void Skip(string page, string row, string value, string why) { skipped.Add(Clean(page) + " / " + Clean(row) + " / " + Clean(value) + ": " + Clean(why)); }
        static Setting Only(Row row) { return row.Settings != null && row.Settings.Length == 1 ? row.Settings[0] : null; }

        // ---- a row of choices ------------------------------------------------------------------

        static void Choices(Place at)
        {
            var first = Go(Fresh(), at);
            int n = ((Segmented)first.Control).Options.Count;
            for (int i = 0; i < n; i++)
            {
                // A choice that asks a question when it is taken (Control style) is kept once per answer.
                for (int answer = 0; answer < 2; answer++)
                {
                    var ui = Fresh();
                    var row = Go(ui, at);
                    var seg = (Segmented)row.Control;
                    var o = seg.Options[i];
                    if (!o.Visible) break;
                    if (!row.IsEnabled(ui.Model)) { Skip(at.Name, row.Label, o.Label, "the row is locked: " + row.LockedReason(ui.Model)); break; }
                    if (!o.Enabled) { Skip(at.Name, row.Label, o.Label, "the choice is dimmed: " + seg.PartReason(i)); break; }
                    string label = row.Label, how = "a click";
                    var expect = Only(row);
                    if (Model.Equivalent(o.Value, seg.Get()))
                    {
                        // The choice that is selected already: another one first, saved, then this one, so that the file holds its line.
                        int other = -1;
                        for (int j = 0; j < n && other < 0; j++) if (j != i && seg.Options[j].Enabled) other = j;
                        if (other < 0) { Skip(at.Name, label, o.Label, "the only choice that can be taken"); break; }
                        Click(ui, row, other);
                        if (ui.Question != null) ui.Answer(1);
                        ui.Save();
                        row = Go(ui, at);
                        seg = (Segmented)row.Control;
                        how = "a click on \"" + seg.Options[other].Label + "\", saved, then a click";
                    }
                    Click(ui, row, i);
                    bool asked = ui.Question != null;
                    if (asked)
                    {
                        how += ", then \"" + ui.Question.Buttons[answer] + "\" to \"" + Clean(ui.Question.Text) + "\"";
                        ui.Answer(answer);
                    }
                    Keep(ui, at.Name, label, o.Label + " (" + o.Value + ")", how, expect);
                    if (!asked) break;
                }
            }
        }

        // ---- a list that opens -----------------------------------------------------------------

        static void ListChoices(Place at)
        {
            var first = Go(Fresh(), at);
            int n = ((Dropdown)first.Control).Options.Count;
            for (int i = 0; i < n; i++)
            {
                var ui = Fresh();
                var row = Go(ui, at);
                var d = (Dropdown)row.Control;
                var o = d.Options[i];
                string how = "the list, then this entry";
                if (!row.IsEnabled(ui.Model))
                {
                    // The HD pack list is locked while Textures is Standard: the HD choice first, as a player would.
                    var textures = Go(ui, at.Page, at.Pad, "textures");
                    if (at.Id != "pack" || textures == null) { Skip(at.Name, row.Label, o.Label, "the row is locked: " + row.LockedReason(ui.Model)); continue; }
                    Click(ui, textures, 1);
                    row = Go(ui, at);
                    d = (Dropdown)row.Control;
                    how = "Textures to HD pack, then the list and this entry";
                    if (!row.IsEnabled(ui.Model)) { Skip(at.Name, row.Label, o.Label, "the row is locked: " + row.LockedReason(ui.Model)); continue; }
                }
                // What a click on an entry of the open list does (Surface.Activate, DropItem).
                d.Set(o.Value);
                Keep(ui, at.Name, row.Label, o.Label, how, Only(row));
            }
        }

        // ---- a switch --------------------------------------------------------------------------

        static void Switch(Place at)
        {
            for (int pass = 0; pass < 2; pass++)
            {
                var ui = Fresh();
                var row = Go(ui, at);
                if (!row.IsEnabled(ui.Model)) { Skip(at.Name, row.Label, "On and Off", "the row is locked: " + row.LockedReason(ui.Model)); return; }
                string label = row.Label, how = "a click";
                var expect = Only(row);
                Click(ui, row, 0);
                if (pass == 1)
                {
                    ui.Save();
                    row = Go(ui, at);
                    Click(ui, row, 0);
                    how = "a click, saved, a click again";
                }
                row = Go(ui, at);
                bool on = ((Toggle)row.Control).Get();
                Keep(ui, at.Name, label, on ? "On" : "Off", how, expect);
            }
        }

        // ---- a slider --------------------------------------------------------------------------

        static void Slide(Place at)
        {
            // A drag to five places of the track, and one press of Left and of Right.
            string[] acts = { "0", "0.25", "0.5", "0.75", "1", "left", "right" };
            foreach (var act in acts)
            {
                var ui = Fresh();
                var row = Go(ui, at);
                if (!row.IsEnabled(ui.Model)) { Skip(at.Name, row.Label, "every value", "the row is locked: " + row.LockedReason(ui.Model)); return; }
                var s = (Slider)row.Control;
                string how;
                if (act == "left" || act == "right")
                {
                    int sub = 0;
                    s.OnNav(ui, act == "left" ? Nav.Left : Nav.Right, false, ref sub);
                    how = "the " + act + " arrow key, once";
                }
                else
                {
                    double t = double.Parse(act, System.Globalization.CultureInfo.InvariantCulture);
                    // The track is the part without its margins (Slider.Layout).
                    var part = s.Parts[0];
                    s.OnDrag(ui, 0, new Point(part.X + 12 + (part.Width - 24) * t, part.Y + part.Height / 2));
                    how = "a drag to " + Math.Round(t * 100) + " % of the track";
                }
                var setting = Only(row);
                Keep(ui, at.Name, row.Label, s.Show(s.Get()) + (setting != null ? " (" + ui.Model.Get(setting) + ")" : ""), how, setting);
            }
        }

        /// <summary>Every step of every slider: file k has each slider at its k-th step.</summary>
        static void Sweep(List<Place> sliders)
        {
            int most = 0;
            var probe = Fresh();
            foreach (var at in sliders)
            {
                var s = (Slider)Go(probe, at).Control;
                most = Math.Max(most, (int)Math.Round((s.Max - s.Min) / s.Step) + 1);
            }
            for (int k = 0; k < most; k++)
            {
                var ui = Fresh();
                var said = new List<string>();
                foreach (var at in sliders)
                {
                    var row = Go(ui, at);
                    if (row == null || !row.IsEnabled(ui.Model)) continue;
                    var s = (Slider)row.Control;
                    int n = (int)Math.Round((s.Max - s.Min) / s.Step) + 1;
                    // What a drag and the arrow keys call with the value they arrive at.
                    s.Set(Math.Max(s.Min, Math.Min(s.Max, Math.Round(s.Min + (k % n) * s.Step, 6))));
                    var setting = Only(row);
                    said.Add(row.Label + " " + (setting != null ? ui.Model.Get(setting) : s.Show(s.Get())));
                }
                Keep(ui, "Every slider", "step " + (k + 1) + " of " + most, string.Join(", ", said), "each slider put to this step of its track");
            }
        }

        // ---- bindings --------------------------------------------------------------------------

        /// <summary>Starts a capture on the row: on "+" when there is one, else on the first chip.</summary>
        static bool Begin(Surface ui, Row row, bool first = false)
        {
            var b = (Bindings)row.Control;
            int part = !first && b.PlusPart(b.ChipCount) ? b.ChipCount : 0;
            Click(ui, row, part);
            return ui.Capture != null;
        }

        static void Keys()
        {
            // Every key the launcher has a name for, one per action and file, on the action's first chip.
            var keys = Enum.GetValues(typeof(Key)).Cast<Key>().Distinct().Where(k => KeyMap.Name(k) != null).ToList();
            var actions = Actions.All;
            for (int from = 0; from < keys.Count; from += actions.Length)
            {
                var ui = Fresh();
                var said = new List<string>();
                for (int a = 0; a < actions.Length && from + a < keys.Count; a++)
                {
                    var row = Go(ui, "Controls", false, "bind:" + actions[a].Target);
                    if (row == null || !row.IsEnabled(ui.Model)) { Skip("Controls, Keyboard & mouse", actions[a].Label, KeyMap.Name(keys[from + a]), "the row is locked"); continue; }
                    if (!Begin(ui, row, true)) { faults.Add("no capture began on " + actions[a].Target); continue; }
                    ui.HandleKey(keys[from + a], ModifierKeys.None, false);
                    if (ui.Question != null) ui.Answer(0);      // Move
                    said.Add(actions[a].Target + "=" + string.Join(",", ui.Model.KeysOf(actions[a].Target)));
                }
                Keep(ui, "Controls, Keyboard & mouse", "Actions, a key each", string.Join(" ", said), "a click on the first chip, the key, \"Move\" when asked");
            }
            // A second and a third key on "+".
            {
                var ui = Fresh();
                var said = new List<string>();
                int next = 0;
                for (int round = 0; round < 2; round++)
                    foreach (var action in actions)
                    {
                        var row = Go(ui, "Controls", false, "bind:" + action.Target);
                        if (row == null || !row.IsEnabled(ui.Model)) continue;
                        var b = (Bindings)row.Control;
                        if (!b.PlusPart(b.ChipCount)) continue;
                        if (!Begin(ui, row)) continue;
                        ui.HandleKey(keys[next++ % keys.Count], ModifierKeys.None, false);
                        if (ui.Question != null) ui.Answer(0);
                    }
                foreach (var action in actions) said.Add(action.Target + "=" + string.Join(",", ui.Model.KeysOf(action.Target)));
                Keep(ui, "Controls, Keyboard & mouse", "Actions, keys added", string.Join(" ", said), "\"+\" on every action twice, a key each, \"Move\" when asked");
            }
        }

        static void MouseInputs()
        {
            var actions = Actions.All.Where(a => !a.Toggle).ToArray();
            for (int turn = 0; turn < actions.Length; turn++)
            {
                var ui = Fresh();
                var said = new List<string>();
                for (int i = 0; i < S.MouseInputs.Length; i++)
                {
                    var action = actions[(turn + i) % actions.Length];
                    var row = Go(ui, "Controls", false, "bind:" + action.Target);
                    if (row == null || !row.IsEnabled(ui.Model)) { Skip("Controls, Keyboard & mouse", action.Label, S.MouseInputs[i], "the row is locked"); continue; }
                    if (!Begin(ui, row)) { faults.Add("no capture began on " + action.Target); continue; }
                    ui.CaptureMouseInput(S.MouseInputs[i]);
                    if (ui.Question != null) ui.Answer(0);
                    said.Add(S.MouseInputs[i] + "=" + ui.Model.MouseTarget(S.MouseInputs[i]));
                }
                Keep(ui, "Controls, Keyboard & mouse", "Actions, the mouse", string.Join(" ", said), "a capture on the action, the mouse button or the wheel on the waiting chip, \"Move\" when asked");
            }
        }

        static ushort PadBit(string button)
        {
            switch (button)
            {
                case "A": return Surface.PadA; case "B": return Surface.PadB; case "X": return Surface.PadX; case "Y": return Surface.PadY;
                case "LB": return Surface.PadLB; case "RB": return Surface.PadRB; case "LS": return Surface.PadLS; case "RS": return Surface.PadRS;
                case "Back": return Surface.PadBack; case "Start": return Surface.PadStart;
                case "DpadUp": return Surface.PadUp; case "DpadDown": return Surface.PadDown; case "DpadLeft": return Surface.PadLeft; case "DpadRight": return Surface.PadRight;
            }
            return 0;
        }

        static void PadButtons()
        {
            var actions = Actions.All.Where(a => !a.Toggle).ToArray();
            for (int turn = 0; turn < actions.Length; turn++)
            {
                var ui = Fresh();
                var rest = new PadState { Connected = true, Kind = PadKind.Xbox, Index = 0 };
                ui.PadUpdate(rest, true);
                var said = new List<string>();
                for (int i = 0; i < S.PadButtons.Length; i++)
                {
                    string button = S.PadButtons[i];
                    var action = actions[(turn + i) % actions.Length];
                    var row = Go(ui, "Controls", true, "bind:" + action.Target);
                    if (row == null || !row.IsEnabled(ui.Model)) { Skip("Controls, Gamepad", action.Label, button, "the row is locked"); continue; }
                    ui.PadUpdate(rest, true);
                    if (!Begin(ui, row)) { faults.Add("no capture began on " + action.Target + " (gamepad)"); continue; }
                    var pressed = rest;
                    if (button == "LT") pressed.LT = 255; else if (button == "RT") pressed.RT = 255; else pressed.Buttons = PadBit(button);
                    ui.PadUpdate(pressed, true);
                    if (ui.Question != null) ui.Answer(0);
                    ui.PadUpdate(rest, true);
                    said.Add(button + "=" + ui.Model.PadTarget(button));
                }
                Keep(ui, "Controls, Gamepad", "Actions, the controller's buttons", string.Join(" ", said), "a capture on the action, the button of a made-up controller, \"Move\" when asked");
            }
        }

        /// <summary>Every chip of the tab taken away with a right click.</summary>
        static void Unbind(bool pad)
        {
            var ui = Fresh();
            foreach (var action in Actions.All)
            {
                for (int guard = 0; guard < 12; guard++)
                {
                    var row = Go(ui, "Controls", pad, "bind:" + action.Target);
                    if (row == null || !row.IsEnabled(ui.Model) || ((Bindings)row.Control).ChipCount == 0) break;
                    Click(ui, row, 0, true);
                }
            }
            Keep(ui, pad ? "Controls, Gamepad" : "Controls, Keyboard & mouse", "Actions", "nothing bound", "a right click on every chip");
        }

        // ---- mods ------------------------------------------------------------------------------

        static void Mod(Place at)
        {
            string[] acts = { "off", "off and on", "up", "down" };
            foreach (var act in acts)
            {
                var ui = Fresh();
                var row = Go(ui, at);
                if (!row.IsEnabled(ui.Model)) { Skip(at.Name, row.Label, "the switch and the arrows", "the row is locked: " + row.LockedReason(ui.Model)); return; }
                int part = act == "up" ? 1 : act == "down" ? 2 : 0;
                if (!row.Control.PartEnabled(part)) { Skip(at.Name, row.Label, act, "nowhere to go that way"); continue; }
                Click(ui, row, part);
                if (act == "off and on") { ui.Save(); Click(ui, Go(ui, at), 0); }
                Keep(ui, at.Name, row.Label, act == "off" ? "switched off" : act == "off and on" ? "switched off, saved, switched on" : "moved " + act,
                    "a click", S.ModsEnabled);
            }
        }

        // ---- the Defaults button ---------------------------------------------------------------

        static void DefaultsOf(string page, bool pad, List<Place> places)
        {
            var ui = Fresh();
            string name = page == "Controls" ? (pad ? "Controls, Gamepad" : "Controls, Keyboard & mouse") : page;
            // Something else on every row first.
            foreach (var at in places.Where(p => p.Page == page && p.Pad == pad))
            {
                var row = Go(ui, at);
                if (row == null || !row.IsEnabled(ui.Model)) continue;
                var seg = row.Control as Segmented;
                var slider = row.Control as Slider;
                if (seg != null)
                {
                    for (int i = seg.Options.Count - 1; i >= 0; i--)
                        if (seg.Options[i].Enabled && !Model.Equivalent(seg.Options[i].Value, seg.Get())) { Click(ui, row, i); break; }
                    if (ui.Question != null) ui.Answer(1);
                }
                else if (row.Control is Toggle) Click(ui, row, 0);
                else if (slider != null)
                {
                    var part = slider.Parts[0];
                    slider.OnDrag(ui, 0, new Point(part.X + 12 + (part.Width - 24) * 0.8, part.Y + part.Height / 2));
                }
            }
            Keep(ui, name, "every row", "something other than the default", "a click or a drag on each row");
            Go(ui, page, pad, "");
            ui.PressDefaults();
            if (ui.Question == null) { faults.Add(name + ": the Defaults button asked nothing"); return; }
            string asked = Clean(ui.Question.Text);
            ui.Answer(0);
            Keep(ui, name, "Defaults button", "the defaults again", "Defaults, then \"Reset\" to \"" + asked + "\"");
        }

        // ---- the run ---------------------------------------------------------------------------

        public static int Run(string baseFolder, string target, List<string> discs)
        {
            baseDir = baseFolder;
            folder = Path.GetFullPath(target);
            iniPath = Path.Combine(folder, "ChiliNative.ini");
            launcherIni = Path.Combine(baseDir, "launcher.ini");
            if (!File.Exists(iniPath)) { Console.Error.WriteLine("No ChiliNative.ini in " + folder + " (the game writes one with --write-default-ini)."); return 2; }
            startIni = File.ReadAllBytes(iniPath);
            startLines = System.Text.Encoding.UTF8.GetString(startIni).Replace("\r", "").Split('\n');
            foreach (var old in Directory.GetFiles(folder, "???.ini")) File.Delete(old);
            string kept = Path.Combine(folder, "transitions");
            if (Directory.Exists(kept)) Directory.Delete(kept, true);
            Directory.CreateDirectory(kept);
            File.WriteAllBytes(Path.Combine(kept, "start.ini"), startIni);
            Model.SaveObserver = Saved;

            // The game program is read once here; what it knows is then in launcher.ini for every later start.
            var model = new Model(baseDir, folder);
            model.InspectGame();
            if (!model.Game.Exists || !model.Game.Probed) { Console.Error.WriteLine("The game program was not found or could not be read: " + model.Game.ExePath); return 2; }
            model.Config.Save();
            startLauncherIni = File.ReadAllBytes(launcherIni);
            Console.WriteLine("game program: " + model.Game.ExePath + (model.Game.Version.Length > 0 ? " (" + model.Game.Version + ")" : ""));

            // The rows there are, page by page, with what is under Advanced.
            var places = new List<Place>();
            var kinds = new Dictionary<Place, string>();
            var seen = Fresh();
            var tabs = new[] { new { Page = "Display", Pad = false }, new { Page = "Graphics", Pad = false }, new { Page = "Controls", Pad = false }, new { Page = "Controls", Pad = true },
                               new { Page = "Textures & mods", Pad = false }, new { Page = "Sound", Pad = false }, new { Page = "Game data", Pad = false } };
            var rowsText = new List<string>();
            foreach (var tab in tabs)
            {
                Go(seen, tab.Page, tab.Pad, "");
                foreach (var row in seen.RowsNow)
                {
                    string kind = row.Control is Segmented ? "choices" : row.Control is Toggle ? "switch" : row.Control is Slider ? "slider" : row.Control is Dropdown ? "list" :
                                  row.Control is Bindings ? "binding" : row is ModRow ? "mod" : row.Control is Buttons ? "buttons" : null;
                    var at = new Place { Page = tab.Page, Pad = tab.Pad, Id = row.Id };
                    rowsText.Add(at.Name + "\t" + row.Id + "\t" + Clean(row.Label) + "\t" + (kind ?? "(nothing to set)"));
                    if (kind == null || row is TabsRow) continue;
                    places.Add(at);
                    kinds[at] = kind;
                }
            }
            File.WriteAllLines(Path.Combine(folder, "rows.tsv"), rowsText);

            foreach (var at in places)
            {
                switch (kinds[at])
                {
                    case "choices": Choices(at); break;
                    case "switch": Switch(at); break;
                    case "slider": Slide(at); break;
                    case "list": ListChoices(at); break;
                    case "mod": Mod(at); break;
                }
            }
            Sweep(places.Where(p => kinds[p] == "slider").ToList());
            Keys();
            MouseInputs();
            PadButtons();
            Unbind(false);
            Unbind(true);
            foreach (var tab in tabs) if (tab.Page != "Game data") DefaultsOf(tab.Page, tab.Pad, places.Where(p => kinds[p] != "binding" && kinds[p] != "mod" && kinds[p] != "list" && kinds[p] != "buttons").ToList());

            // The disc: what the two Choose buttons hand on once the file picker has answered.
            foreach (var disc in discs)
            {
                var ui = Fresh();
                var result = ui.Model.ChooseDisc(disc);
                if (!result.Ok) { Skip("Game data", "Disc", disc, "not taken: " + result.Message); continue; }
                Keep(ui, "Game data", "Disc", disc, "Choose, then this path from the file picker", S.DiscPath);
            }

            File.WriteAllBytes(iniPath, startIni);
            File.WriteAllBytes(launcherIni, startLauncherIni);
            File.WriteAllLines(Path.Combine(folder, "list.tsv"), new[] { "file\tpage\trow\tvalue\thow\tline the file must hold\tlive lines the file at the start did not have" }.Concat(list));
            File.WriteAllLines(Path.Combine(folder, "skipped.txt"), skipped);
            File.WriteAllLines(Path.Combine(folder, "faults.txt"), faults);
            Model.SaveObserver = null;
            File.WriteAllLines(Path.Combine(folder, "transitions.tsv"), transitions);
            Console.WriteLine(count + " settings files in " + folder + ", " + skipped.Count + " values not set (skipped.txt), " + faults.Count + " faults (faults.txt)");
            return faults.Count == 0 ? 0 : 1;
        }
    }
}
