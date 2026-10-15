// The look: palette, the three letterings, drawn icons and the scorched-paper backdrop (DESIGN.md
// section 3). Everything here is drawn by the launcher; nothing comes from the game.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace ChiliLauncher
{
    public static class Pal
    {
        public static Color Hex(string h)
        {
            return Color.FromRgb(Convert.ToByte(h.Substring(1, 2), 16), Convert.ToByte(h.Substring(3, 2), 16), Convert.ToByte(h.Substring(5, 2), 16));
        }
        public static readonly Color Ink = Hex("#0A0503"), Soot = Hex("#140804"), RustDark = Hex("#3C0C05"), Rust = Hex("#7A1F08"),
            Ember = Hex("#B8440C"), Flame = Hex("#F0A020"), HotOrange = Hex("#F85808"), Chili = Hex("#E81010"), Gold = Hex("#F8D820"),
            GoldDark = Hex("#D89A10"), PaleYellow = Hex("#F8C868"), Cream = Hex("#F6E8C8"), White = Colors.White, HealthRed = Hex("#F82808"),
            ChalkGreen = Hex("#9BE86A"), LockedGrey = Hex("#8A7A6A"), Stem = Hex("#3D8B2F"), ComboYellow = Hex("#F8F000"), LogoPale = Hex("#FFF0A0"),
            Problem = Hex("#FF6A5A");      // the red of a problem's text on the dark card (Chili itself is too dark to read there)

        static readonly Dictionary<uint, SolidColorBrush> brushes = new Dictionary<uint, SolidColorBrush>();
        static readonly Dictionary<ulong, Pen> pens = new Dictionary<ulong, Pen>();

        public static Color A(Color c, double alpha) { return Color.FromArgb((byte)Math.Round(Math.Max(0, Math.Min(1, alpha)) * 255), c.R, c.G, c.B); }

        public static SolidColorBrush B(Color c, double alpha = 1.0)
        {
            var k = A(c, alpha * c.A / 255.0);
            uint key = ((uint)k.A << 24) | ((uint)k.R << 16) | ((uint)k.G << 8) | k.B;
            SolidColorBrush b;
            if (!brushes.TryGetValue(key, out b))
            {
                b = new SolidColorBrush(k);
                b.Freeze();
                brushes[key] = b;
            }
            return b;
        }
        public static Pen P(Color c, double thickness, double alpha = 1.0)
        {
            var k = A(c, alpha);
            ulong key = ((ulong)(uint)(((uint)k.A << 24) | ((uint)k.R << 16) | ((uint)k.G << 8) | k.B) << 32) | (uint)Math.Round(thickness * 64);
            Pen p;
            if (!pens.TryGetValue(key, out p))
            {
                p = new Pen(B(k), thickness) { LineJoin = PenLineJoin.Round, StartLineCap = PenLineCap.Round, EndLineCap = PenLineCap.Round };
                p.Freeze();
                pens[key] = p;
            }
            return p;
        }
        public static Color Mix(Color a, Color b, double t)
        {
            t = Math.Max(0, Math.Min(1, t));
            return Color.FromArgb((byte)(a.A + (b.A - a.A) * t), (byte)(a.R + (b.R - a.R) * t), (byte)(a.G + (b.G - a.G) * t), (byte)(a.B + (b.B - a.B) * t));
        }
        public static Color Lighten(Color c, double amount) { return Mix(c, Colors.White, amount); }
        /// <summary>Disabled things: desaturated towards the locked grey.</summary>
        public static Color Locked(Color c) { return Mix(c, LockedGrey, 0.75); }

        static LinearGradientBrush logo;
        /// <summary>The logo lettering's fill: pale at the top, gold, darker gold at the base.</summary>
        public static Brush LogoFill
        {
            get
            {
                if (logo == null)
                {
                    logo = new LinearGradientBrush { StartPoint = new Point(0, 0), EndPoint = new Point(0, 1) };
                    logo.GradientStops.Add(new GradientStop(LogoPale, 0));
                    logo.GradientStops.Add(new GradientStop(Gold, 0.45));
                    logo.GradientStops.Add(new GradientStop(GoldDark, 1));
                    logo.Freeze();
                }
                return logo;
            }
        }
        /// <summary>The combo meter: yellow to flame to health red, mapped over the given width.</summary>
        public static Brush ComboFill(double x, double width)
        {
            var b = new LinearGradientBrush { MappingMode = BrushMappingMode.Absolute, StartPoint = new Point(x, 0), EndPoint = new Point(x + width, 0) };
            b.GradientStops.Add(new GradientStop(ComboYellow, 0));
            b.GradientStops.Add(new GradientStop(Flame, 0.5));
            b.GradientStops.Add(new GradientStop(HealthRed, 1));
            b.Freeze();
            return b;
        }
    }

    public static class Fonts
    {
        public static readonly Typeface Display = Pick("ariblk", new[] { "Arial Black", "Arial", "Impact", "Segoe UI Black", "Segoe UI" }, FontWeights.Black, FontStyles.Normal);
        public static readonly Typeface Heading = Pick("seguibli", new[] { "Segoe UI Black", "Segoe UI", "Verdana", "Arial" }, FontWeights.Black, FontStyles.Italic);
        public static readonly Typeface Ui = Pick("segoeui", new[] { "Segoe UI", "Tahoma" }, FontWeights.Normal, FontStyles.Normal);
        public static readonly Typeface Semi = Pick("seguisb", new[] { "Segoe UI Semibold", "Segoe UI", "Tahoma" }, FontWeights.SemiBold, FontStyles.Normal);
        public static readonly Typeface Mono = Pick("consola", new[] { "Consolas", "Lucida Console" }, FontWeights.Normal, FontStyles.Normal);

        static Typeface Pick(string file, string[] families, FontWeight weight, FontStyle style)
        {
            Typeface first = null;
            foreach (var family in families)
            {
                try
                {
                    var face = new Typeface(new FontFamily(family), style, weight, FontStretches.Normal);
                    GlyphTypeface glyphs;
                    if (!face.TryGetGlyphTypeface(out glyphs)) continue;
                    if (first == null) first = face;
                    string name = Path.GetFileNameWithoutExtension(glyphs.FontUri.LocalPath).ToLowerInvariant();
                    if (name == file) return face;
                }
                catch { }
            }
            return first ?? new Typeface("Segoe UI");
        }
    }

    /// <summary>Text: plain for the UI face, outlined with a hard shadow for the two game letterings.</summary>
    public static class Tx
    {
        static double ppd = 1.0;
        static readonly Dictionary<string, FormattedText> texts = new Dictionary<string, FormattedText>();
        static readonly Dictionary<string, Geometry> shapes = new Dictionary<string, Geometry>();

        public static double PixelsPerDip
        {
            get { return ppd; }
            set { if (Math.Abs(ppd - value) > 0.001) { ppd = value; texts.Clear(); shapes.Clear(); } }
        }

        /// <summary>Screenshots and tests: while this is a list, every text that is drawn is noted in it.</summary>
        public static List<string> Log;
        static bool measuring;

        public static FormattedText Make(string s, Typeface face, double size, Brush brush, double maxWidth = 0, int maxLines = 0)
        {
            if (Log != null && !measuring && !string.IsNullOrEmpty(s)) Log.Add(s);
            string key = s + "\u0001" + face.GetHashCode() + "\u0001" + size + "\u0001" + brush.GetHashCode() + "\u0001" + maxWidth + "\u0001" + maxLines;
            FormattedText t;
            if (texts.TryGetValue(key, out t)) return t;
            if (texts.Count > 3000) texts.Clear();
            t = new FormattedText(s ?? "", CultureInfo.InvariantCulture, FlowDirection.LeftToRight, face, size, brush, ppd);
            if (maxWidth > 0)
            {
                t.MaxTextWidth = maxWidth;
                t.Trimming = TextTrimming.CharacterEllipsis;
                if (maxLines > 0) t.MaxLineCount = maxLines;
            }
            texts[key] = t;
            return t;
        }
        public static double Width(string s, Typeface face, double size)
        {
            measuring = true;
            try { return Make(s, face, size, Brushes.White).WidthIncludingTrailingWhitespace; }
            finally { measuring = false; }
        }
        public static double Draw(DrawingContext dc, string s, Typeface face, double size, Brush brush, double x, double y, double maxWidth = 0, int maxLines = 0)
        {
            var t = Make(s, face, size, brush, maxWidth, maxLines);
            dc.DrawText(t, new Point(x, y));
            return t.WidthIncludingTrailingWhitespace;
        }
        public static void DrawCentered(DrawingContext dc, string s, Typeface face, double size, Brush brush, Rect box, double dy = 0)
        {
            var t = Make(s, face, size, brush);
            dc.DrawText(t, new Point(box.X + (box.Width - t.Width) / 2, box.Y + (box.Height - t.Height) / 2 + dy));
        }
        public static void DrawRight(DrawingContext dc, string s, Typeface face, double size, Brush brush, double right, double y)
        {
            var t = Make(s, face, size, brush);
            dc.DrawText(t, new Point(right - t.WidthIncludingTrailingWhitespace, y));
        }

        public static Geometry Shape(string s, Typeface face, double size)
        {
            if (Log != null && !string.IsNullOrEmpty(s)) Log.Add(s);
            string key = s + "\u0001" + face.GetHashCode() + "\u0001" + size;
            Geometry g;
            if (shapes.TryGetValue(key, out g)) return g;
            if (shapes.Count > 600) shapes.Clear();
            var t = new FormattedText(s ?? "", CultureInfo.InvariantCulture, FlowDirection.LeftToRight, face, size, Brushes.White, ppd);
            g = t.BuildGeometry(new Point(0, 0));
            g.Freeze();
            shapes[key] = g;
            return g;
        }

        /// <summary>Outlined lettering with a hard shadow. x,y is the top-left of the text's line box.</summary>
        public static void Outlined(DrawingContext dc, string s, Typeface face, double size, double x, double y, Brush fill,
            double stroke, double shadowX, double shadowY, double shadowAlpha, double alpha = 1.0)
        {
            var g = Shape(s, face, size);
            if (alpha < 1.0) dc.PushOpacity(alpha);
            if (shadowAlpha > 0)
            {
                dc.PushTransform(new TranslateTransform(x + shadowX, y + shadowY));
                dc.DrawGeometry(Pal.B(Pal.Ink, shadowAlpha), Pal.P(Pal.Ink, stroke * 2, shadowAlpha), g);
                dc.Pop();
            }
            dc.PushTransform(new TranslateTransform(x, y));
            dc.DrawGeometry(null, Pal.P(Pal.Ink, stroke * 2), g);
            dc.DrawGeometry(fill, null, g);
            dc.Pop();
            if (alpha < 1.0) dc.Pop();
        }

        /// <summary>The menu lettering (Segoe UI Black Italic, 3 px ink stroke).</summary>
        public static void Heading(DrawingContext dc, string s, double size, double x, double y, Color fill, double alpha = 1.0)
        {
            double k = size / 30.0;
            Outlined(dc, s, Fonts.Heading, size, x, y, Pal.B(fill), Math.Max(2, 3 * k), 2 * k, 3 * k, 0.5, alpha);
        }
        /// <summary>The logo lettering (Arial Black, gradient fill, 4 px ink stroke, hard shadow).</summary>
        public static void Logo(DrawingContext dc, string s, double size, double x, double y, Brush fill = null)
        {
            double k = size / 40.0;
            Outlined(dc, s, Fonts.Display, size, x, y, fill ?? Pal.LogoFill, 4 * k, 4 * k, 5 * k, 0.6);
        }
        public static double HeadingWidth(string s, double size) { return Width(s, Fonts.Heading, size); }

        /// <summary>
        /// A path shortened in the middle so that it fits. Whole folders go first (the drive and the last
        /// folders stay: C:\Games\…\mods\hd_textures); letters are cut only when one name alone is too long.
        /// </summary>
        public static string MiddleEllipsis(string s, Typeface face, double size, double maxWidth)
        {
            if (string.IsNullOrEmpty(s) || Width(s, face, size) <= maxWidth) return s ?? "";
            char sep = s.IndexOf('\\') >= 0 ? '\\' : '/';
            var parts = s.Split(sep);
            int n = parts.Length;
            // \\server\share counts as the drive.
            int minHead = s.StartsWith("\\\\") ? Math.Min(4, n - 1) : 1;
            if (n > minHead + 2)
            {
                for (int removed = 1; removed <= n - minHead - 1; removed++)
                    for (int head = Math.Min(minHead + 1, n - removed - 1); head >= minHead; head--)
                    {
                        int tail = n - removed - head;
                        if (tail < 1) continue;
                        string t = string.Join(sep.ToString(), parts, 0, head) + sep + "…" + sep + string.Join(sep.ToString(), parts, n - tail, tail);
                        if (Width(t, face, size) <= maxWidth) return t;
                    }
            }
            int keep = s.Length - 1;
            while (keep > 8)
            {
                int left = keep / 2, right = keep - left;
                string t = s.Substring(0, left) + "…" + s.Substring(s.Length - right);
                if (Width(t, face, size) <= maxWidth) return t;
                keep--;
            }
            return s.Substring(0, 4) + "…";
        }
    }

    /// <summary>Vector icons (DESIGN.md 3.7) and the launcher's own ornaments.</summary>
    public static class Art
    {
        public static void Capsule(DrawingContext dc, Rect r, Brush fill, Pen pen)
        {
            double rad = r.Height / 2;
            dc.DrawRoundedRectangle(fill, pen, r, rad, rad);
        }
        public static void Round(DrawingContext dc, Rect r, double radius, Brush fill, Pen pen) { dc.DrawRoundedRectangle(fill, pen, r, radius, radius); }
        /// <summary>The corner of a keyboard key's cap: square against the round shapes of a controller's buttons.</summary>
        public const double KeyCorner = 5;

        /// <summary>
        /// Fire on a capsule (the selected "HD pack" choice): a glow over the capsule, one inked red outline
        /// of uneven tongues with a lick tapering away round each of the capsule's ends, an orange layer and
        /// a gold core inside the larger tongues, and three small sparks. Drawn before the capsule, which
        /// covers the fire's feet. `time` is in seconds; the tips reach up to 35 px above the capsule.
        /// </summary>
        public static void Flames(DrawingContext dc, Rect r, double time)
        {
            double breathe = 0.5 + 0.5 * Math.Sin(time * 2.3);
            // The glow belongs to the flames: above the capsule and no wider than it.
            // Everything made for one frame is frozen: nothing listens for changes that will not come.
            var over = new RectangleGeometry(new Rect(r.X - 4, r.Y - 44, r.Width + 8, 44 + r.Height * 0.5));
            over.Freeze();
            dc.PushClip(over);
            var glow = new RadialGradientBrush(Pal.A(Pal.HotOrange, 0.30 + 0.10 * breathe), Pal.A(Pal.HotOrange, 0));
            glow.Freeze();
            dc.DrawEllipse(glow, null, new Point(r.X + r.Width / 2, r.Y - 5), r.Width * 0.5, 30);
            dc.Pop();

            var t = Tongues(r, time);
            dc.DrawGeometry(Pal.B(Pal.Chili), Pal.P(Pal.Ink, 2), FireOutline(r, t, time));
            dc.DrawGeometry(Pal.B(Pal.HotOrange), null, FireInside(r, t, 0.70, 0.74, 0.0));
            dc.DrawGeometry(Pal.B(Pal.Gold), null, FireInside(r, t, 0.40, 0.46, 0.62));

            for (int j = 0; j < 3; j++)
            {
                double life = Frac(time * 0.55 + j / 3.0 + 0.12);
                double x = r.X + r.Width * (0.24 + 0.26 * j) + Math.Sin(time * 2.6 + j * 2.1) * 2.5 + life * 3;
                double y = r.Y - 14 - life * 22, size = 3.3 - 1.5 * life;
                double alpha = Math.Min(1, life * 6) * (1 - life);
                dc.DrawGeometry(Pal.B(Pal.Gold, alpha), null, Poly(true, x, y - size, x + size * 0.8, y, x, y + size, x - size * 0.8, y));
            }
        }

        struct Tongue { public double Left, Right, TipX, Height, Share; }

        // How wide the tongues are against an even step: uneven, with one or two that lead.
        static readonly double[] tongueWidths = { 0.78, 1.60, 0.62, 1.30, 0.70, 1.48, 0.60, 1.12 };

        /// <summary>
        /// The tongues over the capsule's straight top at this moment. A wide tongue is a tall one; each
        /// flickers on two sines of its own, and the lean travels along the fire like a draught.
        /// </summary>
        static Tongue[] Tongues(Rect r, double time)
        {
            int n = Math.Max(3, (int)Math.Round((r.Width - 14) / 19));
            double sum = 0, widest = 0;
            for (int k = 0; k < n; k++) { sum += tongueWidths[k % tongueWidths.Length]; widest = Math.Max(widest, tongueWidths[k % tongueWidths.Length]); }
            var list = new Tongue[n];
            double x = r.X + 7, span = r.Width - 14;
            for (int k = 0; k < n; k++)
            {
                double part = tongueWidths[k % tongueWidths.Length], w = span * part / sum;
                double share = (part - 0.6) / (widest - 0.6);                       // 0 for the thinnest, 1 for the leader
                double flick = 0.5 + 0.30 * Math.Sin(time * (5.1 + 1.3 * (k % 4)) + k * 2.4)
                                   + 0.20 * Math.Sin(time * (9.7 + 1.1 * (k % 3)) + k * 1.1);
                double lean = (0.06 + 0.24 * Math.Sin(time * 3.4 + k * 0.85)) * w;
                list[k] = new Tongue
                {
                    Left = x, Right = x + w, Share = share,
                    TipX = x + w / 2 + lean,
                    // A leader keeps most of its height while it flickers and a small tongue stays small, so
                    // that no moment shows them level like a fence (leader 0.80 to 1, the smallest 0.25 to 0.36).
                    Height = 34 * (0.36 + 0.64 * Math.Pow(share, 0.8)) * (1 - (0.34 - 0.14 * share) * (1 - flick)),
                };
                x += w;
            }
            return list;
        }

        /// <summary>
        /// The whole fire as one outline: out of the capsule low on its left end, up round that end in a
        /// small lick, over every tongue with a notch between neighbours, and down round the right end. A
        /// lick stands a few px off the capsule at the shoulder and runs back into its edge at half height,
        /// so each side of the fire ends in a point and not in a stub.
        /// </summary>
        static Geometry FireOutline(Rect r, Tongue[] t, double time)
        {
            double h = r.Height, top = r.Y;
            double sway = Math.Sin(time * 3.4 - 0.9), sway2 = Math.Sin(time * 3.4 + t.Length * 0.85);
            var g = new StreamGeometry { FillRule = FillRule.Nonzero };
            using (var c = g.Open())
            {
                // The left lick: a small flame over the capsule's shoulder whose outer side runs down the
                // round end and into it at half height.
                c.BeginFigure(new Point(r.X + 2, top + h * 0.5), true, true);
                var lick = new Point(r.X + 0.5 + 1.5 * sway, top - 8 - 3 * (0.5 + 0.5 * Math.Sin(time * 7.3)));
                c.BezierTo(new Point(r.X - 3, top + h * 0.30), new Point(r.X - 3 + sway, top - 1), lick, true, true);
                var from = new Point(t[0].Left, top + 1 - 2.5 * (0.5 + 0.5 * Math.Sin(time * 6.1 + 4.0)));
                c.BezierTo(new Point(lick.X + 3, lick.Y + 6), new Point(from.X - 1, from.Y - 4), from, true, true);
                for (int k = 0; k < t.Length; k++)
                {
                    double w = t[k].Right - t[k].Left, lean = t[k].TipX - (t[k].Left + w / 2);
                    var tip = new Point(t[k].TipX, top + 2 - t[k].Height);
                    double notch = 0.5 + 0.5 * Math.Sin(time * 6.1 + k * 1.7);
                    var to = new Point(t[k].Right, top + 1 - 2.5 * notch);
                    // A full belly on the way up, a sharp tip that hooks the way it leans.
                    c.BezierTo(new Point(from.X - 0.04 * w, from.Y - 0.52 * (from.Y - tip.Y)),
                               new Point(tip.X - 0.20 * w - 0.6 * lean, tip.Y + 0.38 * t[k].Height), tip, true, true);
                    c.BezierTo(new Point(tip.X + 0.20 * w - 0.6 * lean, tip.Y + 0.38 * t[k].Height),
                               new Point(to.X + 0.04 * w, to.Y - 0.52 * (to.Y - tip.Y)), to, true, true);
                    from = to;
                }
                // The right lick, the left one mirrored.
                lick = new Point(r.Right - 0.5 + 1.5 * sway2, top - 7 - 3 * (0.5 + 0.5 * Math.Sin(time * 6.7 + 2.0)));
                c.BezierTo(new Point(from.X + 1, from.Y - 4), new Point(lick.X - 3, lick.Y + 6), lick, true, true);
                c.BezierTo(new Point(r.Right + 3 + sway2, top - 1), new Point(r.Right + 3, top + h * 0.30),
                           new Point(r.Right - 2, top + h * 0.5), true, true);
            }
            g.Freeze();
            return g;
        }

        /// <summary>
        /// A layer inside the outline: each tongue again, lower and narrower, standing a little to one side
        /// of its red one. Tongues that lead less than `least` (0 to 1) have none, so only the larger
        /// flames carry a core.
        /// </summary>
        static Geometry FireInside(Rect r, Tongue[] t, double tall, double wide, double least)
        {
            double foot = r.Y + 7;                      // behind the capsule
            var g = new StreamGeometry { FillRule = FillRule.Nonzero };
            using (var c = g.Open())
            {
                for (int k = 0; k < t.Length; k++)
                {
                    if (t[k].Share < least) continue;
                    double w = t[k].Right - t[k].Left, mid = t[k].Left + w / 2, lean = t[k].TipX - mid;
                    double side = (k % 2 == 0 ? 1 : -1) * 0.10 * w * (1 - tall);
                    double half = 0.5 * w * wide, height = t[k].Height * tall + (foot - (r.Y + 2));
                    var tip = new Point(mid + side + lean * (0.5 + 0.5 * tall), foot - height);
                    var from = new Point(mid + side - half, foot);
                    var to = new Point(mid + side + half, foot);
                    c.BeginFigure(from, true, true);
                    c.BezierTo(new Point(from.X - 0.03 * w, from.Y - 0.55 * height),
                               new Point(tip.X - 0.36 * half - 0.5 * lean, tip.Y + 0.38 * height), tip, true, true);
                    c.BezierTo(new Point(tip.X + 0.36 * half - 0.5 * lean, tip.Y + 0.38 * height),
                               new Point(to.X + 0.03 * w, to.Y - 0.55 * height), to, true, true);
                }
            }
            g.Freeze();
            return g;
        }

        /// <summary>The fill of a choice that is on fire: gold at the top down to hot orange, breathing slowly.</summary>
        public static Brush EmberFill(double time)
        {
            double breathe = 0.5 + 0.5 * Math.Sin(time * 2.3);
            var b = new LinearGradientBrush { StartPoint = new Point(0, 0), EndPoint = new Point(0, 1) };
            b.GradientStops.Add(new GradientStop(Pal.Mix(Pal.Gold, Pal.ComboYellow, 0.6 * breathe), 0));
            b.GradientStops.Add(new GradientStop(Pal.Flame, 0.55));
            b.GradientStops.Add(new GradientStop(Pal.Mix(Pal.Flame, Pal.HotOrange, 0.8), 1));
            b.Freeze();
            return b;
        }
        static double Hash(double v) { return Frac(Math.Sin(v) * 43758.5453); }
        static double Frac(double v) { return v - Math.Floor(v); }

        static Geometry Poly(bool closed, params double[] xy)
        {
            var g = new StreamGeometry();
            using (var c = g.Open())
            {
                c.BeginFigure(new Point(xy[0], xy[1]), true, closed);
                for (int i = 2; i < xy.Length; i += 2) c.LineTo(new Point(xy[i], xy[i + 1]), true, true);
            }
            g.Freeze();
            return g;
        }

        /// <summary>The sign that a list goes on above or below: the menu's arrow, turned.</summary>
        public static void More(DrawingContext dc, double cx, double cy, bool down, double alpha, bool hover = false)
        {
            // 14 px wide: it sits in the scrollbar's column, between the rows and the card's edge. Hovered, it lifts a little.
            if (alpha < 1) dc.PushOpacity(alpha);
            if (hover) cy += down ? 1 : -1;
            dc.DrawGeometry(Pal.B(hover ? Pal.LogoPale : Pal.Gold), Pal.P(Pal.Ink, 2), down ? Poly(true, cx - 7, cy - 4, cx + 7, cy - 4, cx, cy + 4) : Poly(true, cx - 7, cy + 4, cx + 7, cy + 4, cx, cy - 4));
            if (alpha < 1) dc.Pop();
        }

        /// <summary>The HUD's menu arrow: a triangle pointing right.</summary>
        public static void Pointer(DrawingContext dc, double x, double y, double size, Color fill)
        {
            dc.DrawGeometry(Pal.B(fill), Pal.P(Pal.Ink, 2), Poly(true, x, y, x + size * 0.9, y + size / 2, x, y + size));
        }
        public static void TriangleDown(DrawingContext dc, double cx, double cy, double size, Color fill, bool outline = true)
        {
            dc.DrawGeometry(Pal.B(fill), outline ? Pal.P(Pal.Ink, 1.5) : null, Poly(true, cx - size / 2, cy - size * 0.35, cx + size / 2, cy - size * 0.35, cx, cy + size * 0.45));
        }
        public static void TriangleUp(DrawingContext dc, double cx, double cy, double size, Color fill, bool outline = true)
        {
            dc.DrawGeometry(Pal.B(fill), outline ? Pal.P(Pal.Ink, 1.5) : null, Poly(true, cx - size / 2, cy + size * 0.35, cx + size / 2, cy + size * 0.35, cx, cy - size * 0.45));
        }
        public static void TriangleRight(DrawingContext dc, double cx, double cy, double size, Color fill)
        {
            dc.DrawGeometry(Pal.B(fill), Pal.P(Pal.Ink, 1.5), Poly(true, cx - size * 0.35, cy - size / 2, cx + size * 0.45, cy, cx - size * 0.35, cy + size / 2));
        }
        /// <summary>A check mark as one chalk stroke.</summary>
        public static void Check(DrawingContext dc, double x, double y, double size, Color color, bool outline = true)
        {
            var g = Poly(false, x + size * 0.08, y + size * 0.55, x + size * 0.38, y + size * 0.85, x + size * 0.95, y + size * 0.12);
            if (outline) dc.DrawGeometry(null, Pal.P(Pal.Ink, size * 0.34), g);
            dc.DrawGeometry(null, Pal.P(color, size * 0.2), g);
        }
        /// <summary>A padlock: the sign of a locked row, after its name. `size` is its width; it is a little higher.</summary>
        public static void Padlock(DrawingContext dc, double x, double y, double size, Color? fill = null)
        {
            Color c = fill ?? Pal.Mix(Pal.LockedGrey, Pal.Cream, 0.25);
            double top = y + size * 0.46, height = size * 0.62;
            var shackle = new StreamGeometry();
            using (var g = shackle.Open())
            {
                g.BeginFigure(new Point(x + size * 0.25, top + 1), false, false);
                g.LineTo(new Point(x + size * 0.25, y + size * 0.30), true, true);
                g.ArcTo(new Point(x + size * 0.75, y + size * 0.30), new Size(size * 0.25, size * 0.25), 0, false, SweepDirection.Clockwise, true, true);
                g.LineTo(new Point(x + size * 0.75, top + 1), true, true);
            }
            shackle.Freeze();
            dc.DrawGeometry(null, Pal.P(Pal.Ink, size * 0.40), shackle);
            dc.DrawGeometry(null, Pal.P(c, size * 0.16), shackle);
            dc.DrawRoundedRectangle(Pal.B(c), Pal.P(Pal.Ink, 2), new Rect(x, top, size, height), 2, 2);
            dc.DrawEllipse(Pal.B(Pal.Ink), null, new Point(x + size / 2, top + height * 0.48), size * 0.10, size * 0.10);
        }
        /// <summary>Warning triangle in flame with an ink "!".</summary>
        public static void Warning(DrawingContext dc, double x, double y, double size, Color? fill = null)
        {
            dc.DrawGeometry(Pal.B(fill ?? Pal.Flame), Pal.P(Pal.Ink, 2), Poly(true, x + size / 2, y, x + size, y + size * 0.92, x, y + size * 0.92));
            dc.DrawLine(Pal.P(Pal.Ink, size * 0.13), new Point(x + size / 2, y + size * 0.36), new Point(x + size / 2, y + size * 0.62));
            dc.DrawEllipse(Pal.B(Pal.Ink), null, new Point(x + size / 2, y + size * 0.77), size * 0.07, size * 0.07);
        }
        public static void Folder(DrawingContext dc, double x, double y, double size, Color fill)
        {
            var g = Poly(true, x, y + size * 0.2, x + size * 0.4, y + size * 0.2, x + size * 0.5, y + size * 0.34, x + size, y + size * 0.34,
                x + size, y + size * 0.9, x, y + size * 0.9);
            dc.DrawGeometry(Pal.B(fill), Pal.P(Pal.Ink, 2), g);
        }
        /// <summary>A small picture: marks a texture pack.</summary>
        public static void Picture(DrawingContext dc, double x, double y, double size, Color fill)
        {
            var frame = new Rect(x, y + size * 0.12, size, size * 0.76);
            dc.DrawRoundedRectangle(Pal.B(fill), Pal.P(Pal.Ink, 2), frame, 2, 2);
            dc.DrawGeometry(Pal.B(Pal.Ink, 0.75), null, Poly(true, x + size * 0.1, y + size * 0.8, x + size * 0.38, y + size * 0.42, x + size * 0.58, y + size * 0.66,
                x + size * 0.72, y + size * 0.52, x + size * 0.92, y + size * 0.8));
            dc.DrawEllipse(Pal.B(Pal.Ink, 0.75), null, new Point(x + size * 0.72, y + size * 0.32), size * 0.08, size * 0.08);
        }
        /// <summary>The letters on a controller button's picture, or null where the picture is a drawn shape.</summary>
        static string PadLabel(string button, PadKind look)
        {
            if (button.StartsWith("Dpad")) return null;
            if (look == PadKind.PlayStation)
            {
                switch (button)
                {
                    case "LB": return "L1"; case "RB": return "R1"; case "LT": return "L2"; case "RT": return "R2";
                    case "LS": return "L3"; case "RS": return "R3";
                }
                return null;        // Cross, Circle, Square, Triangle, Options, Share: shapes
            }
            switch (button)
            {
                case "Start": return "\u2630";
                case "Back": return "\u29C9";
            }
            return button;
        }
        /// <summary>How wide PadGlyph draws this button.</summary>
        public static double PadGlyphWidth(string button, double size, PadKind look)
        {
            string label = PadLabel(button, look);
            return label != null && label.Length > 1 ? size + PillExtra : size;
        }
        const double PillExtra = 10;

        // The four shapes of a PlayStation controller, in the colours they have had since 1994.
        static readonly Color PsCross = Pal.Hex("#8FB4FF"), PsCircle = Pal.HealthRed, PsSquare = Pal.Hex("#F3A6E0"), PsTriangle = Pal.Hex("#6FE0B0");

        /// <summary>
        /// A controller button: an ink disc (a pill for the shoulder buttons and the sticks) with what the
        /// button carries. Xbox: the coloured letters A B X Y, LB RB LT RT, LS RS, the Menu and View signs.
        /// PlayStation: cross, circle, square, triangle, L1 R1 L2 R2, L3 R3, Options, Share (Create). The
        /// buttons are named by position as ChiliNative.ini names them (A, B, X, Y, LB ... DpadRight); "DpadLR"
        /// is the D-pad with left and right lit. Returns the width drawn.
        /// </summary>
        public static double PadGlyph(DrawingContext dc, string button, double x, double y, double size = 20, double alpha = 1, PadKind look = PadKind.Xbox)
        {
            string label = PadLabel(button, look);
            double w = label != null && label.Length > 1 ? size + PillExtra : size;
            var box = new Rect(x, y, w, size);
            dc.DrawRoundedRectangle(Pal.B(Pal.Ink, alpha), Pal.P(Pal.Cream, 1.5, 0.55 * alpha), box, size / 2, size / 2);
            double cx = x + w / 2, cy = y + size / 2;
            if (label != null)
            {
                Color c = Pal.Cream;
                if (look == PadKind.Xbox)
                {
                    if (button == "A") c = Pal.ChalkGreen; else if (button == "B") c = Pal.HealthRed; else if (button == "X") c = Pal.Hex("#6FB6FF"); else if (button == "Y") c = Pal.Gold;
                }
                bool symbol = button == "Start" || button == "Back";
                Tx.DrawCentered(dc, label, symbol ? Fonts.Ui : Fonts.Semi, label.Length > 1 ? size * 0.56 : (symbol ? size * 0.55 : size * 0.62), Pal.B(c, alpha), box, -0.5);
            }
            else if (button.StartsWith("Dpad"))
            {
                double a = size * 0.14, l = size * 0.34;
                string[] dirs = { "DpadUp", "DpadRight", "DpadDown", "DpadLeft" };
                for (int i = 0; i < 4; i++)
                {
                    double ang = i * Math.PI / 2;
                    double dx = Math.Sin(ang), dy = -Math.Cos(ang);
                    var tip = new Point(cx + dx * l, cy + dy * l);
                    var b1 = new Point(cx + dx * a * 0.6 - dy * a, cy + dy * a * 0.6 + dx * a);
                    var b2 = new Point(cx + dx * a * 0.6 + dy * a, cy + dy * a * 0.6 - dx * a);
                    bool lit = dirs[i] == button || (button == "DpadLR" && (i == 1 || i == 3));
                    dc.DrawGeometry(Pal.B(lit ? Pal.Flame : Pal.Cream, (lit ? 1 : 0.45) * alpha), null, Poly(true, tip.X, tip.Y, b1.X, b1.Y, b2.X, b2.Y));
                }
            }
            else
            {
                double s = size, t = Math.Max(1.4, s * 0.105);
                switch (button)
                {
                    case "A":
                        dc.DrawLine(Pal.P(PsCross, t, alpha), new Point(cx - s * 0.20, cy - s * 0.20), new Point(cx + s * 0.20, cy + s * 0.20));
                        dc.DrawLine(Pal.P(PsCross, t, alpha), new Point(cx + s * 0.20, cy - s * 0.20), new Point(cx - s * 0.20, cy + s * 0.20));
                        break;
                    case "B":
                        dc.DrawEllipse(null, Pal.P(PsCircle, t, alpha), new Point(cx, cy), s * 0.235, s * 0.235);
                        break;
                    case "X":
                        dc.DrawGeometry(null, Pal.P(PsSquare, t, alpha), Poly(true, cx - s * 0.20, cy - s * 0.20, cx + s * 0.20, cy - s * 0.20, cx + s * 0.20, cy + s * 0.20, cx - s * 0.20, cy + s * 0.20));
                        break;
                    case "Y":
                        dc.DrawGeometry(null, Pal.P(PsTriangle, t, alpha), Poly(true, cx, cy - s * 0.27, cx + s * 0.25, cy + s * 0.17, cx - s * 0.25, cy + s * 0.17));
                        break;
                    case "Start":
                        // Options: three lines.
                        for (int k = -1; k <= 1; k++)
                            dc.DrawLine(Pal.P(Pal.Cream, Math.Max(1.2, s * 0.085), alpha), new Point(cx - s * 0.21, cy + k * s * 0.17), new Point(cx + s * 0.21, cy + k * s * 0.17));
                        break;
                    case "Back":
                        // Share / Create: the small upright button with three rays above it.
                        var ray = Pal.P(Pal.Cream, Math.Max(1.2, s * 0.085), alpha);
                        dc.DrawRoundedRectangle(Pal.B(Pal.Cream, alpha), null, new Rect(cx - s * 0.085, cy - s * 0.01, s * 0.17, s * 0.30), s * 0.085, s * 0.085);
                        dc.DrawLine(ray, new Point(cx, cy - s * 0.14), new Point(cx, cy - s * 0.30));
                        dc.DrawLine(ray, new Point(cx - s * 0.15, cy - s * 0.08), new Point(cx - s * 0.27, cy - s * 0.19));
                        dc.DrawLine(ray, new Point(cx + s * 0.15, cy - s * 0.08), new Point(cx + s * 0.27, cy - s * 0.19));
                        break;
                }
            }
            return w;
        }
        /// <summary>The cap of a keyboard key with its name: the prompts along the bottom when the keyboard was used last.</summary>
        public static double KeyCap(DrawingContext dc, string name, double x, double y, double h = 18)
        {
            double size = h * 0.62;
            double w = Math.Max(h + 2, Math.Ceiling(Tx.Width(name, Fonts.Semi, size)) + 10);
            var box = new Rect(x, y, w, h);
            dc.DrawRoundedRectangle(Pal.B(Pal.Ink), Pal.P(Pal.Cream, 1.5, 0.55), box, 4, 4);
            Tx.DrawCentered(dc, name, Fonts.Semi, size, Pal.B(Pal.Cream), box, -0.5);
            return w;
        }
        /// <summary>A mouse with the given button (1-3) or wheel direction (4 up, 5 down) in flame.</summary>
        public static double MouseGlyph(DrawingContext dc, string input, double x, double y, double h = 18)
        {
            double w = h * 0.68;
            var body = new Rect(x, y, w, h);
            dc.DrawRoundedRectangle(Pal.B(Pal.Cream), Pal.P(Pal.Ink, 1.5), body, w / 2, w / 2);
            dc.DrawLine(Pal.P(Pal.Ink, 1.2), new Point(x, y + h * 0.42), new Point(x + w, y + h * 0.42));
            dc.DrawLine(Pal.P(Pal.Ink, 1.2), new Point(x + w / 2, y), new Point(x + w / 2, y + h * 0.42));
            var flame = Pal.B(Pal.HotOrange);
            string n = input.ToLowerInvariant();
            if (n == "mouse1")
            {
                var g = new CombinedGeometry(GeometryCombineMode.Intersect, new RectangleGeometry(new Rect(x, y, w / 2 - 0.6, h * 0.42 - 0.6)), new RectangleGeometry(body, w / 2, w / 2));
                dc.DrawGeometry(flame, null, g);
            }
            else if (n == "mouse2")
            {
                var g = new CombinedGeometry(GeometryCombineMode.Intersect, new RectangleGeometry(new Rect(x + w / 2 + 0.6, y, w / 2 - 0.6, h * 0.42 - 0.6)), new RectangleGeometry(body, w / 2, w / 2));
                dc.DrawGeometry(flame, null, g);
            }
            else if (n == "mouse3" || n.StartsWith("wheel"))
            {
                dc.DrawRoundedRectangle(flame, Pal.P(Pal.Ink, 1), new Rect(x + w / 2 - 2.2, y + h * 0.1, 4.4, h * 0.28), 2, 2);
            }
            else
            {
                dc.DrawRoundedRectangle(flame, Pal.P(Pal.Ink, 1), new Rect(x - 1.5, y + h * 0.46, 3.5, h * 0.22), 1.5, 1.5);
            }
            return w;
        }

        static Geometry chiliBody, chiliStem, chiliShine, slimBody, slimStem, slimShine;
        static Geometry Frozen(string path) { var g = Geometry.Parse(path); g.Freeze(); return g; }
        static void BuildChili()
        {
            if (chiliBody != null) return;
            // The plump one of the emblem, and the long thin one that sits above the title.
            chiliBody = Frozen("M 7,8 C 13,1 30,1 43,8 C 50,12 55,14 60,12 C 55,20 40,22 27,19 C 16,17 6,15 7,8 Z");
            chiliStem = Frozen("M 6,10 C 2,8 0,4 1,0 L 5,1 C 5,4 7,6 10,7 Z");
            chiliShine = Frozen("M 14,7 C 22,4 32,5 40,9");
            slimBody = Frozen("M 8,5 C 17,0.5 33,1.5 46,7 C 52,9.5 57,10.5 60,8.5 C 56,14 44,15.5 30,13.5 C 20,12.5 9,11.5 8,5 Z");
            slimStem = Frozen("M 8,7.5 C 4,6.5 2,3.5 3,0 L 6.5,1 C 6.5,3 8,4.5 10.5,5 Z");
            slimShine = Frozen("M 15,4.6 C 23,2.8 33,4 41,7.4");
        }
        /// <summary>One chili, 60 units long, drawn with its stem at the origin and pointing right.</summary>
        static void Chili(DrawingContext dc, double stroke, bool slim)
        {
            BuildChili();
            dc.DrawGeometry(Pal.B(Pal.Stem), Pal.P(Pal.Ink, stroke), slim ? slimStem : chiliStem);
            dc.DrawGeometry(Pal.B(Pal.Chili), Pal.P(Pal.Ink, stroke), slim ? slimBody : chiliBody);
            dc.DrawGeometry(null, Pal.P(Pal.Hex("#FF8A70"), slim ? 1.1 : 1.6, 0.9), slim ? slimShine : chiliShine);
        }
        /// <summary>Two chilies crossed: the launcher's one ornament. (x,y) is the top-left, width the span.</summary>
        public static void CrossedChilies(DrawingContext dc, double x, double y, double width, bool slim = false)
        {
            double k = width / 60.0;
            double stroke = slim ? 2.4 / k : Math.Max(1.2, 3.0 / k);
            double angle = slim ? 13 : 16, cy = slim ? 7.5 : 11;
            dc.PushTransform(new TranslateTransform(x, y));
            dc.PushTransform(new ScaleTransform(k, k));
            // One points right and down, the other left and down; they cross at the middle.
            dc.PushTransform(new RotateTransform(angle, 30, cy));
            Chili(dc, stroke, slim);
            dc.Pop();
            dc.PushTransform(new ScaleTransform(-1, 1, 30, 0));
            dc.PushTransform(new RotateTransform(angle, 30, cy));
            Chili(dc, stroke, slim);
            dc.Pop();
            dc.Pop();
            dc.Pop();
            dc.Pop();
        }

        /// <summary>The picture the Play page shows when the art folder has none: a sun disc with the crossed chilies.</summary>
        public static void Emblem(DrawingContext dc, double cx, double cy, double radius)
        {
            var disc = new RadialGradientBrush { GradientOrigin = new Point(0.5, 0.42), Center = new Point(0.5, 0.5), RadiusX = 0.5, RadiusY = 0.5 };
            disc.GradientStops.Add(new GradientStop(Pal.Flame, 0));
            disc.GradientStops.Add(new GradientStop(Pal.Ember, 0.62));
            disc.GradientStops.Add(new GradientStop(Pal.Rust, 1));
            disc.Freeze();
            dc.DrawEllipse(Pal.B(Pal.Ink, 0.55), null, new Point(cx + radius * 0.05, cy + radius * 0.07), radius * 1.04, radius * 1.04);
            dc.DrawEllipse(disc, Pal.P(Pal.Ink, radius * 0.07), new Point(cx, cy), radius, radius);
            // Sun rays: every other wedge a little lighter.
            var rays = new StreamGeometry();
            using (var c = rays.Open())
            {
                int n = 16;
                for (int i = 0; i < n; i += 2)
                {
                    double a0 = (i - 0.5) * 2 * Math.PI / n, a1 = (i + 0.5) * 2 * Math.PI / n;
                    double r0 = radius * 0.30, r1 = radius * 0.90;
                    c.BeginFigure(new Point(cx + Math.Cos(a0) * r0, cy + Math.Sin(a0) * r0), true, true);
                    c.LineTo(new Point(cx + Math.Cos(a0) * r1, cy + Math.Sin(a0) * r1), false, false);
                    c.ArcTo(new Point(cx + Math.Cos(a1) * r1, cy + Math.Sin(a1) * r1), new Size(r1, r1), 0, false, SweepDirection.Clockwise, false, false);
                    c.LineTo(new Point(cx + Math.Cos(a1) * r0, cy + Math.Sin(a1) * r0), false, false);
                }
            }
            rays.Freeze();
            dc.DrawGeometry(Pal.B(Pal.Gold, 0.22), null, rays);
            dc.DrawEllipse(null, Pal.P(Pal.Gold, radius * 0.03), new Point(cx, cy), radius * 0.92, radius * 0.92);
            double w = radius * 1.5;
            CrossedChilies(dc, cx - w / 2, cy - w * 0.20, w);
        }
    }

    /// <summary>Scorched paper: gradient, soot, vignette and grain, computed once per window size.</summary>
    public static class Backdrop
    {
        sealed class Rng
        {
            uint s;
            public Rng(uint seed) { s = seed; }
            public double Next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return (s & 0xFFFFFF) / (double)0x1000000; }
            public double Range(double a, double b) { return a + (b - a) * Next(); }
        }

        /// <summary>Renders the backdrop for a window of w by h pixels. `tile` is art\background.png or null.</summary>
        public static BitmapSource Render(int w, int h, BitmapSource tile)
        {
            w = Math.Max(16, w); h = Math.Max(16, h);
            double unit = Math.Min(w / 1280.0, h / 720.0);   // one logical pixel
            // Soot at an eighth of the resolution: blobs are blurred by 40, so nothing is lost.
            int q = 8, sw = w / q + 2, sh = h / q + 2;
            var soot = new float[sw * sh];
            for (int i = 0; i < soot.Length; i++) soot[i] = 1f;       // transmission
            var rng = new Rng(0xC0FFEE11);
            for (int b = 0; b < 60; b++)
            {
                double bx = rng.Next() * w, by = rng.Next() * h;
                double rx = rng.Range(20, 140) * unit, ry = rx * rng.Range(0.55, 1.0);
                double alpha = rng.Range(0.08, 0.28);
                double blur = 40 * unit;
                double ex = rx + blur, ey = ry + blur;
                int x0 = Math.Max(0, (int)((bx - ex) / q)), x1 = Math.Min(sw - 1, (int)((bx + ex) / q) + 1);
                int y0 = Math.Max(0, (int)((by - ey) / q)), y1 = Math.Min(sh - 1, (int)((by + ey) / q) + 1);
                for (int y = y0; y <= y1; y++)
                    for (int x = x0; x <= x1; x++)
                    {
                        double dx = (x * q - bx) / ex, dy = (y * q - by) / ey;
                        double d = Math.Sqrt(dx * dx + dy * dy);
                        if (d >= 1) continue;
                        double inner = rx / ex * 0.6;
                        double f = d <= inner ? 1 : 1 - (d - inner) / (1 - inner);
                        f = f * f * (3 - 2 * f);
                        soot[y * sw + x] *= (float)(1 - alpha * f);
                    }
            }
            // Radial gradient table: flame core, ember at half, rust at 0.8, dark rust in the corners.
            const int N = 1024;
            var gr = new float[N + 1]; var gg = new float[N + 1]; var gb = new float[N + 1];
            Color[] stops = { Pal.Flame, Pal.Ember, Pal.Rust, Pal.RustDark };
            double[] at = { 0.0, 0.5, 0.8, 1.0 };
            for (int i = 0; i <= N; i++)
            {
                double t = i / (double)N;
                int k = 0;
                while (k < 2 && t > at[k + 1]) k++;
                double u = (t - at[k]) / (at[k + 1] - at[k]);
                u = Math.Max(0, Math.Min(1, u));
                var c = Pal.Mix(stops[k], stops[k + 1], u);
                gr[i] = c.R; gg[i] = c.G; gb[i] = c.B;
            }
            double cx = 0.62 * w, cy = 0.28 * h;
            double far = 0;
            foreach (var corner in new[] { new Point(0, 0), new Point(w, 0), new Point(0, h), new Point(w, h) })
                far = Math.Max(far, Math.Sqrt((corner.X - cx) * (corner.X - cx) + (corner.Y - cy) * (corner.Y - cy)));
            double mx = w / 2.0, my = h / 2.0, mfar = Math.Sqrt(mx * mx + my * my);
            // The optional game tile, as luminance-preserving multiply at 35%.
            int tw = 0, th = 0;
            byte[] tpx = null;
            if (tile != null)
            {
                try
                {
                    var conv = new FormatConvertedBitmap(tile, PixelFormats.Bgr32, null, 0);
                    tw = conv.PixelWidth; th = conv.PixelHeight;
                    tpx = new byte[tw * th * 4];
                    conv.CopyPixels(tpx, tw * 4, 0);
                }
                catch { tpx = null; }
            }
            double tileScale = tpx != null ? tw / (512.0 * unit) : 0;
            var px = new int[w * h];
            uint noise = 0x9E3779B9;
            for (int y = 0; y < h; y++)
            {
                double dy = y - cy, vy = y - my;
                int sy = y / q;
                float fy = (y % q) / (float)q;
                for (int x = 0; x < w; x++)
                {
                    double dx = x - cx;
                    double t = Math.Sqrt(dx * dx + dy * dy) / far;
                    int gi = (int)(t * N);
                    if (gi > N) gi = N;
                    float r = gr[gi], g = gg[gi], b = gb[gi];
                    // soot, bilinear
                    int sx = x / q;
                    float fx = (x % q) / (float)q;
                    int o = sy * sw + sx;
                    float s0 = soot[o] + (soot[o + 1] - soot[o]) * fx;
                    float s1 = soot[o + sw] + (soot[o + sw + 1] - soot[o + sw]) * fx;
                    float tr = s0 + (s1 - s0) * fy;
                    // vignette
                    double vx = x - mx;
                    double vd = Math.Sqrt(vx * vx + vy * vy) / mfar;
                    double v = vd < 0.3 ? 0 : (vd - 0.3) / 0.7;
                    v = v * v * (3 - 2 * v) * 0.55;
                    float k = tr * (float)(1 - v);
                    r = Pal.Ink.R + (r - Pal.Ink.R) * k; g = Pal.Ink.G + (g - Pal.Ink.G) * k; b = Pal.Ink.B + (b - Pal.Ink.B) * k;
                    if (tpx != null)
                    {
                        int tx = (int)(x * tileScale) % tw, ty = (int)(y * tileScale) % th;
                        int ti = (ty * tw + tx) * 4;
                        float lum = (tpx[ti] * 0.11f + tpx[ti + 1] * 0.59f + tpx[ti + 2] * 0.30f) / 255f;
                        float m = 1f + (lum * 1.6f - 1f) * 0.35f;
                        r *= m; g *= m; b *= m;
                    }
                    // grain, 6 %
                    noise ^= noise << 13; noise ^= noise >> 17; noise ^= noise << 5;
                    float n = 1f + (((noise & 0xFFFF) / 32768f) - 1f) * 0.06f;
                    r *= n; g *= n; b *= n;
                    int R = r < 0 ? 0 : r > 255 ? 255 : (int)r, G = g < 0 ? 0 : g > 255 ? 255 : (int)g, B = b < 0 ? 0 : b > 255 ? 255 : (int)b;
                    px[y * w + x] = (R << 16) | (G << 8) | B;
                }
            }
            var bmp = BitmapSource.Create(w, h, 96, 96, PixelFormats.Bgr32, null, px, w * 4);
            bmp.Freeze();
            return bmp;
        }

        /// <summary>The card with its torn top edge, in logical coordinates (330,150 - 1240,600).</summary>
        public static Geometry Card(Rect r, double radius)
        {
            var rng = new Rng(0x7EA4ED6E);
            var g = new StreamGeometry();
            using (var c = g.Open())
            {
                double x = r.Left;
                c.BeginFigure(new Point(x, r.Top + 4), true, true);
                while (x < r.Right)
                {
                    x = Math.Min(r.Right, x + rng.Range(6, 18));
                    c.LineTo(new Point(x, r.Top + rng.Range(-6, 8)), true, false);
                }
                c.LineTo(new Point(r.Right, r.Bottom - radius), true, false);
                c.ArcTo(new Point(r.Right - radius, r.Bottom), new Size(radius, radius), 0, false, SweepDirection.Clockwise, true, false);
                c.LineTo(new Point(r.Left + radius, r.Bottom), true, false);
                c.ArcTo(new Point(r.Left, r.Bottom - radius), new Size(radius, radius), 0, false, SweepDirection.Clockwise, true, false);
            }
            g.Freeze();
            return g;
        }
    }

    /// <summary>The optional pictures of art\ beside the launcher (DESIGN.md 3.6). Missing means drawn.</summary>
    public sealed class ArtFolder
    {
        public BitmapSource Logo, Background, Hero;
        readonly string dir;
        readonly Dictionary<string, BitmapSource> packs = new Dictionary<string, BitmapSource>(StringComparer.OrdinalIgnoreCase);
        public ArtFolder(string baseDir, bool use)
        {
            dir = Path.Combine(baseDir, "art");
            if (!use) return;
            // art\ first; else the same pictures where "Dump textures.cmd" of the package writes them (mods\TEXTURES.md).
            string dumped = Path.Combine(baseDir, "textures", "data", "textures", "menu");
            Logo = Load("logo.png") ?? Load(Path.Combine(dumped, "gamelogo_frontscreen.png"));
            Background = Load("background.png") ?? Load(Path.Combine(dumped, "backgrounds", "background01.png"));
            Hero = Load("hero.png") ?? Load(Path.Combine(dumped, "loadscreens", "loadscreenmacho1.png"));
        }
        BitmapSource Load(string name)
        {
            try
            {
                string p = Path.IsPathRooted(name) ? name : Path.Combine(dir, name);
                if (!File.Exists(p)) return null;
                var bmp = new BitmapImage();
                bmp.BeginInit();
                bmp.CacheOption = BitmapCacheOption.OnLoad;
                bmp.CreateOptions = BitmapCreateOptions.IgnoreColorProfile;
                bmp.UriSource = new Uri(p);
                bmp.EndInit();
                bmp.Freeze();
                return bmp;
            }
            catch { return null; }
        }
        /// <summary>
        /// The picture blurred by a Gaussian of `sigma` of its own pixels. A 480 x 272 load screen shown at
        /// two or three times its size on a large monitor reads as a failed enlargement; put out of focus
        /// on purpose it reads as a backdrop.
        /// </summary>
        public static BitmapSource Soft(BitmapSource picture, double sigma)
        {
            try
            {
                var conv = new FormatConvertedBitmap(picture, PixelFormats.Pbgra32, null, 0);
                int w = conv.PixelWidth, h = conv.PixelHeight;
                var px = new byte[w * h * 4];
                conv.CopyPixels(px, w * 4, 0);
                int radius = Math.Max(1, (int)Math.Ceiling(sigma * 2.6));
                var kernel = new float[radius * 2 + 1];
                float total = 0;
                for (int i = -radius; i <= radius; i++) { kernel[i + radius] = (float)Math.Exp(-i * i / (2 * sigma * sigma)); total += kernel[i + radius]; }
                for (int i = 0; i < kernel.Length; i++) kernel[i] /= total;
                var mid = new float[w * h * 4];
                for (int y = 0; y < h; y++)
                    for (int x = 0; x < w; x++)
                        for (int ch = 0; ch < 4; ch++)
                        {
                            float sum = 0;
                            for (int i = -radius; i <= radius; i++) sum += kernel[i + radius] * px[(y * w + Math.Max(0, Math.Min(w - 1, x + i))) * 4 + ch];
                            mid[(y * w + x) * 4 + ch] = sum;
                        }
                for (int y = 0; y < h; y++)
                    for (int x = 0; x < w; x++)
                        for (int ch = 0; ch < 4; ch++)
                        {
                            float sum = 0;
                            for (int i = -radius; i <= radius; i++) sum += kernel[i + radius] * mid[(Math.Max(0, Math.Min(h - 1, y + i)) * w + x) * 4 + ch];
                            px[(y * w + x) * 4 + ch] = (byte)Math.Max(0, Math.Min(255, Math.Round(sum)));
                        }
                var soft = BitmapSource.Create(w, h, 96, 96, PixelFormats.Pbgra32, null, px, w * 4);
                soft.Freeze();
                return soft;
            }
            catch { return picture; }
        }

        public BitmapSource Pack(string mod)
        {
            BitmapSource b;
            if (!packs.TryGetValue(mod, out b)) { b = Load("pack_" + mod + ".png"); packs[mod] = b; }
            return b;
        }
    }
}
