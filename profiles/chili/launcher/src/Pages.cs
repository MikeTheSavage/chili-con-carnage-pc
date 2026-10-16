// The seven pages (DESIGN.md section 2): which rows each has, what they say, and which key of
// ChiliNative.ini each one writes.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace ChiliLauncher
{
    public sealed partial class Surface
    {
        public const string LauncherVersion = "1.0";

        bool padView;
        readonly HashSet<string> advancedOpen = new HashSet<string>();
        string chosenPack;

        public void SetPadView(bool on) { padView = on; Rebuild(); }
        public void OpenAdvanced(string pageName) { advancedOpen.Add(pageName); Rebuild(); }

        // ---- row builders ----------------------------------------------------------------------

        Row Seg(string label, Setting s, params Option[] options)
        {
            return new Row { Id = s.Id, Label = label, Settings = new[] { s }, Control = new Segmented(() => Model.Get(s), v => Model.Set(s, v), options) };
        }
        Row Tog(string label, Setting s)
        {
            return new Row { Id = s.Id, Label = label, Settings = new[] { s }, Control = new Toggle(() => Model.GetBool(s), v => Model.Set(s, v)) };
        }
        Row Sld(string label, Setting s, double min, double max, double step, Func<double, string> show, bool log = false, string format = "0.###")
        {
            var slider = new Slider(min, max, step, () => Model.GetNum(s, min), v => Model.Set(s, Model.Num(v, format))) { Log = log };
            if (show != null) slider.Show = show;
            return new Row { Id = s.Id, Label = label, Settings = new[] { s }, Control = slider };
        }
        static string Percent(double v) { return Math.Round(v * 100) + " %"; }
        static Option O(string label, string value, Func<bool> enabled = null) { return new Option(label, value, enabled); }

        void AddAdvanced(List<Row> list, string pageName, params Row[] advanced)
        {
            // Rows the game has no setting for are left out; with none left there is nothing to open.
            var known = advanced.Where(r => !r.NeedsNewer(Model)).ToArray();
            if (known.Length == 0) return;
            list.Add(new AdvancedRow(() => advancedOpen.Contains(pageName), () =>
            {
                if (!advancedOpen.Remove(pageName)) advancedOpen.Add(pageName);
                Rebuild();
                if (advancedOpen.Contains(pageName)) scrollTarget = double.MaxValue;
            }) { Id = "advanced:" + pageName });
            if (advancedOpen.Contains(pageName)) list.AddRange(known);
        }

        void BuildPages()
        {
            // No explanatory line under a page's title, on any page (a design rule).
            pages.Add(new Page { Name = "Play", Title = () => PlayTitle(), Build = BuildPlay, Right = () => 926 });
            pages.Add(new Page { Name = "Display", Title = () => "Display", Build = BuildDisplay,
                Defaults = () => Model.ResetTo(Known(S.Fullscreen, S.WindowSize, S.InternalScale, S.FrameLock, S.EarlyPresent, S.Preset)) });
            pages.Add(new Page { Name = "Graphics", Title = () => "Graphics", Build = BuildGraphics,
                Defaults = () => { ApplyQuality("high"); Model.ResetTo(Known(S.Backend, S.GeThread)); } });
            pages.Add(new Page { Name = "Controls", Title = () => "Controls", Build = BuildControls,
                Defaults = ResetDevice });
            pages.Add(new Page { Name = "Textures & mods", Title = () => "Textures & mods", Build = BuildMods,
                Defaults = ResetMods });
            pages.Add(new Page { Name = "Sound", Title = () => "Sound", Build = BuildSound,
                Defaults = () => Model.ResetTo(Known(S.BufferMs)) });
            pages.Add(new Page { Name = "Game data", Title = () => "Game data", Build = BuildGameData });
        }

        Setting[] Known(params Setting[] settings) { return settings.Where(s => Model.Game.Knows(s.Probe)).ToArray(); }
        /// <summary>True when the game program was read and holds the setting: for rows that newer game builds add.</summary>
        bool Has(Setting s) { return Model.Game.Probed && Model.Game.Knows(s.Probe); }

        // ---- Play ------------------------------------------------------------------------------

        string PlayTitle()
        {
            if (!Model.Game.Exists) return "Game not found";
            return Model.Disc.Ok ? "Play" : "Choose your disc";
        }
        List<string> ReadyItems()
        {
            var m = Model;
            var parts = new List<string>();
            parts.Add(m.GetBool(S.Fullscreen) ? "Full screen" : "Window");
            string scaleText = m.Get(S.InternalScale).Trim().ToLowerInvariant();
            int n;
            string limit = AutoLimit(m);
            parts.Add(int.TryParse(scaleText, out n) && n >= 1 && n <= 8 ? (480 * n) + " x " + (272 * n)
                : limit != null ? "Automatic resolution \u2264 " + limit : "Automatic resolution");
            parts.Add(m.Get(S.FrameLock).Trim() == "2" ? "30 fps" : "60 fps");
            string aa = AaNow();
            parts.Add(aa == "off" ? "No anti-aliasing" : aa == "fxaa" ? "Light anti-aliasing" : aa.Replace("msaa", "") + "x anti-aliasing");
            parts.Add(Defaults.LayoutName(m.Layout) + " controls");
            int mods = m.EnabledModCount;
            bool hd = m.TexturePacks.Any(p => p.Enabled);
            if (hd) parts.Add("HD textures");
            int other = mods - m.TexturePacks.Count(p => p.Enabled);
            if (other > 0) parts.Add(other == 1 ? "1 mod" : other + " mods");
            return parts;
        }
        const string Dot = "  \u00B7  ";
        /// <summary>The items joined with dots, broken into lines so that no line ends or starts with a dot.</summary>
        static List<string> WrapItems(List<string> items, double width, Typeface face, double size)
        {
            var lines = new List<string>();
            string current = "";
            foreach (var item in items)
            {
                string longer = current.Length == 0 ? item : current + Dot + item;
                if (current.Length > 0 && Tx.Width(longer, face, size) > width) { lines.Add(current); current = item; }
                else current = longer;
            }
            if (current.Length > 0) lines.Add(current);
            return lines;
        }

        List<Row> BuildPlay()
        {
            var list = new List<Row>();
            var m = Model;
            if (!m.Game.Exists)
            {
                // The program that was looked for, and under it that it is not there: a problem, said as the others are.
                list.Add(new CustomRow("nogame", 60, (dc, ui, y) =>
                {
                    Tx.Draw(dc, Tx.MiddleEllipsis(m.Game.ExePath, Fonts.Ui, 14, 540), Fonts.Ui, 14, Pal.B(Pal.Cream), Lay.LabelX, y + 2);
                    Art.Warning(dc, Lay.LabelX, y + 28, 15);
                    Tx.Draw(dc, GameMissing, Fonts.Ui, 14, Pal.B(Pal.Problem), Lay.LabelX + 22, y + 26, 520, 1);
                }));
                return list;
            }
            if (!m.Disc.Ok)
            {
                // The title ("Choose your disc") and the two buttons say it: no paragraph between them.
                var buttons = new Buttons(new Btn("Choose .iso...", () => { if (ChooseDiscRequested != null) ChooseDiscRequested(true); }),
                                          new Btn("Choose folder...", () => { if (ChooseDiscRequested != null) ChooseDiscRequested(false); })) { Height = 40 };
                list.Add(new DiscButtonsRow { Id = "choose", Control = buttons, Height = 56, Problem = () => m.Disc.State == DiscState.Missing ? null : m.Disc.Message });
                if (!string.IsNullOrEmpty(m.Disc.Message))
                    list.Add(new CustomRow("discproblem", 84, (dc, ui, y) =>
                    {
                        // The path that was tried, and under it why it will not do.
                        bool named = !string.IsNullOrEmpty(m.Disc.Path) && !m.Disc.Message.StartsWith(m.Disc.Path);
                        if (named) Tx.Draw(dc, Tx.MiddleEllipsis(m.Disc.Path, Fonts.Ui, 14, 540), Fonts.Ui, 14, Pal.B(Pal.Cream), Lay.LabelX, y + 2);
                        double ty = named ? y + 26 : y + 2;
                        Art.Warning(dc, Lay.LabelX, ty + 2, 15);
                        Tx.Draw(dc, m.Disc.Message, Fonts.Ui, 14, Pal.B(Pal.Problem), Lay.LabelX + 22, ty, 520, 3);
                    }));
                return list;
            }
            if (!string.IsNullOrEmpty(LastError))
            {
                var said = Tx.Make(LastError, Fonts.Mono, 12, Pal.B(Pal.Cream), 516, 6);
                double boxH = said.Height + 44;
                list.Add(new CustomRow("lasterror", boxH + 6, (dc, ui, y) =>
                {
                    var box = new Rect(Lay.LabelX, y + 2, 548, boxH);
                    dc.DrawRoundedRectangle(Pal.B(Pal.Ink, 0.5), Pal.P(Pal.Chili, 1.5, 0.9), box, 10, 10);
                    Art.Warning(dc, box.X + 14, box.Y + 11, 15);
                    Tx.Draw(dc, "What the game said", Fonts.Semi, 14, Pal.B(Pal.PaleYellow), box.X + 38, box.Y + 9);
                    dc.DrawText(said, new Point(box.X + 16, box.Y + 34));
                }));
            }
            // As high as what is in it, so that after a failed start "Skip this window" is still on the page.
            int readyLines = WrapItems(ReadyItems(), 548 - 106, Fonts.Semi, 15).Count;
            list.Add(new CustomRow("ready", Math.Max(44, readyLines * 21 + 20) + 112, DrawReady));
            list.Add(new Row
            {
                Id = "direct", Label = "Skip this window next time",
                Control = new Toggle(() => m.Config.GetBool("StartGameDirectly", false), v =>
                {
                    m.Config.Set("StartGameDirectly", v ? "true" : "false");
                    m.Config.Save();
                    // Nothing is said about it: the package's second shortcut ("Chili Con Carnage settings",
                    // --settings) is the way back to this window, and Shift held at the start still shows it.
                    InvalidateVisual();
                })
            });
            return list;
        }

        public const string GameMissing = "This program was not found.";

        /// <summary>The two Choose buttons, left-aligned under the title instead of in the control column.</summary>
        sealed class DiscButtonsRow : Row
        {
            public override void Measure(Surface ui) { Height = 56; }
            public override void Layout(Surface ui, double y) { Control.Layout(ui, Lay.LabelX, y, 56); }
            public override void Draw(DrawingContext dc, Surface ui, double y, RowState st)
            {
                st.Enabled = true;
                Control.Draw(dc, ui, st);
            }
        }

        void KeyChip(DrawingContext dc, ref double x, double y, string key, string text)
        {
            double w = Math.Max(34, Tx.Width(key, Fonts.Semi, 14) + 22);
            var r = new Rect(x, y, w, 28);
            // A key is a square cap wherever the launcher draws one (the round shapes are a controller's buttons).
            Art.Round(dc, r, Art.KeyCorner, Pal.B(Pal.Soot, 0.9), Pal.P(Pal.Cream, 1.5, 0.6));
            Tx.DrawCentered(dc, key, Fonts.Semi, 14, Pal.B(Pal.Cream), r, -1);
            x += w + 8;
            x += Tx.Draw(dc, text, Fonts.Ui, 14, Pal.B(Pal.Cream, 0.9), x, y + 4) + 22;
        }

        /// <summary>Screenshots: the Play page as before the first game (no log yet).</summary>
        public bool HideLastPlayed;

        void DrawReady(DrawingContext dc, Surface ui, double y)
        {
            var m = Model;
            double left = Lay.LabelX, width = 548;
            // The "Ready" strip.
            var lines = WrapItems(ReadyItems(), width - 106, Fonts.Semi, 15);
            double sh = Math.Max(44, lines.Count * 21 + 20);
            var strip = new Rect(left, y + 2, width, sh);
            // After a failed start the box above says what the game said: the strip does not call that "Ready".
            bool failed = !string.IsNullOrEmpty(LastError);
            dc.DrawRoundedRectangle(Pal.B(Pal.Ink, 0.45), Pal.P(Pal.Cream, 1.5, failed ? 0.18 : 0.3), strip, 10, 10);
            if (!failed) Tx.Draw(dc, "Ready", Fonts.Heading, 18, Pal.B(Pal.Gold), left + 16, y + 2 + (sh - 26) / 2);
            for (int i = 0; i < lines.Count; i++)
                Tx.Draw(dc, lines[i], Fonts.Semi, 15, Pal.B(failed ? Pal.Cream : Pal.PaleYellow, failed ? 0.6 : 1), left + (failed ? 16 : 86), y + 2 + (sh - lines.Count * 21) / 2 + i * 21);
            double cy = y + sh + 20;
            // The disc.
            Art.Check(dc, left + 2, cy + 3, 15, Pal.ChalkGreen);
            double dx = left + 28;
            dx += Tx.Draw(dc, "Your disc: ", Fonts.Ui, 15, Pal.B(Pal.Cream, 0.8), dx, cy);
            Tx.Draw(dc, Tx.MiddleEllipsis(m.Disc.Path, Fonts.Ui, 15, left + width - dx), Fonts.Ui, 15, Pal.B(Pal.White), dx, cy);
            cy += 32;
            // Three reminders.
            double x = left;
            string pause = m.KeysOf("Start").FirstOrDefault(), free = m.KeysOf("ToggleMouseCapture").FirstOrDefault();
            // A key and the name of what it does, as on the Controls page.
            KeyChip(dc, ref x, cy, "F11", "Full screen");
            if (free != null) KeyChip(dc, ref x, cy, KeyNames.Display(free), "Free the mouse");
            if (pause != null) KeyChip(dc, ref x, cy, KeyNames.Display(pause) == "Escape" ? "Esc" : KeyNames.Display(pause), "Pause");
            cy += 40;
            // With no log yet there is nothing to say here, and nothing is said.
            string log = HideLastPlayed ? null : m.LatestLog();
            if (log != null)
            {
                try { Tx.Draw(dc, "Last played: " + File.GetLastWriteTime(log).ToString("d MMMM yyyy, HH:mm", System.Globalization.CultureInfo.InvariantCulture), Fonts.Ui, 14, Pal.B(Pal.Cream, 0.7), left, cy); }
                catch { }
            }
        }

        void DrawPlayPicture(DrawingContext dc)
        {
            var region = new Rect(940, 150, 300, 450);
            var hero = ArtFiles != null ? ArtFiles.Hero : null;
            if (hero == null)
            {
                Art.Emblem(dc, 1088, 372, 112);
                return;
            }
            // The load screen's left 480 of 512 columns in the card's top right corner, its left edge and its
            // foot faded out. One PSP pixel is 1.3 units at most (stretched over the card's whole height the
            // picture turns to mush), and it is darkened a little: a backdrop, not a photograph. A larger
            // copy of the same picture (art\hero.png taken from an HD pack) is shown at the same size, sharp.
            double psp = hero.PixelHeight / 272.0;                  // the picture's pixels per PSP pixel
            bool padded = Math.Abs(hero.PixelWidth / (double)hero.PixelHeight - 512.0 / 272) < 0.02;
            double srcW = padded ? Math.Round(hero.PixelWidth * 480.0 / 512) : hero.PixelWidth, srcH = hero.PixelHeight;
            double k = 1.3 / psp;
            double shownW = srcW * k, shownH = srcH * k;
            var place = new Rect(region.Right - shownW, region.Y - 8, shownW, shownH);
            // How many of the monitor's pixels one pixel of the picture covers. From about one and a half the
            // enlargement shows, so the picture is put out of focus on purpose and fades sooner.
            double enlarged = k * scale * Tx.PixelsPerDip;
            bool soft = enlarged > 1.45;
            dc.PushClip(cardShape);
            dc.PushClip(new RectangleGeometry(new Rect(region.X - 20, region.Y - 10, region.Width + 20, region.Height + 10)));
            // Eased at both ends, so the eye finds no line where the picture begins.
            var mask = new LinearGradientBrush { MappingMode = BrushMappingMode.Absolute, StartPoint = new Point(region.X - 20, 0), EndPoint = new Point(region.X + (soft ? 150 : 110), 0) };
            mask.GradientStops.Add(new GradientStop(Color.FromArgb(0, 0, 0, 0), 0));
            mask.GradientStops.Add(new GradientStop(Color.FromArgb(18, 0, 0, 0), 0.25));
            mask.GradientStops.Add(new GradientStop(Color.FromArgb(128, 0, 0, 0), 0.6));
            mask.GradientStops.Add(new GradientStop(Color.FromArgb(232, 0, 0, 0), 0.87));
            mask.GradientStops.Add(new GradientStop(Color.FromArgb(255, 0, 0, 0), 1));
            mask.Freeze();
            dc.PushOpacityMask(mask);
            var foot = new LinearGradientBrush { MappingMode = BrushMappingMode.Absolute, StartPoint = new Point(0, place.Bottom - (soft ? 190 : 140)), EndPoint = new Point(0, place.Bottom - 6) };
            foot.GradientStops.Add(new GradientStop(Color.FromArgb(255, 0, 0, 0), 0));
            foot.GradientStops.Add(new GradientStop(Color.FromArgb(128, 0, 0, 0), 0.5));
            foot.GradientStops.Add(new GradientStop(Color.FromArgb(22, 0, 0, 0), 0.85));
            foot.GradientStops.Add(new GradientStop(Color.FromArgb(0, 0, 0, 0), 1));
            foot.Freeze();
            dc.PushOpacityMask(foot);
            if (heroPart == null)
            {
                heroPart = new CroppedBitmap(hero, new Int32Rect(0, 0, (int)srcW, (int)srcH));
                heroPart.Freeze();
            }
            BitmapSource picture = heroPart;
            if (soft)
            {
                // Enough to take the pixel grid and the PSP's dither out, not so much that the face is lost:
                // about two of the monitor's pixels at twice the size.
                double sigma = Math.Round(Math.Max(0.5, Math.Min(1.4, 0.28 * enlarged)) * 10) / 10;
                if (heroSoft == null || heroSoftSigma != sigma) { heroSoft = ArtFolder.Soft(heroPart, sigma); heroSoftSigma = sigma; }
                picture = heroSoft;
            }
            dc.DrawImage(picture, place);
            dc.DrawRectangle(Pal.B(Pal.Ink, soft ? 0.30 : 0.25), null, place);
            dc.Pop();
            dc.Pop();
            dc.Pop();
            dc.Pop();
        }
        BitmapSource heroSoft;
        double heroSoftSigma;

        // ---- Display ---------------------------------------------------------------------------

        static bool IsAutoScale(Model m)
        {
            int n;
            return !(int.TryParse(m.Get(S.InternalScale).Trim(), out n) && n >= 1 && n <= 8);
        }
        /// <summary>
        /// The size [Rendering] Preset keeps the automatic resolution under (low: scale 2, medium: scale 4;
        /// RENDERING.md), or null when it sets no limit.
        /// </summary>
        static string AutoLimit(Model m)
        {
            if (!m.Game.Knows(S.Preset.Probe)) return null;
            string p = m.Get(S.Preset).Trim().ToLowerInvariant();
            return p == "low" ? "960 x 544" : p == "medium" ? "1920 x 1088" : null;
        }

        List<Row> BuildDisplay()
        {
            var m = Model;
            var list = new List<Row>();
            list.Add(Seg("Screen", S.Fullscreen, O("Window", "false"), O("Full screen", "true")));
            // The game has no window-size key yet: the row is there only for a game build that has one.
            if (m.Game.Probed && m.Game.Knows(S.WindowSize.Probe))
            {
                var sizes = new[] { "960x544", "1440x816", "1920x1088", "2880x1632" };
                list.Add(new Row
                {
                    Id = S.WindowSize.Id, Label = "Window size", Settings = new[] { S.WindowSize },
                    Control = new Dropdown(() => m.Get(S.WindowSize), v => m.Set(S.WindowSize, v), sizes.Select(s => O(s.Replace("x", " x "), s))) { Placeholder = "Automatic" },
                    EnabledFn = () => !m.GetBool(S.Fullscreen), Reason = () => "Only used in a window."
                });
            }
            var res = Seg("Rendering resolution", S.InternalScale, O("Auto", "auto"), O("1x", "1"), O("2x", "2"), O("3x", "3"), O("4x", "4"), O("6x", "6"), O("8x", "8"));
            // The size the chosen multiple gives, as a slider shows its number; for Auto, the limit when one is in force.
            var choices = (Segmented)res.Control;
            choices.Pad = 20;
            choices.Readout = () =>
            {
                int n;
                if (int.TryParse(m.Get(S.InternalScale).Trim(), out n) && n >= 1 && n <= 8) return (480 * n) + " x " + (272 * n);
                string limit = AutoLimit(m);
                return limit != null ? "\u2264 " + limit : null;
            };
            list.Add(res);
            list.Add(Seg("Frame rate", S.FrameLock, O("30 fps", "2"), O("60 fps", "1")));
            list.Add(Tog("Show frames early", S.EarlyPresent));
            // [Rendering] Preset: for now all the game does with it is to keep Auto under a size. It is a
            // limit for weak graphics cards, not a look, so it is its own row here and Quality (Graphics)
            // does not write it.
            Func<string> preset = () => { string p = m.Get(S.Preset).Trim().ToLowerInvariant(); return p == "low" || p == "medium" ? p : "high"; };
            AddAdvanced(list, "Display", new Row
            {
                Id = S.Preset.Id, Label = "Auto resolution limit", Settings = new[] { S.Preset },
                EnabledFn = () => IsAutoScale(m), Reason = () => "Only used with Auto resolution.",
                Control = new Segmented(preset, v => { if (v != preset()) m.Set(S.Preset, v); },
                    O("No limit", "high"), O("1920 x 1088", "medium"), O("960 x 544", "low"))
            });
            return list;
        }

        // ---- Graphics --------------------------------------------------------------------------

        // Quality is the launcher's bundle of five looks (anti-aliasing, sharp textures, texture filtering,
        // ambient shadows, glow). It does not write [Rendering] Preset: the game uses that key to limit the
        // automatic resolution, which is a row of its own on the Display page.
        sealed class Quality { public string Name, Aa, AaOld, Aniso, Filter, Ssao, Glow; }
        static readonly Quality[] qualities =
        {
            new Quality { Name = "original", Aa = "off", AaOld = "off", Aniso = "1", Filter = "game", Ssao = "off", Glow = "game" },
            new Quality { Name = "medium", Aa = "fxaa", AaOld = "msaa2", Aniso = "4", Filter = "game", Ssao = "off", Glow = "smooth" },
            new Quality { Name = "high", Aa = "fxaa", AaOld = "msaa4", Aniso = "16", Filter = "game", Ssao = "off", Glow = "smooth" },
            new Quality { Name = "ultra", Aa = "msaa4", AaOld = "msaa8", Aniso = "16", Filter = "game", Ssao = "high", Glow = "smooth" },
        };
        bool KnowsFxaa { get { return Model.Game.Knows(S.AntiAliasing.Probe); } }

        /// <summary>off, fxaa, msaa2, msaa4 or msaa8, from AntiAliasing or the older MSAA key.</summary>
        public string AaNow()
        {
            var m = Model;
            if (KnowsFxaa && m.IsSet(S.AntiAliasing))
            {
                string v = m.Get(S.AntiAliasing).Trim().ToLowerInvariant();
                return v == "none" ? "off" : v;
            }
            if (m.IsSet(S.Msaa) || !KnowsFxaa)
            {
                string n = m.Get(S.Msaa).Trim();
                return n == "2" || n == "4" || n == "8" ? "msaa" + n : (KnowsFxaa && !m.IsSet(S.Msaa) ? "fxaa" : "off");
            }
            return m.DefaultOf(S.AntiAliasing);
        }
        void SetAa(string v)
        {
            var m = Model;
            string n = v.StartsWith("msaa") ? v.Substring(4) : "1";
            if (KnowsFxaa)
            {
                m.Set(S.AntiAliasing, v);
                // An older MSAA line in the file must not contradict it.
                if (m.Doc.Has("Rendering", "MSAA")) m.Set(S.Msaa, n);
            }
            else m.Set(S.Msaa, n);
        }
        string QualityNow()
        {
            var m = Model;
            string aa = AaNow();
            foreach (var q in qualities)
            {
                if (aa != (KnowsFxaa ? q.Aa : q.AaOld)) continue;
                if (m.Game.Knows(S.Anisotropy.Probe) && !Model.Equivalent(m.Get(S.Anisotropy), q.Aniso)) continue;
                if (m.Game.Knows(S.TextureFilter.Probe) && !Model.Equivalent(m.Get(S.TextureFilter), q.Filter)) continue;
                if (m.Game.Knows(S.Ssao.Probe) && !Model.Equivalent(m.Get(S.Ssao), q.Ssao)) continue;
                if (m.Game.Knows(S.Glow.Probe) && !Model.Equivalent(m.Get(S.Glow), q.Glow)) continue;
                // High and Ultra differ only where the game knows the newer keys; without them the preset name decides.
                return q.Name;
            }
            return "custom";
        }
        void ApplyQuality(string name)
        {
            var m = Model;
            var q = qualities.FirstOrDefault(x => x.Name == name);
            if (q == null) return;      // Custom: the rows below stay as they are
            SetAa(KnowsFxaa ? q.Aa : q.AaOld);
            if (m.Game.Knows(S.Anisotropy.Probe)) m.Set(S.Anisotropy, q.Aniso);
            if (m.Game.Knows(S.TextureFilter.Probe)) m.Set(S.TextureFilter, q.Filter);
            if (m.Game.Knows(S.Ssao.Probe)) m.Set(S.Ssao, q.Ssao);
            if (m.Game.Knows(S.Glow.Probe)) m.Set(S.Glow, q.Glow);
        }

        List<Row> BuildGraphics()
        {
            var m = Model;
            var list = new List<Row>();
            Setting[] parts = { S.AntiAliasing, S.Msaa, S.Anisotropy, S.TextureFilter, S.Ssao, S.Glow };
            list.Add(new Row
            {
                Id = "quality", Label = "Quality",
                ChangedFn = () => parts.Any(p => m.IsChanged(p)),
                Control = new Segmented(QualityNow, ApplyQuality, O("Original", "original"), O("Medium", "medium"), O("High", "high"), O("Ultra", "ultra"),
                    new Option("Custom", "custom") { VisibleFn = () => QualityNow() == "custom" })
            });
            list.Add(new Row
            {
                Id = "aa", Label = "Anti-aliasing",
                ChangedFn = () => m.IsChanged(S.AntiAliasing) || m.IsChanged(S.Msaa),
                Control = new Segmented(AaNow, SetAa, O("Off", "off"), new Option("Light", "fxaa") { VisibleFn = () => KnowsFxaa }, O("2x", "msaa2"), O("4x", "msaa4"), O("8x", "msaa8"))
            });
            var filter = Seg("Texture filtering", S.TextureFilter,
                O("Original", "game"), O("Smooth", "linear"), O("Blocky", "nearest"));
            list.Add(filter);
            var aniso = Seg("Sharp textures at an angle", S.Anisotropy,
                O("Off", "1"), O("2x", "2"), O("4x", "4"), O("8x", "8"), O("16x", "16"));
            list.Add(aniso);
            var ssao = Seg("Ambient shadows", S.Ssao, O("Off", "off"), O("Low", "low"), O("High", "high"));
            list.Add(ssao);
            list.Add(Seg("Glow", S.Glow, O("Smooth", "smooth"), O("Original", "game"), O("Off", "off")));
            AddAdvanced(list, "Graphics",
                Seg("Renderer", S.Backend, O("Graphics card", "d3d12"), O("Software", "software")),
                Tog("Draw on a second processor core", S.GeThread));
            return list;
        }

        // ---- Controls --------------------------------------------------------------------------

        void ResetDevice()
        {
            var m = Model;
            if (padView)
            {
                m.ResetPadBindings();
                m.ResetTo(S.DeadzoneLeft, S.DeadzoneRight, S.TriggerThreshold, S.TurnSensitivity, S.LookSensitivity, S.PadInvertY, S.CompensateDeadzone, S.PadIndex, S.PadEnabled);
                m.ResetTo(Known(S.Camera, S.Rumble, S.PlayStationPads, S.Prompts, S.PadGlyphs));
            }
            else
            {
                m.ResetKeyboardMouse();
                m.ResetTo(Known(S.SensitivityX, S.SensitivityY, S.MouseInvertX, S.MouseInvertY, S.HoldPitch, S.Exact, S.Smoothing, S.MouseCapture, S.MenuClicks, S.MouseEnabled, S.WalkScale, S.LookKeySpeed, S.Camera, S.Prompts, S.PadGlyphs));
            }
        }

        List<Row> BuildControls()
        {
            var m = Model;
            var list = new List<Row>();
            pages[3].DefaultsQuestion = padView ? "Reset gamepad bindings to the defaults?" : "Reset keyboard & mouse bindings to the defaults?";
            // Which device the rows below are for: two tabs at the left, not a setting.
            list.Add(new TabsRow("device", new Segmented(() => padView ? "pad" : "keys", v => { padView = v == "pad"; scrollTarget = 0; Rebuild(); },
                O("Keyboard & mouse", "keys"), O("Gamepad", "pad")) { SegHeight = 36 }));
            // The control style: its bindings with it, where they are still the old style's (Model.SwitchLayout).
            var style = new Row
            {
                Id = S.Layout.Id, Label = "Control style", Settings = new[] { S.Layout },
                ChangedFn = () => m.IsChanged(S.Layout) || m.IsChanged(S.Scheme),
                Control = new Segmented(() => m.Layout, v =>
                {
                    if (v == m.Layout) return;
                    m.SwitchLayout(v);
                    Rebuild();
                }, O("Total Overdose PC", "tod_pc"), O("Modern", "modern"), O("PSP original", "psp"))
            };
            list.Add(style);
            // With a controller connected the live sticks further down show it; without one, that is a problem to act on.
            if (padView && !Pad.Connected) list.Add(new CustomRow("padstatus", 40, DrawPadStatus));

            // Aiming first: how fast the view turns is what a player comes here for most often, and it must
            // not sit under two dozen binding rows.
            list.Add(new GroupRow("Aiming"));
            if (!padView)
            {
                // A logarithmic track bent so that the default, 0.08, sits a third along.
                var turn = Sld("Turn speed", S.SensitivityX, 0.01, 1.0, 0.01, null, true);
                ((Slider)turn.Control).Curve = 1.38;
                list.Add(turn);
                var look = Sld("Look speed", S.SensitivityY, 0.01, 1.0, 0.01, null, true);
                ((Slider)look.Control).Curve = 1.38;
                look.EnabledFn = () => m.Scheme != "psp";
                look.Reason = () => "PSP original cannot look up or down.";
                list.Add(look);
                list.Add(Tog("Invert left/right", S.MouseInvertX));
                list.Add(Tog("Invert up/down", S.MouseInvertY));
                list.Add(Tog("Keep the aim where you put it", S.HoldPitch));
            }
            else
            {
                list.Add(Sld("Turn speed", S.TurnSensitivity, 0, 4.0, 0.05, null));
                var pl = Sld("Look speed", S.LookSensitivity, 0, 4.0, 0.05, null);
                pl.EnabledFn = () => m.Scheme != "psp";
                pl.Reason = () => "PSP original cannot look up or down.";
                list.Add(pl);
                list.Add(Tog("Invert up/down", S.PadInvertY));
            }
            // How closely the view follows the aim: with either device, so on both tabs.
            list.Add(Seg("Camera", S.Camera, O("Responsive", "tight"), O("Original", "original")));

            list.Add(new GroupRow("Actions"));
            foreach (var a in Actions.All)
            {
                if (padView && a.Toggle) continue;
                var action = a;
                list.Add(new Row
                {
                    Id = "bind:" + a.Target, Label = a.Name(m.Scheme), Compact = true,
                    Control = new Bindings(a, padView),
                    Settings = new Setting[0],
                    ChangedFn = () => padView ? S.PadButtons.Any(b => m.IsChanged(S.Pad(b)) && (string.Equals(m.PadTarget(b), action.Target, StringComparison.OrdinalIgnoreCase)))
                                             : m.IsChanged(S.Keys(action.Target)) || S.MouseInputs.Any(i => m.IsChanged(S.Mouse(i)) && string.Equals(m.MouseTarget(i), action.Target, StringComparison.OrdinalIgnoreCase)),
                    EnabledFn = () => !(action.Look && m.Scheme == "psp"),
                    Reason = () => "Not used with PSP original."
                });
            }

            if (!padView)
            {
                list.Add(new GroupRow("Mouse"));
                list.Add(Tog("Precise aim", S.Exact));
                list.Add(Sld("Smoothing", S.Smoothing, 0, 0.95, 0.05, null));
                list.Add(Seg("Grab the mouse", S.MouseCapture, O("On click", "click"), O("Never", "never")));
                list.Add(Tog("Clicks work in menus", S.MenuClicks));
                list.Add(Tog("Use the mouse", S.MouseEnabled));
                list.Add(new GroupRow("Keyboard"));
                list.Add(Sld("Walk speed", S.WalkScale, 0.31, 1.0, 0.01, Percent));
                var lks = Sld("Look key speed", S.LookKeySpeed, 0.05, 1.0, 0.05, Percent);
                lks.EnabledFn = () => m.Scheme != "psp";
                lks.Reason = () => "Not used with PSP original.";
                list.Add(lks);
            }
            else
            {
                list.Add(new GroupRow("Sticks and triggers"));
                // The live sticks sit right above the dead zone sliders they help to judge.
                list.Add(new CustomRow("sticks", 64, DrawSticks));
                list.Add(Sld("Left stick dead zone", S.DeadzoneLeft, 0, 0.9, 0.01, Percent));
                list.Add(Sld("Right stick dead zone", S.DeadzoneRight, 0, 0.9, 0.01, Percent));
                list.Add(Sld("Trigger pull", S.TriggerThreshold, 0.01, 1.0, 0.01, Percent));
                list.Add(Tog("Start moving at the dead zone's edge", S.CompensateDeadzone));
                list.Add(new GroupRow("Controller"));
                list.Add(Seg("Which controller", S.PadIndex, O("Automatic", "auto"), O("1", "0"), O("2", "1"), O("3", "2"), O("4", "3")));
                if (Has(S.PlayStationPads)) list.Add(Tog("Use PlayStation controllers", S.PlayStationPads));
                if (Has(S.Rumble))
                {
                    // How strong, in percent, and a button that lets the connected controller show it.
                    var rumble = Sld("Rumble", S.Rumble, 0, 1.0, 0.05, Percent);
                    var strength = (Slider)rumble.Control;
                    strength.TrackWidth = 270;
                    strength.Button = new Btn("Test", () => { if (RumbleRequested != null) RumbleRequested(m.GetNum(S.Rumble, 1)); })
                    {
                        EnabledFn = () => Pad.Connected && Pad.Kind == PadKind.Xbox,
                        Reason = () => Pad.Connected ? "The test works with an Xbox controller." : "No controller found."
                    };
                    list.Add(rumble);
                }
                list.Add(Tog("Use the gamepad", S.PadEnabled));
            }
            // What the game draws for its buttons on screen, and which family's pictures a controller gets
            // (the launcher draws its own controller pictures the same way). With either device: on both tabs.
            list.Add(new GroupRow("Prompts"));
            if (Has(S.Prompts)) list.Add(Seg("On screen", S.Prompts, O("Automatic", "auto"), O("Keyboard", "keyboard"), O("Gamepad", "gamepad"), O("PSP original", "psp")));
            if (Has(S.PadGlyphs)) list.Add(Seg("Button pictures", S.PadGlyphs, O("Automatic", "auto"), O("Xbox", "xbox"), O("PlayStation", "playstation")));
            AddAdvanced(list, "Controls",
                Tog("Precise turning for stick and keys", S.ExactTurn),
                Tog("Listen while in the background", S.BackgroundInput),
                Tog("Input test line", S.ShowInput));
            return list;
        }

        /// <summary>No controller: said like the other problems, in red with the triangle. With one, nothing is said.</summary>
        void DrawPadStatus(DrawingContext dc, Surface ui, double y)
        {
            Art.Warning(dc, Lay.LabelX, y + 12, 16);
            Tx.Draw(dc, "No controller found", Fonts.Semi, 16, Pal.B(Pal.Problem), Lay.LabelX + 26, y + 9);
        }

        /// <summary>
        /// The two stick dials: each a circle with the dead zone as a thin ring in it, so the two sliders under
        /// them can be judged by eye. The live dot of each stick is not drawn here: it is in a visual of its
        /// own (Surface.Dials), so that a stick in the hand does not draw the whole picture again.
        /// </summary>
        void DrawSticks(DrawingContext dc, Surface ui, double y)
        {
            var m = Model;
            bool on = Pad.Connected;
            Tx.Draw(dc, "Stick test", Fonts.Semi, 18, Pal.B(on ? Pal.White : Pal.Mix(Pal.White, Pal.LockedGrey, 0.6), on ? 1 : 0.75), Lay.LabelX, y + 15);
            const double r = 26;
            var centres = new Point[2];
            var dead = new double[2];
            for (int i = 0; i < 2; i++)
            {
                centres[i] = new Point(Lay.ControlX + 30 + i * 76, y + 28);
                dead[i] = m.GetNum(i == 0 ? S.DeadzoneLeft : S.DeadzoneRight, 0.24);
                dc.DrawEllipse(Pal.B(Pal.Ink, 0.5), Pal.P(Pal.Cream, 1.5, on ? 0.5 : 0.25), centres[i], r, r);
                // The dead zone: a ring and nothing in it, so it is not taken for a second dot.
                if (dead[i] > 0.005) dc.DrawEllipse(null, Pal.P(Pal.Flame, 1.5, on ? 0.85 : 0.4), centres[i], r * dead[i], r * dead[i]);
            }
            ui.Dials(centres[0], centres[1], r, dead[0], dead[1]);
        }

        // ---- Textures & mods -------------------------------------------------------------------

        void ResetMods()
        {
            var m = Model;
            foreach (var mod in m.Mods) mod.Enabled = true;
            m.Mods.Sort((a, b) => string.Compare(a.Name, b.Name, StringComparison.OrdinalIgnoreCase));
            m.CommitMods();
            m.ResetTo(Known(S.DumpTextures, S.LogFiles));
        }

        /// <summary>The pack whose pictures the game shows: with several switched on, the last one wins.</summary>
        ModInfo ActivePack()
        {
            var on = Model.TexturePacks.Where(p => p.Enabled).ToList();
            return on.Count > 0 ? on[on.Count - 1] : null;
        }
        int PacksOn { get { return Model.TexturePacks.Count(p => p.Enabled); } }
        void SetTextures(string pack)
        {
            var m = Model;
            foreach (var p in m.TexturePacks) p.Enabled = pack != null && p.Name == pack;
            if (pack != null) chosenPack = pack;
            m.CommitMods();
            Rebuild();
        }

        List<Row> BuildMods()
        {
            var m = Model;
            var list = new List<Row>();
            var packs = m.TexturePacks;
            list.Add(new Row
            {
                Id = "textures", Label = "Textures",
                ChangedFn = () => m.IsChanged(S.ModsEnabled),
                Control = new Segmented(() => ActivePack() != null ? "hd" : "standard",
                    v =>
                    {
                        if (v == "standard") { var was = ActivePack(); if (was != null) chosenPack = was.Name; SetTextures(null); }
                        else { var all = m.TexturePacks; if (all.Count > 0) SetTextures((all.FirstOrDefault(p => p.Name == chosenPack) ?? all[0]).Name); }
                    },
                    // The HD choice burns while it is selected.
                    O("Standard", "standard"), new Option("HD pack", "hd", () => m.TexturePacks.Count > 0) { Flames = true, Reason = () => "No HD pack found." })
            });
            if (packs.Count > 1)
            {
                list.Add(new Row
                {
                    Id = "pack", Label = "HD pack",
                    EnabledFn = () => ActivePack() != null, Reason = () => "Textures are set to Standard.",
                    // Several packs switched on (every mod is on until the player says otherwise): the game
                    // gives the later one the last word, so that one is named, and choosing one leaves it alone on.
                    // While Textures is Standard the field names the pack the HD choice would switch on.
                    Control = new Dropdown(() =>
                    {
                        var a = ActivePack();
                        if (a != null) return a.Name;
                        var all = m.TexturePacks;
                        return all.Count == 0 ? "" : (all.FirstOrDefault(x => x.Name == chosenPack) ?? all[0]).Name;
                    }, v => SetTextures(v), packs.Select(p => new Option(p.Name, p.Name) { OnFn = () => p.Enabled }))
                    {
                        Placeholder = "",
                        Text = () => { var a = ActivePack(); return a != null && PacksOn > 1 ? a.Name + " + " + (PacksOn - 1) + " more" : null; }
                    }
                });
            }
            // With no mods at all there is no list and no heading: the page goes straight on to the mods folder.
            if (m.Mods.Count > 0)
            {
                list.Add(new GroupRow("Installed mods"));
                for (int i = 0; i < m.Mods.Count; i++) list.Add(new ModRow(m.Mods[i], i, m.Mods.Count));
                list.Add(new CustomRow("modgap", 10, (dc, ui, y) => { }));
            }
            list.Add(new Row
            {
                Id = "modsfolder", Label = "Mods folder", Settings = new[] { S.ModsFolder },
                Control = new Buttons(new Btn("Open", () => OpenFolder(m.ModsDir, true), true)) { Path = () => m.ModsDir, PathAction = () => OpenFolder(m.ModsDir, true) }
            });
            list.Add(Tog("Dump textures while playing", S.DumpTextures));
            AddAdvanced(list, "Textures & mods",
                Tog("Log the files the game reads", S.LogFiles));
            return list;
        }

        void OpenFolder(string path, bool create = false)
        {
            try { if (create && !Directory.Exists(path)) Directory.CreateDirectory(path); } catch { }
            if (OpenPathRequested != null) OpenPathRequested(path);
        }

        // ---- Sound -----------------------------------------------------------------------------

        List<Row> BuildSound()
        {
            var list = new List<Row>();
            var row = Sld("Sound buffer", S.BufferMs, 20, 500, 10, v => Math.Round(v) + " ms", false, "0");
            var s = (Slider)row.Control;
            s.Log = true;
            // Three marks on the track, with their numbers and no words.
            s.Marks.Add(new Mark(40, "40"));
            s.Marks.Add(new Mark(70, "70"));
            s.Marks.Add(new Mark(150, "150"));
            list.Add(row);
            // The game has no volume keys: how loud it is, is set in the Windows volume mixer.
            list.Add(new Row
            {
                Id = "volume", Label = "Volume",
                Control = new Buttons(new Btn("Open Windows volume mixer", OpenVolumeMixer) { Prompt = "Open" })
            });
            return list;
        }

        void OpenVolumeMixer()
        {
            string mixer = Path.Combine(Environment.SystemDirectory, "SndVol.exe");
            if (OpenPathRequested != null) OpenPathRequested(File.Exists(mixer) ? mixer : "ms-settings:apps-volume");
        }

        // ---- Game data -------------------------------------------------------------------------

        List<Row> BuildGameData()
        {
            var m = Model;
            var list = new List<Row>();
            var disc = new Buttons(new Btn("Choose .iso...", () => { if (ChooseDiscRequested != null) ChooseDiscRequested(true); }),
                                   new Btn("Choose folder...", () => { if (ChooseDiscRequested != null) ChooseDiscRequested(false); }))
            {
                Path = () => string.IsNullOrEmpty(m.Disc.Path) ? m.Get(S.DiscPath) : m.Disc.Path, PathOnTop = true,
                PathAction = () => { if (m.Disc.Ok) OpenFolder(m.Disc.IsImage ? Path.GetDirectoryName(m.Disc.Path) : m.Disc.Path); }
            };
            list.Add(new Row
            {
                Id = S.DiscPath.Id, Label = "Disc", Control = disc,
                ChangedFn = () => m.IsChanged(S.DiscPath),
                Problem = () => m.Disc.Ok || string.IsNullOrEmpty(m.Disc.Message) ? null : m.Disc.Message
            });
            list.Add(new Row
            {
                Id = "saves", Label = "Saves",
                Control = new Buttons(new Btn("Open", () => OpenFolder(m.SavesDir, true), true)) { Path = () => m.SavesDir, PathAction = () => OpenFolder(m.SavesDir, true) }
            });
            list.Add(new Row
            {
                Id = "inifile", Label = "Settings file",
                Control = new Buttons(new Btn("Open", () => { Save(); if (OpenPathRequested != null) OpenPathRequested(m.IniPath); }, true)) { Path = () => m.IniPath, PathAction = () => OpenFolder(m.IniDir) }
            });
            list.Add(new Row
            {
                Id = "logs", Label = "Logs",
                Control = new Buttons(new Btn("Open", () => OpenFolder(LogFolder()), true)) { Path = () => LogFolder(), PathAction = () => OpenFolder(LogFolder()) }
            });
            list.Add(new Row
            {
                Id = "program", Label = "Game program",
                Problem = () => m.Game.Exists ? null : GameMissing,
                Control = new Buttons(new Btn("Open", () => OpenFolder(m.GameDir), true)) { Path = () => m.Game.ExePath, PathAction = () => OpenFolder(m.GameDir) }
            });
            AddAdvanced(list, "Game data",
                Seg("Game thread priority", S.Priority, O("Normal", "normal"), O("High", "high")));
            return list;
        }

        /// <summary>
        /// The window's title: the game's name first, then the versions. They stand in the title bar and
        /// nowhere on the pages (a line of them under the last row read as that row's explanation).
        /// </summary>
        public string WindowTitle()
        {
            var m = Model;
            string text = "Chili Con Carnage" + Dot + "Launcher " + LauncherVersion;
            if (!m.Game.Exists) return text;
            string game = m.Game.Version;
            if (string.IsNullOrEmpty(game))
            {
                game = Path.GetFileName(m.Game.ExePath);
                try { game += ", " + File.GetLastWriteTime(m.Game.ExePath).ToString("d MMM yyyy", System.Globalization.CultureInfo.InvariantCulture); } catch { }
            }
            return text + Dot + "Game " + game;
        }

        string LogFolder()
        {
            var m = Model;
            string log = m.LatestLog();
            if (log != null) return Path.GetDirectoryName(log);
            return m.LogsDir;
        }
    }
}
