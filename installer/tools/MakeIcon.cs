// The program's icon, drawn by code: a scorched round badge with one chili over a small flame, in the
// launcher's palette (profiles/chili/launcher/src/Theme.cs). Nothing of it comes from the game or its box.
//
//   MakeIcon.exe <out folder> [--sheet <file.png>]
//
// writes app_icon.ico (16, 20, 24, 32, 40, 48, 64, 128 and 256 pixels; 256 as PNG, the others as 32-bit
// bitmaps) and app_icon.png (1024 x 1024). Each size is drawn on its own, not scaled down from a larger one:
// at 16 and 20 pixels the flame and the rays are left out and the outlines are one pixel wide, so the chili
// stays readable. --sheet also writes a picture of every size on a dark and a light ground, for checking.
using System;
using System.Collections.Generic;
using System.IO;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using ChiliLauncher;

[assembly: System.Runtime.Versioning.TargetFramework(".NETFramework,Version=v4.8", FrameworkDisplayName = ".NET Framework 4.8")]

namespace ChiliSetup.Tools
{
    public static class MakeIcon
    {
        static readonly int[] sizes = { 16, 20, 24, 32, 40, 48, 64, 128, 256 };

        [STAThread]
        public static int Main(string[] args)
        {
            if (args.Length < 1) { Console.Error.WriteLine("usage: MakeIcon.exe <out folder> [--sheet <file.png>]"); return 2; }
            string outDir = Path.GetFullPath(args[0]);
            string sheet = null;
            for (int i = 1; i < args.Length; i++) if (args[i] == "--sheet" && i + 1 < args.Length) sheet = args[++i];
            Directory.CreateDirectory(outDir);
            var images = new List<KeyValuePair<int, BitmapSource>>();
            foreach (int s in sizes) images.Add(new KeyValuePair<int, BitmapSource>(s, Render(s)));
            WriteIco(Path.Combine(outDir, "app_icon.ico"), images);
            SavePng(Render(1024), Path.Combine(outDir, "app_icon.png"));
            Console.WriteLine("wrote " + Path.Combine(outDir, "app_icon.ico") + " and app_icon.png");
            if (sheet != null) { SavePng(Sheet(images), sheet); Console.WriteLine("wrote " + sheet); }
            return 0;
        }

        /// <summary>One size of the icon, drawn for that size.</summary>
        public static BitmapSource Render(int size)
        {
            var visual = new DrawingVisual();
            using (var dc = visual.RenderOpen())
            {
                dc.PushTransform(new ScaleTransform(size / 256.0, size / 256.0));
                Draw(dc, size);
                dc.Pop();
            }
            var bitmap = new RenderTargetBitmap(size, size, 96, 96, PixelFormats.Pbgra32);
            bitmap.Render(visual);
            bitmap.Freeze();
            return bitmap;
        }

        /// <summary>The icon in a 256 by 256 space; `pixels` is the size it ends up at, which decides the detail.</summary>
        public static void Draw(DrawingContext dc, int pixels)
        {
            bool tiny = pixels <= 20, small = pixels <= 32;
            double px = 256.0 / pixels;                       // one pixel of the result, in this space
            var centre = new Point(128, 128);
            double rim = tiny ? 127 - px * 0.5 : 120;
            // A hard shadow under the badge (not at the small sizes, where it would only blur the edge).
            if (!small) dc.DrawEllipse(Pal.B(Pal.Ink, 0.55), null, new Point(132, 134), rim, rim);
            // The rim: dark rust with an ink edge.
            double edge = tiny ? px : Math.Max(px * 1.2, 7);
            dc.DrawEllipse(Pal.B(Pal.RustDark), Pal.P(Pal.Ink, edge), centre, rim, rim);
            // The core: flame at its upper middle to ember to rust at the rim.
            double core = rim - (tiny ? px * 1.6 : small ? 18 : 16);
            var fill = new RadialGradientBrush { GradientOrigin = new Point(0.5, 0.36), Center = new Point(0.5, 0.45), RadiusX = 0.62, RadiusY = 0.62 };
            fill.GradientStops.Add(new GradientStop(Pal.Mix(Pal.Flame, Pal.Gold, 0.25), 0));
            fill.GradientStops.Add(new GradientStop(Pal.Ember, 0.55));
            fill.GradientStops.Add(new GradientStop(Pal.Rust, 1));
            fill.Freeze();
            dc.DrawEllipse(fill, tiny ? null : Pal.P(Pal.Ink, Math.Max(px, 3), 0.6), centre, core, core);
            var coreClip = new EllipseGeometry(centre, core, core);
            coreClip.Freeze();
            dc.PushClip(coreClip);
            if (!small)
            {
                // Sun rays, every other wedge a little lighter, as on the launcher's emblem; then a grain of soot.
                var rays = new StreamGeometry();
                using (var c = rays.Open())
                {
                    int n = 16;
                    for (int i = 0; i < n; i += 2)
                    {
                        double a0 = (i - 0.5) * 2 * Math.PI / n - Math.PI / 2, a1 = (i + 0.5) * 2 * Math.PI / n - Math.PI / 2;
                        double r0 = core * 0.18, r1 = core * 1.05;
                        c.BeginFigure(new Point(128 + Math.Cos(a0) * r0, 128 + Math.Sin(a0) * r0), true, true);
                        c.LineTo(new Point(128 + Math.Cos(a0) * r1, 128 + Math.Sin(a0) * r1), false, false);
                        c.LineTo(new Point(128 + Math.Cos(a1) * r1, 128 + Math.Sin(a1) * r1), false, false);
                        c.LineTo(new Point(128 + Math.Cos(a1) * r0, 128 + Math.Sin(a1) * r0), false, false);
                    }
                }
                rays.Freeze();
                dc.DrawGeometry(Pal.B(Pal.Gold, 0.16), null, rays);
                if (pixels >= 64)
                {
                    uint s = 0xC0FFEE11;
                    for (int i = 0; i < 220; i++)
                    {
                        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
                        double x = 20 + (s & 0xFFFF) / 65535.0 * 216;
                        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
                        double y = 20 + (s & 0xFFFF) / 65535.0 * 216;
                        double r = 0.6 + (s >> 28) / 16.0 * 1.4;
                        dc.DrawEllipse(Pal.B(Pal.Ink, 0.10 + (s >> 24 & 7) / 70.0), null, new Point(x, y), r, r);
                    }
                }
                // The flame at the bottom of the badge: three tongues, an inked red outline, orange, a gold core.
                Flame(dc, 128, 238, 1.0, Math.Max(px * 1.2, 5));
            }
            else if (!tiny)
            {
                Flame(dc, 128, 236, 1.05, px);
            }
            dc.Pop();
            // A gold highlight along the rim's upper left.
            if (!tiny)
            {
                var arc = new StreamGeometry();
                using (var c = arc.Open())
                {
                    double r = rim - (small ? 6 : 8);
                    double a0 = Math.PI * 1.08, a1 = Math.PI * 1.42;
                    c.BeginFigure(new Point(128 + Math.Cos(a0) * r, 128 + Math.Sin(a0) * r), false, false);
                    c.ArcTo(new Point(128 + Math.Cos(a1) * r, 128 + Math.Sin(a1) * r), new Size(r, r), 0, false, SweepDirection.Clockwise, true, false);
                }
                arc.Freeze();
                dc.DrawGeometry(null, Pal.P(Pal.Gold, small ? px : 5, 0.85), arc);
            }
            // The chili: the launcher's plump one, nearly upright, its stem at the top, leaning 20 degrees.
            Chili(dc, tiny ? px * 1.1 : small ? px * 1.3 : Math.Max(px * 1.4, 6.5), tiny);
        }

        static void Flame(DrawingContext dc, double cx, double baseY, double k, double stroke)
        {
            // Outline: three tongues, the middle one tallest, each hooked a little.
            var outer = Tongues(cx, baseY, new[] { -50.0, 0.0, 48.0 }, new[] { 44.0, 58.0, 44.0 }, new[] { 64.0, 98.0, 70.0 }, k);
            var middle = Tongues(cx, baseY, new[] { -44.0, 0.0, 42.0 }, new[] { 30.0, 42.0, 30.0 }, new[] { 44.0, 74.0, 48.0 }, k);
            var inner = Tongues(cx, baseY, new[] { -2.0, 0.0, 2.0 }, new[] { 0.1, 26.0, 0.1 }, new[] { 0.0, 46.0, 0.0 }, k);
            dc.DrawGeometry(Pal.B(Pal.Chili), Pal.P(Pal.Ink, stroke), outer);
            dc.DrawGeometry(Pal.B(Pal.HotOrange), null, middle);
            dc.DrawGeometry(Pal.B(Pal.Gold), null, inner);
        }

        static Geometry Tongues(double cx, double baseY, double[] offsets, double[] widths, double[] heights, double k)
        {
            var g = new StreamGeometry { FillRule = FillRule.Nonzero };
            using (var c = g.Open())
            {
                for (int i = 0; i < offsets.Length; i++)
                {
                    if (heights[i] <= 0) continue;
                    double x = cx + offsets[i] * k, w = widths[i] * k, h = heights[i] * k, lean = (i - 1) * 6 * k + 4 * k;
                    var from = new Point(x - w / 2, baseY);
                    var tip = new Point(x + lean, baseY - h);
                    var to = new Point(x + w / 2, baseY);
                    c.BeginFigure(from, true, true);
                    c.BezierTo(new Point(from.X - 0.1 * w, baseY - 0.55 * h), new Point(tip.X - 0.35 * w, tip.Y + 0.40 * h), tip, true, true);
                    c.BezierTo(new Point(tip.X + 0.30 * w, tip.Y + 0.42 * h), new Point(to.X + 0.12 * w, baseY - 0.5 * h), to, true, true);
                }
            }
            g.Freeze();
            return g;
        }

        // The launcher's plump chili (Theme.cs, Art.BuildChili): 60 units long, its stem at the origin, pointing right.
        static readonly Geometry body = Geometry.Parse("M 7,8 C 13,1 30,1 43,8 C 50,12 55,14 60,12 C 55,20 40,22 27,19 C 16,17 6,15 7,8 Z");
        static readonly Geometry stem = Geometry.Parse("M 6,10 C 2,8 0,4 1,0 L 5,1 C 5,4 7,6 10,7 Z");
        static readonly Geometry shine = Geometry.Parse("M 14,7 C 22,4 32,5 40,9");

        static void Chili(DrawingContext dc, double stroke, bool tiny)
        {
            const double k = 3.55;                      // 60 units -> 213 of the 256
            var t = new TransformGroup();
            t.Children.Add(new TranslateTransform(-31, -11));
            t.Children.Add(new ScaleTransform(k, k));
            t.Children.Add(new RotateTransform(110));
            t.Children.Add(new TranslateTransform(134, 118));
            t.Freeze();
            dc.PushTransform(t);
            double s = stroke / k;
            if (!tiny)
            {
                // Its hard shadow on the badge.
                dc.PushTransform(new TranslateTransform(1.6, -1.2));
                dc.DrawGeometry(Pal.B(Pal.Ink, 0.45), null, body);
                dc.Pop();
            }
            dc.DrawGeometry(Pal.B(Pal.Stem), Pal.P(Pal.Ink, s), stem);
            dc.DrawGeometry(Pal.B(Pal.Chili), Pal.P(Pal.Ink, s), body);
            dc.DrawGeometry(null, Pal.P(Pal.Hex("#FF8A70"), tiny ? s * 0.9 : Math.Max(s * 0.55, 1.5), 0.95), shine);
            dc.Pop();
        }

        static void SavePng(BitmapSource image, string file)
        {
            var encoder = new PngBitmapEncoder();
            encoder.Frames.Add(BitmapFrame.Create(image));
            using (var f = File.Create(file)) encoder.Save(f);
        }

        static byte[] PngBytes(BitmapSource image)
        {
            var encoder = new PngBitmapEncoder();
            encoder.Frames.Add(BitmapFrame.Create(image));
            using (var m = new MemoryStream()) { encoder.Save(m); return m.ToArray(); }
        }

        /// <summary>The .ico container: a 6-byte header, a 16-byte entry per image, then the images.</summary>
        static void WriteIco(string file, List<KeyValuePair<int, BitmapSource>> images)
        {
            var data = new List<byte[]>();
            foreach (var pair in images) data.Add(pair.Key >= 256 ? PngBytes(pair.Value) : Dib(pair.Value));
            using (var f = File.Create(file))
            using (var w = new BinaryWriter(f))
            {
                w.Write((ushort)0); w.Write((ushort)1); w.Write((ushort)images.Count);
                int offset = 6 + 16 * images.Count;
                for (int i = 0; i < images.Count; i++)
                {
                    int s = images[i].Key;
                    w.Write((byte)(s >= 256 ? 0 : s)); w.Write((byte)(s >= 256 ? 0 : s));
                    w.Write((byte)0); w.Write((byte)0);
                    w.Write((ushort)1); w.Write((ushort)32);
                    w.Write(data[i].Length); w.Write(offset);
                    offset += data[i].Length;
                }
                foreach (var d in data) w.Write(d);
            }
        }

        /// <summary>A 32-bit bitmap as an icon holds it: the header with the height doubled, the rows bottom up, an empty mask.</summary>
        static byte[] Dib(BitmapSource image)
        {
            var straight = new FormatConvertedBitmap(image, PixelFormats.Bgra32, null, 0);
            int w = straight.PixelWidth, h = straight.PixelHeight;
            var px = new byte[w * h * 4];
            straight.CopyPixels(px, w * 4, 0);
            int maskRow = ((w + 31) / 32) * 4;
            using (var m = new MemoryStream())
            using (var b = new BinaryWriter(m))
            {
                b.Write(40); b.Write(w); b.Write(h * 2); b.Write((ushort)1); b.Write((ushort)32); b.Write(0);
                b.Write(w * h * 4 + maskRow * h); b.Write(0); b.Write(0); b.Write(0); b.Write(0);
                for (int y = h - 1; y >= 0; y--) b.Write(px, y * w * 4, w * 4);
                b.Write(new byte[maskRow * h]);
                return m.ToArray();
            }
        }

        /// <summary>Every size at 1:1 and the four smallest four times enlarged, on a dark and a light ground.</summary>
        static BitmapSource Sheet(List<KeyValuePair<int, BitmapSource>> images)
        {
            var big = Render(512);
            var visual = new DrawingVisual();
            int width = 1180, height = 700;
            using (var dc = visual.RenderOpen())
            {
                dc.DrawRectangle(new SolidColorBrush(Color.FromRgb(32, 32, 32)), null, new Rect(0, 0, width, height / 2));
                dc.DrawRectangle(new SolidColorBrush(Color.FromRgb(236, 236, 236)), null, new Rect(0, height / 2, width, height / 2));
                for (int band = 0; band < 2; band++)
                {
                    double y0 = band * height / 2 + 12, x = 12;
                    foreach (var pair in images)
                    {
                        dc.DrawImage(pair.Value, new Rect(x, y0, pair.Key, pair.Key));
                        x += pair.Key + 10;
                    }
                    x = 12;
                    double y1 = y0 + 266;
                    foreach (var pair in images)
                    {
                        if (pair.Key > 20) continue;
                        var group = new DrawingGroup();
                        RenderOptions.SetBitmapScalingMode(group, BitmapScalingMode.NearestNeighbor);
                        group.Children.Add(new ImageDrawing(pair.Value, new Rect(0, 0, pair.Key * 3, pair.Key * 3)));
                        dc.PushTransform(new TranslateTransform(x + 700, y0));
                        dc.DrawDrawing(group);
                        dc.Pop();
                        x += pair.Key * 3 + 10;
                    }
                }
                dc.DrawImage(big, new Rect(width - 330, 20, 300, 300));
            }
            var bitmap = new RenderTargetBitmap(width, height, 96, 96, PixelFormats.Pbgra32);
            bitmap.Render(visual);
            return bitmap;
        }
    }
}
