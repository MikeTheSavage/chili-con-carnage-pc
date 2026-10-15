// The launcher's controls, in the game's shapes (DESIGN.md 3.3): capsule segments, the health-bar
// toggle, the combo-meter slider, drop-down, buttons, path fields and binding chips. Each control lays
// out its parts (hit areas) and draws itself; the Surface routes mouse, keyboard and gamepad to it.
using System;
using System.Collections.Generic;
using System.Linq;
using System.Windows;
using System.Windows.Media;

namespace ChiliLauncher
{
    public enum Nav { Up, Down, Left, Right, Activate, Space, Back, Delete, Open, Tab, ShiftTab }

    public struct RowState
    {
        public bool Focused;       // the row has the keyboard or gamepad focus (and the focus is shown)
        public int Sub;            // which part of the control has it
        public int Hover;          // part under the mouse, -1 for none
        public int Pressed;        // part held down, -1 for none
        public bool Enabled;
    }

    public static class Lay
    {
        public const double CardX = 330, CardY = 150, CardW = 910, CardH = 450;
        public const double LabelX = 362, ControlX = 760, ControlW = 450, RowLeft = 348, RowRight = 1222;
        // The rows start right under the page title: there is no summary line between them.
        // (Five rows, the Advanced divider and one row under it fit without scrolling: the Display page.)
        public const double RowsTop = 234, RowsBottom = 596;
        // Nothing is written under a setting's name (a design rule):
        // no hint, no note beside a group heading, no description under a mod, no "why it is locked" line.
        // The one exception is a problem the player has to act on (red, with the warning triangle). A row
        // that is locked shows a padlock after its name and says why in the message line when the player
        // tries to use it (Row.LockedReason); a row the game has no setting for is not there at all.
    }

    public abstract class Control
    {
        public readonly List<Rect> Parts = new List<Rect>();
        public Func<bool> EnabledFn;
        public bool Enabled { get { return EnabledFn == null || EnabledFn(); } }
        /// <summary>True when Left and Right walk through the parts (chips, buttons) instead of changing a value.</summary>
        public virtual bool HasSubFocus { get { return false; } }
        public virtual double ExtraHeight { get { return 0; } }
        public abstract void Layout(Surface ui, double x, double y, double h);
        public abstract void Draw(DrawingContext dc, Surface ui, RowState st);
        public virtual bool OnNav(Surface ui, Nav nav, bool shift, ref int sub) { return false; }
        public virtual void OnClick(Surface ui, int part, Point p, bool right) { }
        public virtual void OnDrag(Surface ui, int part, Point p) { }
        public virtual bool PartEnabled(int part) { return true; }
        /// <summary>Why a part that is dimmed cannot be used, or null: said when the player clicks it.</summary>
        public virtual string PartReason(int part) { return null; }
        /// <summary>Why something dimmed at this place, that is no part of the control, cannot be changed; null elsewhere.</summary>
        public virtual string ReasonAt(Point p) { return null; }

        protected void Chip(DrawingContext dc, Surface ui, int part, Rect r, bool on, bool hover, bool pressed, bool enabled)
        {
            double t = ui.Animate(Tuple.Create((object)this, part), hover && !pressed ? 1 : 0, 80);
            Color fill = on ? Pal.Flame : Pal.Ink;
            double alpha = on ? 1.0 : 0.45;
            if (pressed) { fill = Pal.RustDark; alpha = 1.0; }
            else { fill = Pal.Lighten(fill, 0.12 * t); alpha = on ? 1.0 : 0.45 + 0.15 * t; }
            var pen = on ? Pal.P(Pal.Ink, 2) : Pal.P(Pal.Cream, 2, 0.35 + 0.25 * t);
            Art.Capsule(dc, r, Pal.B(fill, alpha), pen);
        }
    }

    public sealed class Option
    {
        public string Label, Value;
        public Func<bool> EnabledFn;
        /// <summary>Animated flames rise from this choice while it is the selected one.</summary>
        public bool Flames;
        /// <summary>When set and false the choice is not drawn at all (Quality's "Custom" until it applies).</summary>
        public Func<bool> VisibleFn;
        /// <summary>Why the choice is dimmed: said in the message line when the player clicks it.</summary>
        public Func<string> Reason;
        /// <summary>In a list where more than one thing can be on (texture packs): true while this one is. It gets a tick.</summary>
        public Func<bool> OnFn;
        public Option(string label, string value, Func<bool> enabled = null) { Label = label; Value = value; EnabledFn = enabled; }
        public bool Visible { get { return VisibleFn == null || VisibleFn(); } }
        public bool Enabled { get { return Visible && (EnabledFn == null || EnabledFn()); } }
    }

    /// <summary>Capsule segments: one of a few choices.</summary>
    public sealed class Segmented : Control
    {
        public readonly List<Option> Options = new List<Option>();
        public Func<string> Get;
        public Action<string> Set;
        public double SegHeight = 32;
        /// <summary>What a choice is wider than its word.</summary>
        public double Pad = 28;
        /// <summary>A value shown after the choices in gold, the way a slider shows its number (the size in pixels of "3x").</summary>
        public Func<string> Readout;
        double readoutX, readoutY;
        public Segmented(Func<string> get, Action<string> set, params Option[] options) { Get = get; Set = set; Options.AddRange(options); }

        int Selected()
        {
            string v = Get();
            for (int i = 0; i < Options.Count; i++) if (Model.Equivalent(Options[i].Value, v)) return i;
            return -1;
        }
        public override bool PartEnabled(int part) { return part >= 0 && part < Options.Count && Options[part].Enabled; }
        public override string PartReason(int part)
        {
            if (part < 0 || part >= Options.Count || !Options[part].Visible || Options[part].Enabled || Options[part].Reason == null) return null;
            return Options[part].Reason();
        }

        public override void Layout(Surface ui, double x, double y, double h)
        {
            Parts.Clear();
            double cx = x, top = y + (h - SegHeight) / 2;
            foreach (var o in Options)
            {
                // A choice that is not shown keeps its place in the list of parts, with nothing to hit.
                if (!o.Visible) { Parts.Add(Rect.Empty); continue; }
                double w = Math.Ceiling(Tx.Width(o.Label, Fonts.Semi, 15)) + Pad;
                Parts.Add(new Rect(cx, top, w, SegHeight));
                cx += w + 8;
            }
            readoutX = cx + 6; readoutY = top + (SegHeight - 22) / 2;
        }
        public override void Draw(DrawingContext dc, Surface ui, RowState st)
        {
            int sel = Selected();
            for (int i = 0; i < Options.Count; i++)
            {
                var r = Parts[i];
                if (r.IsEmpty) continue;
                bool on = i == sel, en = st.Enabled && Options[i].Enabled;
                bool pressed = en && st.Pressed == i, hover = en && st.Hover == i;
                if (pressed) r.Offset(1, 1);
                // The choice that burns is drawn in the fire's own layer: flames, the ember capsule and its name.
                if (on && en && Options[i].Flames) { ui.Burn(Parts[i], Options[i].Label, pressed); continue; }
                if (!Options[i].Enabled && st.Enabled) dc.PushOpacity(0.45);
                Chip(dc, ui, i, r, on, hover, pressed, en);
                Color text = on && !pressed ? Pal.Ink : (hover ? Pal.PaleYellow : Pal.Cream);
                Tx.DrawCentered(dc, Options[i].Label, Fonts.Semi, 15, Pal.B(text), r, -1);
                if (!Options[i].Enabled && st.Enabled) dc.Pop();
            }
            string value = Readout != null ? Readout() : null;
            if (!string.IsNullOrEmpty(value)) Tx.Draw(dc, value, Fonts.Semi, 15, Pal.B(Pal.Gold), readoutX, readoutY);
        }
        /// <summary>True when there is another choice A could go to.</summary>
        public bool HasNext
        {
            get
            {
                int sel = Selected();
                for (int i = 0; i < Options.Count; i++) if (i != sel && Options[i].Enabled) return true;
                return false;
            }
        }
        public override bool OnNav(Surface ui, Nav nav, bool shift, ref int sub)
        {
            int sel = Selected();
            if (nav == Nav.Left || nav == Nav.Right)
            {
                int step = nav == Nav.Left ? -1 : 1, dimmed = -1;
                int i = sel < 0 ? (step > 0 ? -1 : Options.Count) : sel;
                for (i += step; i >= 0 && i < Options.Count; i += step)
                {
                    if (Options[i].Enabled) { Set(Options[i].Value); return true; }
                    if (dimmed < 0 && Options[i].Visible) dimmed = i;
                }
                // Nothing further that way can be chosen: a dimmed choice the player ran into says why.
                if (dimmed >= 0) ui.SayPartReason(this, dimmed);
                return true;
            }
            if (nav == Nav.Activate || nav == Nav.Space)
            {
                // The next choice round the row; never the selected one again (on Textures that would pick a pack anew).
                for (int k = 1; k < Options.Count; k++)
                {
                    int i = ((sel < 0 ? -1 : sel) + k) % Options.Count;
                    if (Options[i].Enabled && i != sel) { Set(Options[i].Value); return true; }
                }
                // No other choice can be chosen: the dimmed one says why.
                for (int i = 0; i < Options.Count; i++)
                    if (Options[i].Visible && !Options[i].Enabled) { ui.SayPartReason(this, i); break; }
                return true;
            }
            return false;
        }
        public override void OnClick(Surface ui, int part, Point p, bool right)
        {
            // A click on the choice that is selected already changes nothing (on Textures it would otherwise
            // pick a pack again and switch the others off).
            if (!right && PartEnabled(part) && part != Selected()) Set(Options[part].Value);
        }
    }

    /// <summary>The health bar as a switch.</summary>
    public sealed class Toggle : Control
    {
        public Func<bool> Get;
        public Action<bool> Set;
        public Toggle(Func<bool> get, Action<bool> set) { Get = get; Set = set; }
        public static void DrawSwitch(DrawingContext dc, Surface ui, object key, Rect r, bool on, bool hover, bool pressed)
        {
            double t = ui.Animate(key, on ? 1 : 0, 100);
            Art.Capsule(dc, r, Pal.B(Pal.Ink, hover ? 0.62 : 0.5), Pal.P(Pal.Ink, 2));
            if (t > 0.01)
            {
                dc.PushOpacity(t);
                Art.Capsule(dc, r, Pal.B(hover ? Pal.Lighten(Pal.HealthRed, 0.12) : Pal.HealthRed), Pal.P(Pal.Ink, 2));
                var inner = new Rect(r.X + 2.5, r.Y + 2.5, r.Width - 5, r.Height - 5);
                Art.Capsule(dc, inner, null, Pal.P(Pal.Flame, 3));
                dc.Pop();
            }
            double kx = r.X + 14 + (r.Width - 28) * t;
            dc.DrawEllipse(Pal.B(pressed ? Pal.PaleYellow : Pal.Cream), Pal.P(Pal.Ink, 2), new Point(kx, r.Y + r.Height / 2), 12, 12);
        }
        public override void Layout(Surface ui, double x, double y, double h)
        {
            Parts.Clear();
            Parts.Add(new Rect(x, y + (h - 28) / 2, 64, 28));
        }
        public override void Draw(DrawingContext dc, Surface ui, RowState st)
        {
            DrawSwitch(dc, ui, this, Parts[0], Get(), st.Enabled && st.Hover == 0, st.Enabled && st.Pressed == 0);
            Tx.Draw(dc, Get() ? "On" : "Off", Fonts.Semi, 15, Pal.B(Get() ? Pal.PaleYellow : Pal.Cream, Get() ? 1 : 0.7), Parts[0].Right + 12, Parts[0].Y + 3);
        }
        public override bool OnNav(Surface ui, Nav nav, bool shift, ref int sub)
        {
            if (nav == Nav.Activate || nav == Nav.Space) { Set(!Get()); return true; }
            if (nav == Nav.Left) { if (Get()) Set(false); return true; }
            if (nav == Nav.Right) { if (!Get()) Set(true); return true; }
            return false;
        }
        public override void OnClick(Surface ui, int part, Point p, bool right) { if (!right) Set(!Get()); }
    }

    public sealed class Mark
    {
        public double Value; public string Label;
        public Mark(double v, string l) { Value = v; Label = l; }
    }

    /// <summary>The combo meter as a slider.</summary>
    public sealed class Slider : Control
    {
        public double Min, Max, Step;
        public bool Log;
        public double Curve = 1;           // above 1 gives the low end of the track more room
        public Func<double> Get;
        public Action<double> Set;
        public Func<double, string> Show = v => Model.Num(v, "0.00");
        public readonly List<Mark> Marks = new List<Mark>();
        public double TrackWidth = 380;
        /// <summary>A button after the value (Rumble's "Test"): part 1; A or Enter presses it.</summary>
        public Btn Button;
        public Slider(double min, double max, double step, Func<double> get, Action<double> set) { Min = min; Max = max; Step = step; Get = get; Set = set; }
        public override bool PartEnabled(int part) { return part != 1 || Button == null || Button.EnabledFn == null || Button.EnabledFn(); }
        public override string PartReason(int part) { return part == 1 && Button != null && !PartEnabled(1) && Button.Reason != null ? Button.Reason() : null; }

        public override double ExtraHeight { get { return Marks.Count > 0 ? 14 : 0; } }
        double ToT(double v)
        {
            v = Math.Max(Min, Math.Min(Max, v));
            double t = Log ? Math.Log(v / Min) / Math.Log(Max / Min) : (v - Min) / (Max - Min);
            return Curve == 1 ? t : Math.Pow(t, Curve);
        }
        double FromT(double t)
        {
            t = Math.Max(0, Math.Min(1, t));
            if (Curve != 1) t = Math.Pow(t, 1 / Curve);
            double v = Log ? Min * Math.Pow(Max / Min, t) : Min + (Max - Min) * t;
            v = Math.Round(v / Step) * Step;
            return Math.Max(Min, Math.Min(Max, Math.Round(v, 6)));
        }
        Rect track;
        public override void Layout(Surface ui, double x, double y, double h)
        {
            Parts.Clear();
            double cy = y + (h - ExtraHeight) / 2;
            track = new Rect(x + 4, cy - 5, TrackWidth, 10);
            Parts.Add(new Rect(x - 8, cy - 16, TrackWidth + 24, 32));
            if (Button != null)
            {
                double w = Buttons.WidthOf(Button.Label);
                Parts.Add(new Rect(x + Lay.ControlW - w, cy - 17, w, 34));
            }
        }
        public override void Draw(DrawingContext dc, Surface ui, RowState st)
        {
            double v = Get(), t = ToT(v);
            bool hover = st.Enabled && st.Hover == 0, pressed = st.Enabled && st.Pressed == 0;
            dc.DrawRoundedRectangle(Pal.B(Pal.Ink, 0.55), Pal.P(Pal.Ink, 2), track, 5, 5);
            if (t > 0.001)
            {
                var fill = new Rect(track.X, track.Y, Math.Max(10, track.Width * t), track.Height);
                dc.DrawRoundedRectangle(Pal.ComboFill(track.X, track.Width), Pal.P(Pal.Ink, 1.5), fill, 5, 5);
            }
            foreach (var m in Marks)
            {
                double mx = track.X + track.Width * ToT(m.Value);
                dc.DrawRectangle(Pal.B(Pal.Cream, 0.8), null, new Rect(mx - 1, track.Bottom + 6, 2, 8));
                var ft = Tx.Make(m.Label, Fonts.Ui, 11, Pal.B(Pal.Cream, 0.8));
                dc.DrawText(ft, new Point(mx - ft.Width / 2, track.Bottom + 15));
            }
            double tx = track.X + track.Width * t;
            dc.DrawEllipse(Pal.B(pressed ? Pal.PaleYellow : hover ? Pal.White : Pal.Cream), Pal.P(Pal.Ink, 3), new Point(tx, track.Y + 5), 11, 11);
            Tx.Draw(dc, Show(v), Fonts.Semi, 15, Pal.B(Pal.Gold), track.Right + 18, track.Y - 6);
            if (Button != null)
            {
                bool en = st.Enabled && PartEnabled(1);
                Buttons.DrawButton(dc, Parts[1], Button.Label, en && st.Hover == 1, en && st.Pressed == 1, false, PartEnabled(1), ui, Tuple.Create((object)this, 1));
            }
        }
        public override bool OnNav(Surface ui, Nav nav, bool shift, ref int sub)
        {
            if ((nav == Nav.Activate || nav == Nav.Space) && Button != null) { if (PartEnabled(1)) Button.Action(); else ui.SayPartReason(this, 1); return true; }
            if (nav != Nav.Left && nav != Nav.Right) return false;
            int dir = nav == Nav.Left ? -1 : 1;
            double v = Get();
            if (Log)
            {
                double next = FromT(ToT(v) + dir * (shift ? 0.1 : 0.02));
                if (Math.Abs(next - v) < Step / 2) next = Math.Max(Min, Math.Min(Max, v + dir * Step));
                Set(Math.Round(next, 6));
            }
            else Set(Math.Max(Min, Math.Min(Max, Math.Round((Math.Round(v / Step) + dir * (shift ? 10 : 1)) * Step, 6))));
            return true;
        }
        public override void OnClick(Surface ui, int part, Point p, bool right)
        {
            if (right) return;
            if (part == 1) { if (Button != null && PartEnabled(1)) Button.Action(); }
            else OnDrag(ui, part, p);
        }
        public override void OnDrag(Surface ui, int part, Point p) { if (part == 0) Set(FromT((p.X - track.X) / track.Width)); }
    }

    /// <summary>A capsule that opens a list.</summary>
    public sealed class Dropdown : Control
    {
        public readonly List<Option> Options = new List<Option>();
        public Func<string> Get;
        public Action<string> Set;
        public string Placeholder = "Choose";
        /// <summary>What the capsule shows instead of the selected option's name (two packs on: "b + 1 more").</summary>
        public Func<string> Text;
        public double MinWidth = 180;
        public Dropdown(Func<string> get, Action<string> set, IEnumerable<Option> options) { Get = get; Set = set; Options.AddRange(options); }
        public int Selected()
        {
            string v = Get();
            for (int i = 0; i < Options.Count; i++) if (Model.Equivalent(Options[i].Value, v)) return i;
            return -1;
        }
        public override void Layout(Surface ui, double x, double y, double h)
        {
            Parts.Clear();
            double w = MinWidth;
            foreach (var o in Options) w = Math.Max(w, Tx.Width(o.Label, Fonts.Semi, 15) + 56);
            string shown = Text != null ? Text() : null;
            if (!string.IsNullOrEmpty(shown)) w = Math.Max(w, Tx.Width(shown, Fonts.Semi, 15) + 56);
            Parts.Add(new Rect(x, y + (h - 32) / 2, Math.Min(w, Lay.ControlW), 32));
        }
        public override void Draw(DrawingContext dc, Surface ui, RowState st)
        {
            var r = Parts[0];
            bool hover = st.Enabled && st.Hover == 0, pressed = st.Enabled && st.Pressed == 0, open = ui.IsDropdownOpen(this);
            if (pressed) r.Offset(1, 1);
            Chip(dc, ui, 0, r, false, hover || open, pressed, st.Enabled);
            int sel = Selected();
            string shown = Text != null ? Text() : null;
            if (string.IsNullOrEmpty(shown)) shown = sel >= 0 ? Options[sel].Label : Placeholder;
            Tx.Draw(dc, shown, Fonts.Semi, 15, Pal.B(hover ? Pal.PaleYellow : Pal.Cream), r.X + 14, r.Y + 5, r.Width - 44, 1);
            if (open) Art.TriangleUp(dc, r.Right - 18, r.Y + r.Height / 2, 10, Pal.Cream);
            else Art.TriangleDown(dc, r.Right - 18, r.Y + r.Height / 2, 10, Pal.Cream);
        }
        public override bool OnNav(Surface ui, Nav nav, bool shift, ref int sub)
        {
            if (nav == Nav.Activate || nav == Nav.Space) { ui.OpenDropdown(this); return true; }
            return false;
        }
        public override void OnClick(Surface ui, int part, Point p, bool right) { if (!right) ui.OpenDropdown(this); }
    }

    public sealed class Btn
    {
        public string Label;
        public Action Action;
        public Func<bool> EnabledFn;
        /// <summary>Why the button is dimmed: said when the player presses it all the same.</summary>
        public Func<string> Reason;
        public bool IsOpen;       // the X button of a gamepad runs it
        /// <summary>The word for it in the prompts along the bottom, when the label is longer than a prompt may be.</summary>
        public string Prompt;
        public Btn(string label, Action action, bool isOpen = false) { Label = label; Action = action; IsOpen = isOpen; }
    }

    /// <summary>One or more secondary buttons, optionally after a read-only path field.</summary>
    public sealed class Buttons : Control
    {
        public readonly List<Btn> Items = new List<Btn>();
        public Func<string> Path;          // when set, a path capsule fills the space before the buttons
        public Action PathAction;          // Enter on the path: open the folder
        public bool PathOnTop;             // the path takes the whole width, the buttons sit under it
        public double Height = 34;
        public Buttons(params Btn[] items) { Items.AddRange(items); }
        public override double ExtraHeight { get { return PathOnTop ? 42 : 0; } }
        public override bool HasSubFocus { get { return Parts.Count > 1; } }
        int First { get { return Path != null ? 1 : 0; } }
        public override bool PartEnabled(int part)
        {
            int i = part - First;
            return i < 0 || i >= Items.Count || Items[i].EnabledFn == null || Items[i].EnabledFn();
        }
        public static double WidthOf(string label) { return Math.Ceiling(Tx.Width(label, Fonts.Semi, 15)) + 32; }
        public override void Layout(Surface ui, double x, double y, double h)
        {
            Parts.Clear();
            double top = y + (h - Height) / 2;
            double total = Items.Sum(b => WidthOf(b.Label)) + 8 * Math.Max(0, Items.Count - 1);
            if (Path != null && PathOnTop)
            {
                top = y + 11;
                Parts.Add(new Rect(x, top, Lay.ControlW, Height));
                top += Height + 8;
            }
            else if (Path != null)
            {
                double pw = Lay.ControlW - total - (Items.Count > 0 ? 10 : 0);
                Parts.Add(new Rect(x, top, pw, Height));
                x += pw + 10;
            }
            foreach (var b in Items)
            {
                double w = WidthOf(b.Label);
                Parts.Add(new Rect(x, top, w, Height));
                x += w + 8;
            }
        }
        public static void DrawButton(DrawingContext dc, Rect r, string label, bool hover, bool pressed, bool focus, bool enabled = true, Surface ui = null, object key = null)
        {
            if (pressed) r.Offset(1, 1);
            double t = ui != null && key != null ? ui.Animate(key, hover && !pressed ? 1 : 0, 80) : (hover ? 1 : 0);
            Color fill = pressed ? Pal.RustDark : Pal.Lighten(Pal.Ink, 0.12 * t);
            if (!enabled) dc.PushOpacity(0.45);
            Art.Capsule(dc, r, Pal.B(fill, pressed ? 1 : 0.45 + 0.15 * t), Pal.P(Pal.Cream, 2, 0.5 + 0.3 * t));
            Tx.DrawCentered(dc, label, Fonts.Semi, 15, Pal.B(hover ? Pal.PaleYellow : Pal.Cream), r, -1);
            if (!enabled) dc.Pop();
            if (focus) Art.Round(dc, new Rect(r.X - 4, r.Y - 4, r.Width + 8, r.Height + 8), r.Height / 2 + 4, null, Pal.P(Pal.Gold, 2));
        }
        public override void Draw(DrawingContext dc, Surface ui, RowState st)
        {
            if (Path != null)
            {
                var r = Parts[0];
                bool hover = st.Enabled && st.Hover == 0 && PathAction != null;
                Art.Capsule(dc, r, Pal.B(Pal.Soot, 0.6), Pal.P(Pal.Cream, 1.5, hover ? 0.5 : 0.25));
                string p = Path();
                bool none = string.IsNullOrEmpty(p);
                string shown = Tx.MiddleEllipsis(none ? "not chosen yet" : p, Fonts.Ui, 14, r.Width - 28);
                Tx.Draw(dc, shown, Fonts.Ui, 14, Pal.B(Pal.Cream, none ? 0.5 : 1), r.X + 14, r.Y + 7);
                if (st.Focused && st.Sub == 0 && HasSubFocus) Art.Round(dc, new Rect(r.X - 4, r.Y - 4, r.Width + 8, r.Height + 8), r.Height / 2 + 4, null, Pal.P(Pal.Gold, 2));
            }
            for (int i = 0; i < Items.Count; i++)
            {
                int part = First + i;
                bool en = st.Enabled && PartEnabled(part);
                DrawButton(dc, Parts[part], Items[i].Label, en && st.Hover == part, en && st.Pressed == part, st.Focused && st.Sub == part && HasSubFocus, PartEnabled(part), ui, Tuple.Create((object)this, part));
            }
        }
        /// <summary>What A does on this part, for the prompts: the button's own name without its dots, "Open" on the path field.</summary>
        public string Verb(int part)
        {
            part = Math.Max(0, Math.Min(Math.Max(0, Parts.Count - 1), part));
            int i = part - First;
            if (i < 0) return PathAction != null ? "Open" : null;
            if (i >= Items.Count || !PartEnabled(part)) return null;
            return Items[i].Prompt ?? Items[i].Label.TrimEnd('.');
        }
        void Run(int part)
        {
            int i = part - First;
            if (i < 0) { if (PathAction != null) PathAction(); return; }
            if (i < Items.Count && PartEnabled(part)) Items[i].Action();
        }
        public override bool OnNav(Surface ui, Nav nav, bool shift, ref int sub)
        {
            sub = Math.Max(0, Math.Min(Parts.Count - 1, sub));
            if (nav == Nav.Left) { if (sub > 0) { sub--; return true; } return false; }
            if (nav == Nav.Right) { if (sub < Parts.Count - 1) sub++; return true; }
            if (nav == Nav.Activate || nav == Nav.Space) { Run(sub); return true; }
            if (nav == Nav.Open)
            {
                for (int i = 0; i < Items.Count; i++) if (Items[i].IsOpen) { Run(First + i); return true; }
                if (PathAction != null) { PathAction(); return true; }
            }
            return false;
        }
        public override void OnClick(Surface ui, int part, Point p, bool right) { if (!right) Run(part); }
    }

    /// <summary>One chip of a binding row: a key, a mouse input or a gamepad button.</summary>
    public sealed class ChipInfo
    {
        public string Name;          // ini name: W, Mouse1, WheelUp, A, LT ...
        public bool Mouse, Pad, Fixed;
    }

    /// <summary>The chips of one action: press to rebind, "+" adds, Delete removes.</summary>
    public sealed class Bindings : Control
    {
        public readonly ActionDef Action;
        public readonly bool PadView;
        public Bindings(ActionDef action, bool padView) { Action = action; PadView = padView; }
        public override bool HasSubFocus { get { return true; } }
        public int Max { get { return Action.Toggle ? 1 : 3; } }

        public List<ChipInfo> Chips(Model m)
        {
            var list = new List<ChipInfo>();
            if (PadView)
            {
                foreach (var b in S.PadButtons)
                    if (string.Equals(m.PadTarget(b), Action.Target, StringComparison.OrdinalIgnoreCase)) list.Add(new ChipInfo { Name = b, Pad = true });
            }
            else
            {
                foreach (var k in m.KeysOf(Action.Target)) list.Add(new ChipInfo { Name = k });
                if (!Action.Toggle)
                    foreach (var i in S.MouseInputs)
                        if (string.Equals(m.MouseTarget(i), Action.Target, StringComparison.OrdinalIgnoreCase)) list.Add(new ChipInfo { Name = i, Mouse = true });
            }
            return list;
        }

        List<ChipInfo> chips = new List<ChipInfo>();
        bool hasPlus, hasFixed, hasNone;
        Rect noneRect;
        public int ChipCount { get { return chips.Count; } }
        public bool PlusPart(int part) { return hasPlus && part == chips.Count; }

        // How the controller's buttons are drawn and named: Xbox letters or PlayStation shapes (Surface.Look).
        PadKind look;
        bool create;
        string Show(ChipInfo c) { return c.Pad ? KeyNames.PadDisplay(c.Name, look, create) : KeyNames.Display(c.Name); }
        /// <summary>
        /// The words in a chip. A controller's button is its picture and its name, with two exceptions that
        /// would say the same thing twice: the four letters of an Xbox pad are the picture alone (A, not
        /// "A A"), and a PlayStation pad's L1 to R3 are the name alone, written as large as any chip's words
        /// (the small pill with the same two letters is left out: the chip is the pill).
        /// </summary>
        string ChipText(ChipInfo c)
        {
            if (c.Pad && look == PadKind.Xbox && c.Name.Length == 1) return "";
            return Show(c);
        }
        bool ChipPicture(ChipInfo c)
        {
            if (!c.Pad) return false;
            string n = c.Name.ToUpperInvariant();
            return !(look == PadKind.PlayStation && (n == "LB" || n == "RB" || n == "LT" || n == "RT" || n == "LS" || n == "RS"));
        }
        double ChipWidth(ChipInfo c)
        {
            string t = ChipText(c);
            double w = (t.Length > 0 ? Math.Ceiling(Tx.Width(t, Fonts.Semi, 15)) : 0) + 24;
            if (ChipPicture(c)) w += Art.PadGlyphWidth(c.Name, 20, look) + (t.Length > 0 ? 6 : -12);
            if (c.Mouse) w += 18;
            return Math.Max(34, w);
        }
        /// <summary>A keyboard key is a square cap wherever it is drawn; a controller's button and the mouse stay round.</summary>
        static bool IsKey(ChipInfo c) { return !c.Pad && !c.Mouse; }

        double fixedX, baseY;
        Rect fixedRect = Rect.Empty;
        public override string ReasonAt(Point p)
        {
            return hasFixed && fixedRect.Contains(p) ? "Always on the " + Action.Stick.ToLowerInvariant() + "." : null;
        }
        public override void Layout(Surface ui, double x, double y, double h)
        {
            Parts.Clear();
            fixedRect = Rect.Empty;
            chips = Chips(ui.Model);
            look = ui.Look; create = ui.PadSaysCreate;
            double top = y + (h - 30) / 2;
            baseY = top;
            var cap = ui.Capture;
            if (cap != null && cap.Control == this)
            {
                // The chip being captured takes the row: 200 wide, then Cancel and Remove.
                Parts.Add(new Rect(x, top, 200, 30));
                double cw = Math.Ceiling(Tx.Width("Cancel", Fonts.Semi, 14)) + 20;
                Parts.Add(new Rect(x + 210, top, cw + 30, 30));
                if (cap.Index < chips.Count) Parts.Add(new Rect(x + 210 + cw + 38, top, Math.Ceiling(Tx.Width("Remove", Fonts.Semi, 14)) + 26, 30));
                hasPlus = false; hasNone = false;
                return;
            }
            hasFixed = PadView && Action.Stick != null && ui.Model.Scheme != "psp";
            fixedX = x;
            if (hasFixed)
            {
                double fw = Math.Ceiling(Tx.Width(Action.Stick, Fonts.Semi, 14)) + 24;
                fixedRect = new Rect(x, top, fw, 30);
                x += fw + 8;
            }
            foreach (var c in chips)
            {
                double w = ChipWidth(c);
                Parts.Add(new Rect(x, top, w, 30));
                x += w + 8;
            }
            hasPlus = chips.Count < Max;
            // Nothing bound: a dim "none" that is no button, and the same "+" as on every other row after it.
            hasNone = chips.Count == 0 && !hasFixed;
            if (hasNone) { noneRect = new Rect(x, top, 58, 30); x += 58 + 8; }
            if (hasPlus) Parts.Add(new Rect(x, top, 34, 30));
        }

        public override void Draw(DrawingContext dc, Surface ui, RowState st)
        {
            var cap = ui.Capture;
            if (cap != null && cap.Control == this) { DrawCapture(dc, ui, st, cap); return; }
            if (hasFixed)
            {
                double w = Math.Ceiling(Tx.Width(Action.Stick, Fonts.Semi, 14)) + 24;
                var r = new Rect(fixedX, baseY, w, 30);
                Art.Capsule(dc, r, Pal.B(Pal.Soot, 0.5), Pal.P(Pal.Cream, 1.5, 0.25));
                Tx.DrawCentered(dc, Action.Stick, Fonts.Semi, 14, Pal.B(Pal.Cream, 0.75), r, -1);
            }
            for (int i = 0; i < chips.Count; i++)
            {
                var r = Parts[i];
                var c = chips[i];
                bool hover = st.Enabled && st.Hover == i, pressed = st.Enabled && st.Pressed == i;
                if (pressed) r.Offset(1, 1);
                bool key = IsKey(c);
                var fill = Pal.B(pressed ? Pal.RustDark : hover ? Pal.Lighten(Pal.Soot, 0.12) : Pal.Soot, 0.9);
                if (key) Art.Round(dc, r, Art.KeyCorner, fill, Pal.P(Pal.Cream, 1.5, hover ? 0.9 : 0.6));
                else Art.Capsule(dc, r, fill, Pal.P(Pal.Cream, 1.5, hover ? 0.9 : 0.6));
                string name = ChipText(c);
                double tx = r.X + 12;
                if (ChipPicture(c))
                {
                    // Alone in its chip the picture stands in the middle.
                    double gw = Art.PadGlyphWidth(c.Name, 20, look);
                    double gx = name.Length > 0 ? r.X + 6 : r.X + (r.Width - gw) / 2;
                    Art.PadGlyph(dc, c.Name, gx, r.Y + 5, 20, 1, look);
                    tx = gx + gw + 6;
                }
                if (c.Mouse) { Art.MouseGlyph(dc, c.Name, r.X + 10, r.Y + 6, 18); tx += 18; }
                if (name.Length > 0) Tx.Draw(dc, name, Fonts.Semi, 15, Pal.B(hover ? Pal.PaleYellow : Pal.Cream), tx, r.Y + 4);
                if (st.Focused && st.Sub == i) Art.Round(dc, new Rect(r.X - 4, r.Y - 4, r.Width + 8, r.Height + 8), key ? Art.KeyCorner + 4 : 19, null, Pal.P(Pal.Gold, 2));
            }
            if (hasNone)
            {
                var dim = new Pen(Pal.B(Pal.Cream, 0.28), 1.5) { DashStyle = new DashStyle(new double[] { 3, 2.5 }, 0) };
                Art.Capsule(dc, noneRect, null, dim);
                Tx.DrawCentered(dc, "none", Fonts.Semi, 14, Pal.B(Pal.Cream, 0.5), noneRect, -1);
            }
            if (hasPlus)
            {
                int i = chips.Count;
                var r = Parts[i];
                bool hover = st.Enabled && st.Hover == i, pressed = st.Enabled && st.Pressed == i;
                if (pressed) r.Offset(1, 1);
                var pen = new Pen(Pal.B(Pal.Cream, hover ? 0.9 : 0.5), 1.5) { DashStyle = new DashStyle(new double[] { 3, 2.5 }, 0) };
                Art.Capsule(dc, r, Pal.B(pressed ? Pal.RustDark : Pal.Ink, hover ? 0.5 : 0.25), pen);
                Tx.DrawCentered(dc, "+", Fonts.Semi, 17, Pal.B(hover ? Pal.PaleYellow : Pal.Cream, 0.9), r, -2);
                if (st.Focused && st.Sub == i) Art.Round(dc, new Rect(r.X - 4, r.Y - 4, r.Width + 8, r.Height + 8), 19, null, Pal.P(Pal.Gold, 2));
            }
        }

        void DrawCapture(DrawingContext dc, Surface ui, RowState st, CaptureState cap)
        {
            // The waiting chip: its fill and its words stand still and are drawn here. The pulsing outline and
            // the five seconds draining round the small dial are drawn by the Surface in a visual of their
            // own (ui.Waiting), so that waiting does not draw the whole picture again and again.
            var r = Parts[0];
            Art.Capsule(dc, r, Pal.B(Pal.Soot, 0.95), null);
            string text = cap.Note ?? (PadView ? "Press a button..." : Action.Toggle ? "Press a key..." : "Press a key or click...");
            Tx.DrawCentered(dc, text, Fonts.Semi, text.Length > 22 ? 13 : 15, Pal.B(Pal.Gold), r, -1);
            // Cancel and Remove: buttons, drawn as the other small buttons are.
            var c = Parts[1];
            bool hover = st.Hover == 1;
            Art.Capsule(dc, c, Pal.B(Pal.Ink, hover ? 0.6 : 0.45), Pal.P(Pal.Cream, 1.5, hover ? 0.9 : 0.5));
            Tx.Draw(dc, "Cancel", Fonts.Semi, 14, Pal.B(hover ? Pal.PaleYellow : Pal.Cream), c.X + 13, c.Y + 5);
            var centre = new Point(c.Right - 18, c.Y + 15);
            dc.DrawEllipse(null, Pal.P(Pal.Cream, 2, 0.25), centre, 7, 7);
            ui.Waiting(r, centre, 7);
            if (Parts.Count > 2)
            {
                var rm = Parts[2];
                bool over = st.Hover == 2;
                Art.Capsule(dc, rm, Pal.B(Pal.Ink, over ? 0.6 : 0.45), Pal.P(Pal.Cream, 1.5, over ? 0.9 : 0.5));
                Tx.DrawCentered(dc, "Remove", Fonts.Semi, 14, Pal.B(over ? Pal.PaleYellow : Pal.Cream), rm, -1);
            }
        }

        public override bool OnNav(Surface ui, Nav nav, bool shift, ref int sub)
        {
            int count = Parts.Count;
            if (count == 0) return false;
            sub = Math.Max(0, Math.Min(count - 1, sub));
            if (nav == Nav.Left) { if (sub > 0) { sub--; return true; } return false; }
            if (nav == Nav.Right) { if (sub < count - 1) sub++; return true; }
            if (nav == Nav.Activate || nav == Nav.Space) { ui.BeginCapture(this, sub); return true; }
            if (nav == Nav.Delete || nav == Nav.Open)
            {
                if (sub < chips.Count) { ui.RemoveBinding(this, chips[sub]); return true; }
            }
            return false;
        }
        public override void OnClick(Surface ui, int part, Point p, bool right)
        {
            if (right) { if (part < chips.Count) ui.RemoveBinding(this, chips[part]); return; }
            ui.BeginCapture(this, part);
        }
        public ChipInfo ChipAt(int index) { return index >= 0 && index < chips.Count ? chips[index] : null; }
    }

    // ---- rows ------------------------------------------------------------------------------------

    public class Row
    {
        public string Id = "";
        public string Label = "";
        public Func<string> Problem;          // under the name, in red with a warning triangle: something the player must act on
        public Func<bool> ChangedFn;          // rows that write more than one key
        public Control Control;
        public Setting[] Settings = new Setting[0];
        public Func<bool> EnabledFn;
        /// <summary>Why the row is locked: said in the message line when the player tries to use it.</summary>
        public Func<string> Reason;
        public double Height = 56;
        public double Top;
        /// <summary>Where the row's top was drawn last, or NaN when it was not drawn.</summary>
        public double ShownY = double.NaN;
        public double Right = Lay.RowRight;
        public bool Compact;                  // binding rows: 44 px
        /// <summary>The row stands under the page's title and does not move with the list (the Controls page's tabs).</summary>
        public bool Pinned;
        public virtual bool Focusable { get { return Control != null; } }
        /// <summary>Tests: where the padlock was drawn last, or Rect.Empty when the row showed none.</summary>
        public Rect Padlock = Rect.Empty;

        /// <summary>True when the game beside the launcher has no such setting: the row is then left out.</summary>
        public bool NeedsNewer(Model m)
        {
            foreach (var s in Settings) if (!m.Game.Knows(s.Probe)) return true;
            return false;
        }
        public bool IsEnabled(Model m) { return !NeedsNewer(m) && (EnabledFn == null || EnabledFn()) && (Control == null || Control.Enabled); }
        public bool Changed(Model m)
        {
            if (ChangedFn != null && ChangedFn()) return true;
            foreach (var s in Settings) if (m.IsChanged(s)) return true;
            return false;
        }

        /// <summary>Why the row cannot be used now, or null when it can.</summary>
        public string LockedReason(Model m)
        {
            if (IsEnabled(m)) return null;
            return Reason != null ? Reason() : null;
        }

        /// <summary>The one thing that may stand under a row's name: a problem the player has to act on.</summary>
        protected string ProblemText() { return Problem != null ? Problem() : null; }

        public const double NameSlot = 10;      // from the end of the name to the padlock or the gold dot

        /// <summary>The padlock of a locked row and the gold dot of a changed one, after the name.</summary>
        protected void DrawMarks(DrawingContext dc, Model m, bool locked, double x, double midY)
        {
            Padlock = Rect.Empty;
            if (locked)
            {
                Padlock = new Rect(x + NameSlot - 4, midY - 7, 12, 13);
                Art.Padlock(dc, Padlock.X, Padlock.Y, 12);
                x += NameSlot + 10;
            }
            if (Changed(m)) dc.DrawEllipse(Pal.B(Pal.Gold), Pal.P(Pal.Ink, 1), new Point(x + NameSlot, midY), 3.5, 3.5);
        }

        public virtual void Measure(Surface ui)
        {
            if (Compact) { Height = 44; return; }
            string problem = ProblemText();
            double h = 56;
            if (!string.IsNullOrEmpty(problem))
            {
                var ft = Tx.Make(problem, Fonts.Ui, 13, Pal.B(Pal.Cream, 0.7), Lay.ControlX - Lay.LabelX - 34, 3);
                h = Math.Max(56, 30 + ft.Height + 8);
            }
            if (Control != null) h = Math.Max(h, 56 + Control.ExtraHeight);
            Height = Math.Ceiling(h);
        }

        public virtual void Layout(Surface ui, double y)
        {
            if (Control != null) Control.Layout(ui, Lay.ControlX, y, Compact ? 44 : 56 + Control.ExtraHeight);
        }

        public virtual void Draw(DrawingContext dc, Surface ui, double y, RowState st)
        {
            var m = ui.Model;
            bool enabled = IsEnabled(m);
            st.Enabled = enabled;
            if (st.Focused) ui.FocusRing(dc, new Rect(Lay.RowLeft, y - 1, Right - Lay.RowLeft, Height + 2), 8);
            string problem = ProblemText();
            Color labelColor = !enabled ? Pal.Mix(Pal.White, Pal.LockedGrey, 0.6) : st.Focused ? Pal.PaleYellow : Pal.White;
            // With nothing under it, the name sits level with the control.
            double ly = Compact ? (string.IsNullOrEmpty(problem) ? y + 9 : y + 2) : (string.IsNullOrEmpty(problem) ? y + 15 : y + 5);
            double lw = Tx.Draw(dc, Label, Fonts.Semi, Compact ? 16 : 18, Pal.B(labelColor, enabled ? 1 : 0.75), Lay.LabelX, ly);
            DrawMarks(dc, m, !enabled, Lay.LabelX + lw, ly + (Compact ? 12 : 14));
            if (!string.IsNullOrEmpty(problem))
            {
                double hx = Lay.LabelX, hy = Compact ? y + 23 : y + 30;
                Art.Warning(dc, hx, hy + 1, 14, Pal.Flame);
                var ht = Tx.Make(problem, Fonts.Ui, 13, Pal.B(Pal.Problem), Lay.ControlX - Lay.LabelX - 34, Compact ? 1 : 3);
                dc.DrawText(ht, new Point(hx + 20, hy));
            }
            if (Control != null)
            {
                // A locked row's control is dimmed and shows no focus, hover or press of its own.
                if (!enabled) { st.Focused = false; st.Hover = -1; st.Pressed = -1; dc.PushOpacity(0.45); }
                Control.Draw(dc, ui, st);
                if (!enabled) dc.Pop();
            }
        }
    }

    /// <summary>
    /// Two or three segments alone at the left edge, like tabs: what the rows below show (the Controls
    /// page's "Keyboard & mouse / Gamepad"). Not a setting, so it has no name beside it.
    /// </summary>
    public sealed class TabsRow : Row
    {
        public TabsRow(string id, Segmented tabs) { Id = id; Control = tabs; Height = 50; Pinned = true; }
        public override void Measure(Surface ui) { Height = 50; }
        public override void Layout(Surface ui, double y) { Control.Layout(ui, Lay.LabelX, y, 50); }
        public override void Draw(DrawingContext dc, Surface ui, double y, RowState st)
        {
            st.Enabled = true;
            var p = Control.Parts;
            if (st.Focused && p.Count > 0)
                ui.FocusRing(dc, new Rect(p[0].X - 6, p[0].Y - 6, p[p.Count - 1].Right - p[0].X + 12, p[0].Height + 12), p[0].Height / 2 + 6);
            Control.Draw(dc, ui, st);
        }
    }

    /// <summary>A small heading between groups of rows.</summary>
    public sealed class GroupRow : Row
    {
        public GroupRow(string text) { Label = text; Height = 38; Id = "group:" + text; }
        public override bool Focusable { get { return false; } }
        public override void Measure(Surface ui) { Height = 38; }
        public override void Layout(Surface ui, double y) { }
        public override void Draw(DrawingContext dc, Surface ui, double y, RowState st)
        {
            double w = Tx.Draw(dc, Label, Fonts.Semi, 15, Pal.B(Pal.PaleYellow), Lay.LabelX, y + 12);
            double x = Lay.LabelX + w + 12;
            dc.DrawRectangle(Pal.B(Pal.Cream, 0.22), null, new Rect(x, y + 23, Math.Max(0, Right - 12 - x), 1.5));
        }
    }

    /// <summary>The "Advanced" divider: closed by default, opens the rows under it.</summary>
    public sealed class AdvancedRow : Row
    {
        sealed class Opener : Control
        {
            readonly Action toggle;
            public Opener(Action t) { toggle = t; }
            public override void Layout(Surface ui, double x, double y, double h) { Parts.Clear(); Parts.Add(new Rect(Lay.RowLeft, y, Lay.RowRight - Lay.RowLeft, 34)); }
            public override void Draw(DrawingContext dc, Surface ui, RowState st) { }
            public override bool OnNav(Surface ui, Nav nav, bool shift, ref int sub)
            {
                if (nav == Nav.Activate || nav == Nav.Space) { toggle(); return true; }
                return false;
            }
            public override void OnClick(Surface ui, int part, Point p, bool right) { if (!right) toggle(); }
        }
        readonly Func<bool> open;
        public bool IsOpen { get { return open(); } }
        public AdvancedRow(Func<bool> isOpen, Action toggle) { Label = "Advanced"; Id = "advanced"; open = isOpen; Control = new Opener(toggle); Height = 34; }
        public override void Measure(Surface ui) { Height = 34; }
        public override void Layout(Surface ui, double y) { Control.Layout(ui, 0, y, 34); }
        public override void Draw(DrawingContext dc, Surface ui, double y, RowState st)
        {
            if (st.Focused) ui.FocusRing(dc, new Rect(Lay.RowLeft, y + 1, Right - Lay.RowLeft, Height - 2), 8);
            bool hover = st.Hover == 0;
            Color c = hover || st.Focused ? Pal.PaleYellow : Pal.Cream;
            if (open()) Art.TriangleDown(dc, Lay.LabelX + 6, y + 19, 11, c);
            else Art.TriangleRight(dc, Lay.LabelX + 6, y + 18, 11, c);
            double w = Tx.Draw(dc, "Advanced", Fonts.Semi, 15, Pal.B(c), Lay.LabelX + 20, y + 8);
            double x = Lay.LabelX + 20 + w + 12;
            dc.DrawRectangle(Pal.B(Pal.Cream, 0.22), null, new Rect(x, y + 19, Math.Max(0, Right - 12 - x), 1.5));
        }
    }

    /// <summary>A row drawn by its page (the Play page's summary, notes, the controller header).</summary>
    public sealed class CustomRow : Row
    {
        public Action<DrawingContext, Surface, double> Paint;
        public CustomRow(string id, double height, Action<DrawingContext, Surface, double> paint) { Id = id; Height = height; Paint = paint; }
        public override bool Focusable { get { return false; } }
        public override void Measure(Surface ui) { }
        public override void Layout(Surface ui, double y) { }
        public override void Draw(DrawingContext dc, Surface ui, double y, RowState st) { Paint(dc, ui, y); }
    }

    /// <summary>One mod of the list: switch, name, up and down. An empty folder, or one that cannot be read, is locked.</summary>
    public sealed class ModRow : Row
    {
        sealed class ModControl : Control
        {
            public override bool HasSubFocus { get { return true; } }
            readonly ModRow row;
            public ModControl(ModRow r) { row = r; }
            public override void Layout(Surface ui, double x, double y, double h)
            {
                Parts.Clear();
                Parts.Add(new Rect(Lay.LabelX + 2, y + 8, 64, 28));
                Parts.Add(new Rect(row.Right - 78, y + 8, 30, 28));
                Parts.Add(new Rect(row.Right - 44, y + 8, 30, 28));
            }
            public override bool PartEnabled(int part)
            {
                if (part == 1) return row.Index > 0;
                if (part == 2) return row.Index < row.Count - 1;
                return true;
            }
            public override void Draw(DrawingContext dc, Surface ui, RowState st) { }
            public override bool OnNav(Surface ui, Nav nav, bool shift, ref int sub)
            {
                sub = Math.Max(0, Math.Min(2, sub));
                if (nav == Nav.Left) { if (sub > 0) { sub--; return true; } return false; }
                if (nav == Nav.Right) { if (sub < 2) sub++; return true; }
                if (nav == Nav.Activate || nav == Nav.Space) { row.Act(ui, sub); return true; }
                return false;
            }
            public override void OnClick(Surface ui, int part, Point p, bool right) { if (!right) row.Act(ui, part); }
        }
        public readonly ModInfo Mod;
        public int Index, Count;
        public ModRow(ModInfo mod, int index, int count)
        {
            Mod = mod; Index = index; Count = count; Id = "mod:" + mod.Name; Height = 44;
            Control = new ModControl(this);
            Settings = new[] { S.ModsEnabled };
            Label = mod.Name;
            EnabledFn = () => !Mod.Empty && !Mod.Unreadable;
            Reason = () => Mod.Unreadable ? "This folder cannot be read." : "This folder is empty.";
        }
        public override void Measure(Surface ui) { Height = 44; }
        public override void Layout(Surface ui, double y) { Control.Layout(ui, 0, y, 44); }
        void Act(Surface ui, int part)
        {
            var mods = ui.Model.Mods;
            if (!IsEnabled(ui.Model)) return;
            if (part == 0) Mod.Enabled = !Mod.Enabled;
            else if (part == 1 && Index > 0) { mods.RemoveAt(Index); mods.Insert(Index - 1, Mod); ui.MoveFocusWithRow(-1); }
            else if (part == 2 && Index < Count - 1) { mods.RemoveAt(Index); mods.Insert(Index + 1, Mod); ui.MoveFocusWithRow(1); }
            else return;
            ui.Model.CommitMods();
            ui.Rebuild();
        }
        public override void Draw(DrawingContext dc, Surface ui, double y, RowState st)
        {
            bool enabled = IsEnabled(ui.Model);
            var band = new Rect(Lay.RowLeft + 4, y, Right - Lay.RowLeft - 8, 44);
            if (Index % 2 == 1) dc.DrawRoundedRectangle(Pal.B(Pal.Ink, 0.15), null, band, 6, 6);
            if (st.Focused) ui.FocusRing(dc, new Rect(Lay.RowLeft, y - 1, Right - Lay.RowLeft, 46), 8);
            var p = Control.Parts;
            // A locked row is dimmed as a whole: switch, picture, name and arrows.
            if (!enabled) { st.Hover = -1; st.Pressed = -1; dc.PushOpacity(0.45); }
            // A folder that is empty or cannot be read loads nothing: its switch is drawn off.
            Toggle.DrawSwitch(dc, ui, Mod, p[0], Mod.Enabled && enabled, st.Hover == 0, st.Pressed == 0);
            if (enabled && st.Focused && st.Sub == 0) Art.Round(dc, new Rect(p[0].X - 4, p[0].Y - 4, p[0].Width + 8, p[0].Height + 8), 18, null, Pal.P(Pal.Gold, 2));
            double x = Lay.LabelX + 82;
            if (Mod.HasTextures)
            {
                var thumb = ui.ArtFiles.Pack(Mod.Name);
                if (thumb != null) { dc.DrawImage(thumb, new Rect(x, y + 6, 32, 32)); dc.DrawRectangle(null, Pal.P(Pal.Ink, 1.5), new Rect(x, y + 6, 32, 32)); x += 40; }
                else { Art.Picture(dc, x, y + 12, 20, Pal.Cream); x += 28; }
            }
            // The name alone, level with the switch.
            double nw = Tx.Draw(dc, Mod.Name, Fonts.Semi, 16, Pal.B(Mod.Enabled && enabled ? Pal.White : Pal.Mix(Pal.White, Pal.LockedGrey, 0.6)), x, y + 11, Right - 124 - x, 1);
            for (int i = 1; i <= 2; i++)
            {
                bool en = enabled && Control.PartEnabled(i);
                bool hover = en && st.Hover == i;
                Color c = hover ? Pal.PaleYellow : Pal.Cream;
                if (!en && enabled) dc.PushOpacity(0.25);
                if (st.Pressed == i && en) dc.PushTransform(new TranslateTransform(1, 1));
                if (i == 1) Art.TriangleUp(dc, p[i].X + 15, p[i].Y + 14, 15, c);
                else Art.TriangleDown(dc, p[i].X + 15, p[i].Y + 14, 15, c);
                if (st.Pressed == i && en) dc.Pop();
                if (!en && enabled) dc.Pop();
                if (enabled && st.Focused && st.Sub == i) Art.Round(dc, new Rect(p[i].X - 1, p[i].Y - 2, p[i].Width + 2, p[i].Height + 4), 8, null, Pal.P(Pal.Gold, 2));
            }
            if (!enabled) dc.Pop();
            // The padlock is not dimmed: it is the cue.
            Padlock = Rect.Empty;
            if (!enabled) { Padlock = new Rect(Math.Min(x + nw, Right - 124) + NameSlot - 4, y + 15, 12, 13); Art.Padlock(dc, Padlock.X, Padlock.Y, 12); }
        }
    }
}
