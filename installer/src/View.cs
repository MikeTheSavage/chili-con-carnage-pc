// The picture: everything Setup shows is drawn here in a 1024 x 640 space that WPF scales by the monitor's
// DPI (the launcher's palette, backdrop, torn card and drawn ornaments; the game's own pictures only from the
// player's disc, once accepted). The picture also says where its controls are, for the mouse, the keyboard
// and the controller.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Windows;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using ChiliLauncher;

namespace ChiliSetup
{
    public enum HotKind { Button, Primary, Toggle, Field, Slider, Mute, Rail, Choice }

    public sealed class Hot
    {
        public string Id;
        public Rect R;
        public HotKind Kind;
        public bool Enabled = true;
        public int Order;
    }

    public sealed class SetupView : FrameworkElement
    {
        public const double W = 1024, H = 640;
        public static readonly Rect CardRect = new Rect(270, 116, 730, 424);
        public readonly SetupModel M;
        public bool Static;
        public double StaticTime = 1.3;
        readonly System.Diagnostics.Stopwatch clock = System.Diagnostics.Stopwatch.StartNew();
        public double Now { get { return Static ? StaticTime : clock.Elapsed.TotalSeconds; } }
        public bool WindowActive = true;

        public string FocusId;
        public bool ShowFocus;
        string hoverId, pressedId;
        bool dragging;
        public Action<string> Activated = delegate { };
        public Action<string, double> SliderMoved = delegate { };
        public Action<string> Dropped = delegate { };

        List<Hot> hots = new List<Hot>();
        double scale = 1, ox, oy;
        BitmapSource backdrop;
        int backdropW, backdropH;
        double pageAt = -10, railFrom, railAt = -10;
        Page shownPage;

        public SetupView(SetupModel m)
        {
            M = m;
            shownPage = m.Page;
            Focusable = true;
            FocusVisualStyle = null;
            AllowDrop = true;
            SnapsToDevicePixels = true;
        }

        public IList<Hot> Hots { get { return hots; } }

        /// <summary>A page change starts its small fade; the rail's pointer slides.</summary>
        public void PageChanged()
        {
            if (shownPage == M.Page) return;
            railFrom = StepOf(shownPage);
            shownPage = M.Page;
            pageAt = Now;
            railAt = Now;
            FocusId = null;
            InvalidateVisual();
        }
        static int StepOf(Page p) { return p == Page.Uninstall ? 0 : Math.Min(5, (int)p); }

        /// <summary>True while something moves by itself (the window asks for frames only then).</summary>
        public bool Moving
        {
            get
            {
                if (Static || !WindowActive) return false;
                double t = Now;
                if (t - pageAt < 0.2 || t - railAt < 0.2) return true;
                if (M.Page == Page.Install && M.Installing) return true;
                if (M.Page == Page.Uninstall && M.Removing == 1) return true;
                if (ShowFocus && M.Device == Device.Pad && FocusId != null) return true;
                if (M.Checking || M.FolderChecking) return true;
                return false;
            }
        }

        // ---------------------------------------------------------------- input

        public Point ToLogical(Point p) { return new Point((p.X - ox) / scale, (p.Y - oy) / scale); }
        Hot HitTest(Point logical)
        {
            for (int i = hots.Count - 1; i >= 0; i--) if (hots[i].R.Contains(logical)) return hots[i];
            return null;
        }
        public Hot Find(string id) { return hots.FirstOrDefault(h => h.Id == id); }

        protected override void OnMouseMove(MouseEventArgs e)
        {
            base.OnMouseMove(e);
            var p = ToLogical(e.GetPosition(this));
            if (dragging && pressedId != null)
            {
                var h = Find(pressedId);
                if (h != null && h.Kind == HotKind.Slider) SliderMoved(h.Id, (p.X - h.R.X) / h.R.Width);
                return;
            }
            var hit = M.Ask != null ? HitTest(p) : HitTest(p);
            string id = hit != null && hit.Enabled ? hit.Id : null;
            if (id != hoverId) { hoverId = id; InvalidateVisual(); }
            Cursor = id != null ? Cursors.Hand : null;
        }
        protected override void OnMouseLeave(MouseEventArgs e)
        {
            base.OnMouseLeave(e);
            if (hoverId != null) { hoverId = null; InvalidateVisual(); }
        }
        protected override void OnMouseLeftButtonDown(MouseButtonEventArgs e)
        {
            base.OnMouseLeftButtonDown(e);
            Focus();
            M.Device = Device.Mouse;
            ShowFocus = false;
            var hit = HitTest(ToLogical(e.GetPosition(this)));
            if (hit == null || !hit.Enabled) { InvalidateVisual(); return; }
            pressedId = hit.Id;
            FocusId = hit.Id;
            if (hit.Kind == HotKind.Slider)
            {
                dragging = true;
                CaptureMouse();
                SliderMoved(hit.Id, (ToLogical(e.GetPosition(this)).X - hit.R.X) / hit.R.Width);
            }
            else CaptureMouse();
            InvalidateVisual();
        }
        protected override void OnMouseLeftButtonUp(MouseButtonEventArgs e)
        {
            base.OnMouseLeftButtonUp(e);
            string was = pressedId;
            bool wasDragging = dragging;
            pressedId = null;
            dragging = false;
            ReleaseMouseCapture();
            var hit = HitTest(ToLogical(e.GetPosition(this)));
            if (!wasDragging && was != null && hit != null && hit.Id == was && hit.Enabled) Activated(was);
            InvalidateVisual();
        }
        protected override void OnMouseWheel(MouseWheelEventArgs e)
        {
            base.OnMouseWheel(e);
            var hit = HitTest(ToLogical(e.GetPosition(this)));
            if (hit != null && (hit.Kind == HotKind.Slider || hit.Kind == HotKind.Mute) && M.MusicOn)
                SliderMoved("volume", (M.Volume + (e.Delta > 0 ? 5 : -5)) / 100.0 - 0.0);
        }
        protected override void OnDragEnter(DragEventArgs e) { DragFeedback(e, true); }
        protected override void OnDragOver(DragEventArgs e) { DragFeedback(e, true); }
        protected override void OnDragLeave(DragEventArgs e) { M.DragOver = false; InvalidateVisual(); }
        void DragFeedback(DragEventArgs e, bool over)
        {
            bool ok = M.Page == Page.Disc && e.Data.GetDataPresent(DataFormats.FileDrop) && M.Ask == null;
            e.Effects = ok ? DragDropEffects.Copy : DragDropEffects.None;
            e.Handled = true;
            if (M.DragOver != (ok && over)) { M.DragOver = ok && over; InvalidateVisual(); }
        }
        protected override void OnDrop(DragEventArgs e)
        {
            M.DragOver = false;
            InvalidateVisual();
            if (M.Page != Page.Disc || M.Ask != null) return;
            var files = e.Data.GetData(DataFormats.FileDrop) as string[];
            if (files != null && files.Length > 0) Dropped(files[0]);
        }

        /// <summary>The focusable controls in their order (the dialog's only, while one is open).</summary>
        public List<Hot> FocusOrder()
        {
            var list = hots.Where(h => h.Enabled && (M.Ask == null || h.Kind == HotKind.Choice)).OrderBy(h => h.Order).ToList();
            return list;
        }
        public void MoveFocus(int delta)
        {
            var list = FocusOrder();
            if (list.Count == 0) return;
            int i = list.FindIndex(h => h.Id == FocusId);
            if (i < 0) i = delta > 0 ? DefaultFocusIndex(list) - 1 : DefaultFocusIndex(list) + 1;
            i = ((i + delta) % list.Count + list.Count) % list.Count;
            FocusId = list[i].Id;
            ShowFocus = true;
            InvalidateVisual();
        }
        int DefaultFocusIndex(List<Hot> list)
        {
            int i = list.FindIndex(h => h.Kind == HotKind.Primary);
            if (M.Ask != null) i = list.FindIndex(h => h.Id == "q" + M.Ask.Focus);
            return Math.Max(0, i);
        }
        /// <summary>The focused control, or the primary button when nothing is focused.</summary>
        public Hot Focused
        {
            get
            {
                var h = FocusId != null ? Find(FocusId) : null;
                return h != null && h.Enabled ? h : null;
            }
        }
        public void EnsureFocus()
        {
            if (Focused != null) return;
            var list = FocusOrder();
            if (list.Count > 0) FocusId = list[DefaultFocusIndex(list)].Id;
        }

        // ---------------------------------------------------------------- drawing

        void EnsureBackdrop(double w, double h, double dpi)
        {
            double f = Math.Min(1.0, 1600.0 / Math.Max(1, w * dpi));
            int bw = Math.Max(16, (int)Math.Round(w * dpi * f)), bh = Math.Max(16, (int)Math.Round(h * dpi * f));
            if (backdrop != null && Math.Abs(bw - backdropW) < 3 && Math.Abs(bh - backdropH) < 3) return;
            backdrop = Backdrop.Render(bw, bh, null);
            backdropW = bw; backdropH = bh;
        }

        protected override void OnDpiChanged(DpiScale oldDpi, DpiScale newDpi)
        {
            base.OnDpiChanged(oldDpi, newDpi);
            InvalidateVisual();
        }

        Hot Add(string id, Rect r, HotKind kind, int order, bool enabled = true)
        {
            var h = new Hot { Id = id, R = r, Kind = kind, Order = order, Enabled = enabled };
            hots.Add(h);
            return h;
        }
        bool IsHover(string id) { return hoverId == id; }
        bool IsPressed(string id) { return pressedId == id && hoverId == id; }
        bool IsFocus(string id) { return ShowFocus && FocusId == id; }

        /// <summary>Screenshots: the picture into any drawing.</summary>
        public void Paint(DrawingContext dc) { OnRender(dc); }

        protected override void OnRender(DrawingContext dc)
        {
            double w = ActualWidth, h = ActualHeight;
            if (w < 1 || h < 1) return;
            if (!Static)
            {
                try { if (PresentationSource.FromVisual(this) != null) Tx.PixelsPerDip = VisualTreeHelper.GetDpi(this).PixelsPerDip; } catch { }
            }
            scale = Math.Min(w / W, h / H);
            ox = (w - W * scale) / 2; oy = (h - H * scale) / 2;
            EnsureBackdrop(w, h, Tx.PixelsPerDip);
            dc.DrawRectangle(Pal.B(Pal.RustDark), null, new Rect(0, 0, w, h));
            dc.DrawImage(backdrop, new Rect(0, 0, w, h));
            dc.PushTransform(new MatrixTransform(scale, 0, 0, scale, ox, oy));
            hots = new List<Hot>();
            double t = Now;

            Header(dc);
            if (M.Uninstaller) Emblem(dc); else Rail(dc, t);
            Card(dc, t);
            BottomBar(dc, t);
            Prompts(dc);
            if (M.Ask != null) Dialog(dc, t);
            dc.Pop();
        }

        // The header: the crossed chilies, the title in Changa One, the version; the disc's own badge once accepted.
        void Header(DrawingContext dc)
        {
            Art.CrossedChilies(dc, 26, 30, 62);
            const string title = "CHILI CON CARNAGE";
            Tx.Outlined(dc, title, Faces.Display, 44, 100, 18, Pal.LogoFill, 4, 3, 4, 0.6);
            double tw = Tx.Width(title, Faces.Display, 44);
            Tx.Outlined(dc, M.Uninstaller ? "PC" : "PC", Faces.Display, 26, 100 + tw + 12, 36, Pal.B(Pal.Chili), 3, 2, 3, 0.5);
            Tx.DrawRight(dc, M.Version, Faces.Numbers, 16, Pal.B(Pal.Gold), 1000, 22);
            if (M.Icon0 != null && !M.Uninstaller)
            {
                var r = new Rect(1000 - 72, 50, 72, 40);
                dc.DrawRoundedRectangle(Pal.B(Pal.Ink, 0.55), null, new Rect(r.X - 2, r.Y - 2, r.Width + 4, r.Height + 4), 4, 4);
                var g = new DrawingGroup();
                RenderOptions.SetBitmapScalingMode(g, BitmapScalingMode.HighQuality);
                g.Children.Add(new ImageDrawing(M.Icon0, r));
                dc.DrawDrawing(g);
            }
        }

        void Emblem(DrawingContext dc)
        {
            Art.Emblem(dc, 137, 300, 92);
        }

        void Rail(DrawingContext dc, double t)
        {
            int current = StepOf(M.Page);
            bool locked = M.Installing || M.Page == Page.Install || M.Page == Page.Done;
            // The pointer slides between steps.
            double k = Math.Min(1, (t - railAt) / 0.15);
            k = k * k * (3 - 2 * k);
            double pointerStep = railFrom + (current - railFrom) * k;
            for (int i = 0; i < SetupModel.Steps.Length; i++)
            {
                double y = 140 + i * 56;
                bool done = i < current || (M.Page == Page.Done && !M.Failed && i == 5);
                bool reachable = !locked && i < current && i <= M.Furthest;
                Color c = i == current ? Pal.Gold : done ? Pal.HotOrange : Pal.A(Pal.Cream, 0.42);
                var r = new Rect(24, y - 6, 226, 44);
                if (reachable)
                {
                    var hot = Add("rail" + i, r, HotKind.Rail, 400 + i);
                    if (IsHover(hot.Id)) dc.DrawRoundedRectangle(Pal.B(Pal.Cream, 0.07), null, r, 8, 8);
                    if (IsFocus(hot.Id)) FocusRing(dc, r, 8, t);
                }
                if (done && i != current) Art.Check(dc, 30, y + 7, 18, Pal.ChalkGreen);
                Tx.Outlined(dc, SetupModel.Steps[i], Faces.Display, 20, 60, y, Pal.B(c), 2.5, 1.5, 2, 0.45);
            }
            double py = 140 + pointerStep * 56 + 7;
            if (!(M.Page == Page.Done && !M.Failed)) Art.Pointer(dc, 32, py, 18, Pal.Gold);
        }

        void Card(DrawingContext dc, double t)
        {
            var card = Backdrop.Card(CardRect, 8);
            dc.DrawGeometry(Pal.B(Pal.Soot, 0.78), Pal.P(Pal.Ink, 2, 0.6), card);
            if (M.DragOver) dc.DrawGeometry(null, Pal.P(Pal.Gold, 3), card);
            // The disc's title picture as a backdrop in the card's lower right, from page 2 on.
            if (M.Pic1 != null && M.Page != Page.Welcome && !M.Uninstaller)
            {
                dc.PushClip(card);
                var r = new Rect(1000 - 444, 540 - 252, 444, 252);
                bool soft = Tx.PixelsPerDip * scale * r.Width / M.Pic1.PixelWidth > 1.5;
                var left = new LinearGradientBrush { MappingMode = BrushMappingMode.Absolute, StartPoint = new Point(r.X, 0), EndPoint = new Point(r.X + 140, 0) };
                left.GradientStops.Add(new GradientStop(Colors.Transparent, 0));
                left.GradientStops.Add(new GradientStop(Colors.Black, 1));
                left.Freeze();
                var top = new LinearGradientBrush { MappingMode = BrushMappingMode.Absolute, StartPoint = new Point(0, r.Y), EndPoint = new Point(0, r.Y + 130) };
                top.GradientStops.Add(new GradientStop(Colors.Transparent, 0));
                top.GradientStops.Add(new GradientStop(Colors.Black, 1));
                top.Freeze();
                dc.PushOpacityMask(left);
                dc.PushOpacityMask(top);
                dc.PushOpacity(0.32);
                var g = new DrawingGroup();
                RenderOptions.SetBitmapScalingMode(g, BitmapScalingMode.HighQuality);
                g.Children.Add(new ImageDrawing(soft && M.Pic1Soft != null ? M.Pic1Soft : M.Pic1, r));
                dc.DrawDrawing(g);
                dc.Pop(); dc.Pop(); dc.Pop();
                dc.Pop();
            }
            double k = Math.Min(1, (t - pageAt) / 0.12);
            if (k < 1) { dc.PushOpacity(k); dc.PushTransform(new TranslateTransform(0, 12 * (1 - k))); }
            switch (M.Page)
            {
                case Page.Welcome: Welcome(dc, t); break;
                case Page.Disc: DiscPage(dc, t); break;
                case Page.Folder: FolderPage(dc, t); break;
                case Page.Options: OptionsPage(dc, t); break;
                case Page.Install: InstallPage(dc, t); break;
                case Page.Done: DonePage(dc, t); break;
                case Page.Uninstall: UninstallPage(dc, t); break;
            }
            if (k < 1) { dc.Pop(); dc.Pop(); }
        }

        const double X0 = 298, Right = 972;

        void Title(DrawingContext dc, string s)
        {
            Tx.Outlined(dc, s, Faces.Display, 30, X0, 128, Pal.B(Pal.Gold), 3, 2, 3, 0.5);
        }
        void Line(DrawingContext dc, string s, double y, Color? c = null, double size = 17)
        {
            Tx.Draw(dc, s, Faces.Line, size, Pal.B(c ?? Pal.Cream), X0, y, Right - X0, 1);
        }
        void Problem(DrawingContext dc, string s, double x, double y, double maxWidth)
        {
            Art.Warning(dc, x, y + 2, 18, Pal.Flame);
            Tx.Draw(dc, s, Faces.Label, 16, Pal.B(Pal.Problem), x + 28, y, maxWidth - 28, 1);
        }
        void Good(DrawingContext dc, string s, double x, double y, double maxWidth, Typeface face = null)
        {
            Art.Check(dc, x, y + 3, 17, Pal.ChalkGreen);
            Tx.Draw(dc, s, face ?? Faces.Label, 16, Pal.B(Pal.ChalkGreen), x + 28, y, maxWidth - 28, 1);
        }

        void Welcome(DrawingContext dc, double t)
        {
            Title(dc, "Welcome");
            Line(dc, "This installs the PC version of Chili Con Carnage.", 192);
            Line(dc, "It needs your own copy of the PSP game as an .iso file.", 220);
            double y = 284;
            Fact(dc, "Version", M.Version, y);
            Fact(dc, "Disk space", Util.Mb(M.Need), y + 40);
            Fact(dc, "Windows", "10, 11", y + 80);
            Art.Emblem(dc, 862, 372, 86);
            if (M.Damaged != null) Problem(dc, M.Damaged, X0, 450, 520);
        }
        void Fact(DrawingContext dc, string name, string value, double y)
        {
            Tx.Draw(dc, name, Faces.Label, 17, Pal.B(Pal.Cream, 0.75), X0, y);
            Tx.Draw(dc, value, Faces.Numbers, 17, Pal.B(Pal.Gold), X0 + 150, y);
        }

        void DiscPage(DrawingContext dc, double t)
        {
            Title(dc, "Your disc");
            Line(dc, "Choose the .iso of Chili Con Carnage (USA, ULUS-10216), or drop it here.", 192);
            // The drop area.
            var zone = new Rect(X0, 228, Right - X0, 134);
            var dash = new Pen(Pal.B(M.DragOver ? Pal.Gold : Pal.Cream, M.DragOver ? 1 : 0.28), M.DragOver ? 2.5 : 1.6) { DashStyle = new DashStyle(new double[] { 5, 4 }, 0) };
            dash.Freeze();
            dc.DrawRoundedRectangle(Pal.B(Pal.Ink, M.DragOver ? 0.45 : 0.28), dash, zone, 10, 10);
            double cy = zone.Y + zone.Height / 2;
            if (M.DiscOk && M.Icon0 != null)
            {
                var r = new Rect(zone.X + 22, cy - 40, 144, 80);
                var g = new DrawingGroup();
                RenderOptions.SetBitmapScalingMode(g, BitmapScalingMode.HighQuality);
                g.Children.Add(new ImageDrawing(M.Icon0, r));
                dc.DrawRoundedRectangle(Pal.B(Pal.Ink, 0.6), null, new Rect(r.X - 3, r.Y - 3, r.Width + 6, r.Height + 6), 5, 5);
                dc.DrawDrawing(g);
            }
            else DiscGlyph(dc, zone.X + 94, cy, 46, M.DiscOk ? Pal.ChalkGreen : M.Checking ? Pal.Gold : Pal.Cream, t);
            double tx = zone.X + 196;
            if (M.Checking) Tx.Draw(dc, "Checking…", Faces.Label, 18, Pal.B(Pal.Cream), tx, cy - 12);
            else if (M.DiscOk)
            {
                Tx.Draw(dc, "Chili Con Carnage (USA)", Faces.Label, 20, Pal.B(Pal.White), tx, cy - 30);
                Tx.Draw(dc, Disc.Id, Faces.Numbers, 16, Pal.B(Pal.Gold), tx, cy + 2);
                Tx.Draw(dc, M.Disc.IsFolder ? "folder" : Util.Grouped(M.Disc.Size) + " bytes", Faces.Numbers, 16, Pal.B(Pal.Gold), tx + 120, cy + 2);
            }
            // The path and the two buttons.
            double y = 380;
            var field = new Rect(X0, y, 396, 40);
            PathField(dc, "path_iso", field, M.DiscPath, 100, t);
            Button(dc, "browse_iso", new Rect(field.Right + 12, y, 112, 40), "Browse…", false, true, 101, t);
            Button(dc, "browse_disc_folder", new Rect(field.Right + 136, y, Right - field.Right - 136, 40), "Browse folder…", false, true, 102, t);
            // The verdict.
            double vy = 436;
            if (M.Checking) { }
            else if (M.DiscOk) Good(dc, M.Disc.Summary, X0, vy, Right - X0);
            else if (M.Disc != null && M.Disc.Message.Length > 0) Problem(dc, M.Disc.Message, X0, vy, Right - X0);
        }

        static void DiscGlyph(DrawingContext dc, double cx, double cy, double r, Color tint, double t)
        {
            var c = new Point(cx, cy);
            var face = new RadialGradientBrush { GradientOrigin = new Point(0.35, 0.3) };
            face.GradientStops.Add(new GradientStop(Pal.Mix(Pal.Cream, tint, 0.3), 0));
            face.GradientStops.Add(new GradientStop(Pal.Mix(Pal.LockedGrey, Pal.Ink, 0.35), 1));
            face.Freeze();
            dc.DrawEllipse(Pal.B(Pal.Ink, 0.5), null, new Point(cx + 3, cy + 4), r, r);
            dc.PushOpacity(0.75);
            dc.DrawEllipse(face, Pal.P(Pal.Ink, 2.5), c, r, r);
            dc.DrawEllipse(null, Pal.P(Pal.Cream, 1, 0.35), c, r * 0.72, r * 0.72);
            dc.DrawEllipse(Pal.B(Pal.Rust), Pal.P(Pal.Ink, 2), c, r * 0.3, r * 0.3);
            dc.DrawEllipse(Pal.B(Pal.Ink), null, c, r * 0.11, r * 0.11);
            dc.Pop();
        }

        void FolderPage(DrawingContext dc, double t)
        {
            Title(dc, "Folder");
            Line(dc, "Where the game is installed.", 192);
            double y = 232;
            var field = new Rect(X0, y, Right - X0 - 124, 40);
            PathField(dc, "path_dir", field, M.Dir, 100, t);
            Button(dc, "browse_dir", new Rect(field.Right + 12, y, 112, 40), "Browse…", false, true, 101, t);
            double ny = 292;
            Tx.Draw(dc, "Needs " + Util.Mb(M.Need), Faces.Numbers, 16, Pal.B(Pal.Gold), X0, ny);
            double nx = X0 + Tx.Width("Needs " + Util.Mb(M.Need), Faces.Numbers, 16);
            if (M.Folder != null && M.Folder.Free >= 0)
                Tx.Draw(dc, "  ·  " + Util.Free(M.Folder.Free) + " free", Faces.Numbers, 16, Pal.B(Pal.Gold), nx, ny);
            double vy = 332;
            if (M.FolderChecking) Tx.Draw(dc, "Checking…", Faces.Label, 16, Pal.B(Pal.Cream), X0, vy);
            else if (M.Folder != null)
            {
                if (M.Folder.Verdict == FolderVerdict.Update) Good(dc, Folder.TextUpdate, X0, vy, Right - X0);
                else if (!M.Folder.Ok)
                {
                    Problem(dc, M.Folder.Message, X0, vy, M.Folder.Verdict == FolderVerdict.Running ? 470 : Right - X0);
                    if (M.Folder.Verdict == FolderVerdict.Running) Button(dc, "check_again", new Rect(Right - 150, vy - 8, 150, 36), "Check again", false, true, 102, t);
                }
            }
        }

        void OptionsPage(DrawingContext dc, double t)
        {
            Title(dc, "Options");
            double y = 186;
            OptionRow(dc, "toggle_hd", "HD loading pictures", M.HdLoading && M.HasHd, M.HasHd, Util.Mb(M.HdBytes), y, 100, t);
            OptionRow(dc, "toggle_desktop", "Desktop shortcut", M.Desktop, true, null, y + 64, 101, t);
            OptionRow(dc, "toggle_startmenu", "Start menu shortcut", M.StartMenu, true, null, y + 128, 102, t);
            OptionRow(dc, "toggle_launch", "Start the game when done", M.Launch, true, null, y + 192, 103, t);
        }

        void OptionRow(DrawingContext dc, string id, string name, bool on, bool enabled, string value, double y, int order, double t)
        {
            var row = new Rect(X0 - 10, y, Right - X0 + 20, 52);
            var hot = Add(id, row, HotKind.Toggle, order, enabled);
            if (enabled && IsHover(id)) dc.DrawRoundedRectangle(Pal.B(Pal.Cream, 0.06), null, row, 10, 10);
            if (IsFocus(id)) FocusRing(dc, row, 10, t);
            double cy = y + 26;
            Tx.Draw(dc, name, Faces.Label, 19, Pal.B(enabled ? Pal.White : Pal.Locked(Pal.Cream)), X0, cy - 14);
            if (!enabled) Art.Padlock(dc, X0 + Tx.Width(name, Faces.Label, 19) + 12, cy - 11, 16);
            var sw = new Rect(Right - 66, cy - 14, 64, 28);
            Switch(dc, sw, on, enabled, IsHover(id), IsPressed(id));
            if (value != null) Tx.DrawRight(dc, value, Faces.Numbers, 16, Pal.B(enabled ? Pal.Gold : Pal.Locked(Pal.Gold)), sw.X - 16, cy - 11);
        }

        /// <summary>The on/off switch: a capsule that fills with ember when on, a cream knob that slides.</summary>
        static void Switch(DrawingContext dc, Rect r, bool on, bool enabled, bool hover, bool pressed)
        {
            Brush face;
            if (on)
            {
                var g = new LinearGradientBrush { StartPoint = new Point(0, 0), EndPoint = new Point(0, 1) };
                g.GradientStops.Add(new GradientStop(enabled ? Pal.Flame : Pal.Locked(Pal.Flame), 0));
                g.GradientStops.Add(new GradientStop(enabled ? Pal.Ember : Pal.Locked(Pal.Ember), 1));
                g.Freeze();
                face = g;
            }
            else face = Pal.B(pressed ? Pal.RustDark : Pal.Ink, 0.85);
            Art.Capsule(dc, r, face, Pal.P(on ? Pal.Gold : Pal.Cream, 1.6, on ? (enabled ? 0.9 : 0.4) : (hover ? 0.75 : 0.45)));
            double kx = on ? r.Right - r.Height / 2 : r.X + r.Height / 2;
            dc.DrawEllipse(Pal.B(enabled ? (hover ? Pal.White : Pal.Cream) : Pal.Locked(Pal.Cream)), Pal.P(Pal.Ink, 2), new Point(kx, r.Y + r.Height / 2), r.Height / 2 - 4, r.Height / 2 - 4);
        }

        void InstallPage(DrawingContext dc, double t)
        {
            if (M.RolledBack)
            {
                Title(dc, "Not installed");
                Line(dc, "Nothing was installed.", 192);
                return;
            }
            Title(dc, M.Updating ? "Updating…" : "Installing…");
            double fraction = M.Total > 0 ? Math.Max(0, Math.Min(1, M.Done / (double)M.Total)) : 0;
            string amount = (M.Done / 1048576).ToString("N0", CultureInfo.InvariantCulture) + " of " + Util.Mb(M.Total);
            Tx.Draw(dc, amount, Faces.Numbers, 17, Pal.B(Pal.Gold), X0, 252);
            Progress(dc, new Rect(X0, 300, Right - X0, 26), fraction, t, M.Installing);
            string file = M.Current ?? "";
            Tx.Draw(dc, file.Replace('/', '\\'), Faces.Path, 15, Pal.B(Pal.Cream, 0.85), X0, 344, 470, 1);
        }

        /// <summary>The progress bar: the combo meter's gradient fills it, the fire of the HD choice burns at its head.</summary>
        void Progress(DrawingContext dc, Rect r, double fraction, double t, bool burning)
        {
            var track = new Rect(r.X, r.Y, r.Width, r.Height);
            double head = r.X + r.Width * fraction;
            if (burning && fraction > 0.02 && WindowActive && !Static) Art.Flames(dc, new Rect(Math.Max(r.X, head - 90), r.Y, Math.Min(90, head - r.X), r.Height), t);
            else if (burning && fraction > 0.02) Art.Flames(dc, new Rect(Math.Max(r.X, head - 90), r.Y, Math.Min(90, head - r.X), r.Height), 1.3);
            Art.Capsule(dc, track, Pal.B(Pal.Ink, 0.85), Pal.P(Pal.Cream, 1.5, 0.45));
            if (fraction > 0)
            {
                var fill = new Rect(r.X + 3, r.Y + 3, Math.Max(r.Height - 6, (r.Width - 6) * fraction), r.Height - 6);
                Art.Capsule(dc, fill, Pal.ComboFill(r.X, r.Width), null);
                dc.DrawRoundedRectangle(null, Pal.P(Pal.White, 1.2, 0.35), new Rect(fill.X + 4, fill.Y + 2.5, Math.Max(0, fill.Width - 8), 0.1), 0, 0);
            }
        }

        void DonePage(DrawingContext dc, double t)
        {
            if (M.Failed)
            {
                Title(dc, "Not installed");
                Problem(dc, M.Result.Message, X0, 194, Right - X0);
                Button(dc, "show_log", new Rect(X0, 244, 140, 40), "Show log", false, true, 100, t);
                return;
            }
            Title(dc, M.Updating ? "Updated" : "Installed");
            string dir = Tx.MiddleEllipsis(M.Result != null ? M.Result.Dir : M.Dir, Faces.Line, 17, Right - X0 - 260);
            Line(dc, "Chili Con Carnage PC is in " + dir + ".", 192);
            double y = 236;
            if (M.Result != null)
                foreach (var w in M.Result.Warnings) { Problem(dc, w, X0, y, Right - X0); y += 30; }
            y = Math.Max(y + 14, 262);
            double x = X0;
            foreach (var chip in new[] { new[] { "F11", "Full screen" }, new[] { "F1", "Free the mouse" }, new[] { "Esc", "Pause" } })
            {
                x += Art.KeyCap(dc, chip[0], x, y, 26) + 10;
                x += Tx.Draw(dc, chip[1], Faces.Label, 16, Pal.B(Pal.Cream), x, y + 2) + 28;
            }
        }

        void UninstallPage(DrawingContext dc, double t)
        {
            if (M.Removing == 2)
            {
                Title(dc, "Removed");
                Line(dc, M.RemoveAll ? "Chili Con Carnage PC was removed." : "Chili Con Carnage PC was removed. Saves, settings and mods are kept.", 192);
                return;
            }
            if (M.Removing == 3)
            {
                Title(dc, "Not removed");
                Problem(dc, M.RemoveMessage, X0, 194, Right - X0);
                return;
            }
            Title(dc, M.Removing == 1 ? "Removing…" : "Remove Chili Con Carnage PC");
            string dir = Tx.MiddleEllipsis(M.Dir, Faces.Line, 17, Right - X0 - 240);
            Line(dc, "Removes the program from " + dir + ".", 192);
            if (M.Removing == 1) { Progress(dc, new Rect(X0, 300, Right - X0, 26), M.Total > 0 ? M.Done / (double)M.Total : 0, t, true); return; }
            OptionRow(dc, "toggle_removeall", "Delete saves, settings and mods too", M.RemoveAll, true, null, 240, 100, t);
        }

        void BottomBar(DrawingContext dc, double t)
        {
            dc.DrawLine(Pal.P(Pal.Cream, 1, 0.18), new Point(24, 552.5), new Point(1000, 552.5));
            if (!M.Uninstaller) MusicControl(dc, t);
            if (!string.IsNullOrEmpty(M.Message)) Problem(dc, M.Message, 280, 576, 420);
            string back = null, next = null;
            bool nextOn = true, backOn = true;
            switch (M.Page)
            {
                case Page.Welcome: back = "Close"; next = "Next"; nextOn = M.Damaged == null; break;
                case Page.Disc: back = "Back"; next = "Next"; nextOn = M.DiscOk; break;
                case Page.Folder: back = "Back"; next = M.Update ? "Update" : "Next"; nextOn = M.FolderOk; break;
                case Page.Options: back = "Back"; next = M.Update ? "Update" : "Install"; break;
                case Page.Install:
                    if (M.RolledBack) next = "Close";
                    else { back = "Cancel"; backOn = M.Installing; }
                    break;
                case Page.Done: next = M.Failed ? "Close" : (M.Launch ? "Play" : "Finish"); break;
                case Page.Uninstall:
                    if (M.Removing == 0) { back = "Close"; next = "Remove"; }
                    else if (M.Removing == 1) { }
                    else next = "Close";
                    break;
            }
            if (back != null) Button(dc, "back", new Rect(720, 567, 120, 42), back, false, backOn, 300, t);
            if (next != null) Button(dc, "next", new Rect(860, 562, 140, 52), next, true, nextOn, 310, t);
        }

        void MusicControl(DrawingContext dc, double t)
        {
            bool on = M.MusicOn;
            double alpha = on ? 1 : 0.4;
            var speaker = new Rect(24, 567, 42, 42);
            var hot = Add("mute", speaker, HotKind.Mute, 200, on);
            dc.PushOpacity(alpha);
            Art.Capsule(dc, speaker, Pal.B(IsPressed("mute") ? Pal.RustDark : Pal.Ink, 0.75), Pal.P(Pal.Cream, 1.5, IsHover("mute") ? 0.8 : 0.45));
            if (IsFocus("mute")) FocusRing(dc, speaker, 21, t);
            SpeakerGlyph(dc, speaker.X + 21, speaker.Y + 21, M.Muted || !on);
            var track = new Rect(78, 584, 120, 10);
            Add("volume", new Rect(track.X - 6, 572, track.Width + 12, 34), HotKind.Slider, 201, on);
            if (IsFocus("volume")) FocusRing(dc, new Rect(track.X - 8, 576, track.Width + 16, 26), 13, t);
            Art.Capsule(dc, track, Pal.B(Pal.Ink, 0.85), Pal.P(Pal.Cream, 1.2, 0.4));
            double v = M.Volume / 100.0;
            if (v > 0) Art.Capsule(dc, new Rect(track.X + 1.5, track.Y + 1.5, Math.Max(7, (track.Width - 3) * v), track.Height - 3), M.Muted ? Pal.B(Pal.LockedGrey) : Pal.ComboFill(track.X, track.Width), null);
            double kx = track.X + track.Width * v;
            dc.DrawEllipse(Pal.B(IsHover("volume") ? Pal.White : Pal.Cream), Pal.P(Pal.Ink, 2), new Point(kx, track.Y + track.Height / 2), 8, 8);
            double nw = Tx.Draw(dc, M.Volume.ToString(CultureInfo.InvariantCulture), Faces.Numbers, 15, Pal.B(M.Muted ? Pal.LockedGrey : Pal.Gold), 212, 578);
            Tx.Draw(dc, "%", Faces.Label, 14, Pal.B(M.Muted ? Pal.LockedGrey : Pal.Gold), 213 + nw, 579);
            dc.Pop();
        }

        static void SpeakerGlyph(DrawingContext dc, double cx, double cy, bool muted)
        {
            var horn = new StreamGeometry();
            using (var c = horn.Open())
            {
                c.BeginFigure(new Point(cx - 10, cy - 4), true, true);
                c.LineTo(new Point(cx - 5, cy - 4), true, true);
                c.LineTo(new Point(cx + 1, cy - 10), true, true);
                c.LineTo(new Point(cx + 1, cy + 10), true, true);
                c.LineTo(new Point(cx - 5, cy + 4), true, true);
                c.LineTo(new Point(cx - 10, cy + 4), true, true);
            }
            horn.Freeze();
            dc.DrawGeometry(Pal.B(Pal.Cream), Pal.P(Pal.Ink, 1.2), horn);
            if (muted)
            {
                dc.DrawLine(Pal.P(Pal.Problem, 2.4), new Point(cx + 5, cy - 6), new Point(cx + 13, cy + 6));
                dc.DrawLine(Pal.P(Pal.Problem, 2.4), new Point(cx + 13, cy - 6), new Point(cx + 5, cy + 6));
                return;
            }
            for (int i = 1; i <= 2; i++)
            {
                double r = 4.5 * i + 1;
                var arc = new StreamGeometry();
                using (var c = arc.Open())
                {
                    c.BeginFigure(new Point(cx + 2 + r * Math.Cos(-0.85), cy + r * Math.Sin(-0.85)), false, false);
                    c.ArcTo(new Point(cx + 2 + r * Math.Cos(0.85), cy + r * Math.Sin(0.85)), new Size(r, r), 0, false, SweepDirection.Clockwise, true, false);
                }
                arc.Freeze();
                dc.DrawGeometry(null, Pal.P(Pal.Gold, 2), arc);
            }
        }

        /// <summary>The buttons' meanings along the bottom: after a controller its buttons, after the keyboard its keys.</summary>
        void Prompts(DrawingContext dc)
        {
            if (M.Device != Device.Pad && M.Device != Device.Keyboard) return;
            var items = new List<string[]>();
            bool pad = M.Device == Device.Pad;
            if (M.Ask != null) { items.Add(new[] { pad ? "A" : "Enter", "Choose" }); items.Add(new[] { pad ? "B" : "Esc", "Back" }); }
            else
            {
                var next = Find("next");
                if (next != null && next.Enabled) items.Add(new[] { pad ? "Start" : "Enter", NextName() });
                if (pad) items.Add(new[] { "A", "Select" });
                var back = Find("back");
                if (back != null && back.Enabled) items.Add(new[] { pad ? "B" : "Esc", BackName() });
                if (M.Page == Page.Disc || M.Page == Page.Folder) items.Add(new[] { pad ? "Y" : "F", "Browse" });
                if (M.MusicOn) items.Add(new[] { pad ? "X" : "M", M.Muted ? "Sound" : "Mute" });
            }
            // Right-aligned under the buttons.
            double total = 0;
            var widths = new List<double>();
            foreach (var it in items)
            {
                double gw = pad ? Art.PadGlyphWidth(it[0], 18, M.PadLook) : Math.Max(20, Math.Ceiling(Tx.Width(it[0], ChiliLauncher.Fonts.Semi, 18 * 0.62)) + 10);
                double tw = Tx.Width(it[1], Faces.Label, 14);
                widths.Add(gw);
                total += gw + 6 + tw + 18;
            }
            double x = 1000 - total + 18, y = 619;
            for (int i = 0; i < items.Count; i++)
            {
                if (pad) Art.PadGlyph(dc, items[i][0], x, y, 18, 1, M.PadLook);
                else Art.KeyCap(dc, items[i][0], x, y, 18);
                x += widths[i] + 6;
                x += Tx.Draw(dc, items[i][1], Faces.Label, 14, Pal.B(Pal.Cream, 0.85), x, y) + 18;
            }
        }
        string NextName() { var h = Find("next"); return h != null ? nextLabel : ""; }
        string BackName() { return backLabel; }
        string nextLabel = "Next", backLabel = "Back";

        void Dialog(DrawingContext dc, double t)
        {
            dc.DrawRectangle(Pal.B(Pal.Ink, 0.62), null, new Rect(-ox / scale - 1, -oy / scale - 1, W + 2 * ox / scale + 2, H + 2 * oy / scale + 2));
            var r = new Rect(312, 214, 400, 196);
            var card = Backdrop.Card(r, 8);
            dc.DrawGeometry(Pal.B(Pal.Soot, 0.96), Pal.P(Pal.Gold, 2, 0.7), card);
            Tx.Outlined(dc, M.Ask.Text, Faces.Display, 24, r.X + 28, r.Y + 34, Pal.B(Pal.Gold), 2.5, 1.5, 2, 0.45);
            double bx = r.Right - 28;
            for (int i = M.Ask.Buttons.Length - 1; i >= 0; i--)
            {
                bool primary = i == 0;
                double bw = primary ? 130 : Math.Max(120, Tx.Width(M.Ask.Buttons[i], Faces.Label, 17) + 40);
                var br = new Rect(bx - bw, r.Bottom - 30 - (primary ? 50 : 44) + (primary ? 0 : -3), bw, primary ? 50 : 44);
                Button(dc, "q" + i, br, M.Ask.Buttons[i], primary, true, 500 + i, t, HotKind.Choice);
                bx -= bw + 14;
            }
        }

        // ---------------------------------------------------------------- controls

        void FocusRing(DrawingContext dc, Rect r, double radius, double t)
        {
            double a = M.Device == Device.Pad && !Static ? 0.7 + 0.3 * Math.Sin(t * 4.2) : 1;
            var outer = new Rect(r.X - 4, r.Y - 4, r.Width + 8, r.Height + 8);
            dc.DrawRoundedRectangle(null, Pal.P(Pal.Ink, 5, 0.6), outer, radius + 4, radius + 4);
            dc.DrawRoundedRectangle(null, Pal.P(Pal.Gold, 2.4, a), outer, radius + 4, radius + 4);
        }

        void Button(DrawingContext dc, string id, Rect r, string label, bool primary, bool enabled, int order, double t, HotKind kind = HotKind.Button)
        {
            var hot = Add(id, r, kind == HotKind.Choice ? HotKind.Choice : primary ? HotKind.Primary : HotKind.Button, order, enabled);
            if (id == "next") nextLabel = label;
            if (id == "back") backLabel = label;
            bool hover = enabled && IsHover(id), pressed = enabled && IsPressed(id);
            if (primary)
            {
                var face = pressed ? Pal.RustDark : hover ? Pal.Lighten(Pal.Ember, 0.12) : Pal.Ember;
                if (!enabled) face = Pal.Locked(Pal.Ember);
                dc.DrawRoundedRectangle(Pal.B(Pal.Ink, 0.55), null, new Rect(r.X + 3, r.Y + 4, r.Width, r.Height), 10, 10);
                dc.DrawRoundedRectangle(Pal.B(face), Pal.P(Pal.Ink, 3), r, 10, 10);
                dc.DrawRoundedRectangle(null, Pal.P(enabled ? Pal.Gold : Pal.LockedGrey, 1.5, enabled ? 0.85 : 0.5), new Rect(r.X + 4, r.Y + 4, r.Width - 8, r.Height - 8), 7, 7);
                double size = r.Height >= 50 ? 26 : 22;
                var shape = Tx.Shape(label, Faces.Display, size);
                var b = shape.Bounds;
                double x = r.X + (r.Width - b.Width) / 2 - b.X, y = r.Y + (r.Height - b.Height) / 2 - b.Y;
                Tx.Outlined(dc, label, Faces.Display, size, x, y, enabled ? Pal.LogoFill : Pal.B(Pal.Locked(Pal.Gold)), 3, 1.5, 2.5, 0.5, enabled ? 1 : 0.7);
            }
            else
            {
                var face = pressed ? Pal.RustDark : hover ? Pal.Mix(Pal.Soot, Pal.Rust, 0.45) : Pal.Soot;
                Art.Round(dc, r, Math.Min(r.Height / 2, 12), Pal.B(face, enabled ? 0.92 : 0.5), Pal.P(enabled ? Pal.Cream : Pal.LockedGrey, 1.5, enabled ? (hover ? 0.85 : 0.5) : 0.3));
                Tx.DrawCentered(dc, label, Faces.Label, 16, Pal.B(enabled ? Pal.Cream : Pal.Locked(Pal.Cream)), r, -0.5);
            }
            if (IsFocus(id) && enabled) FocusRing(dc, r, primary ? 10 : Math.Min(r.Height / 2, 12), t);
        }

        void PathField(DrawingContext dc, string id, Rect r, string path, int order, double t)
        {
            var hot = Add(id, r, HotKind.Field, order);
            bool hover = IsHover(id);
            Art.Capsule(dc, r, Pal.B(Pal.Ink, 0.7), Pal.P(Pal.Cream, 1.4, hover ? 0.7 : 0.35));
            Art.Folder(dc, r.X + 14, r.Y + 10, 20, Pal.Flame);
            string shown = Tx.MiddleEllipsis(path ?? "", Faces.Path, 15, r.Width - 64);
            if (!string.IsNullOrEmpty(shown)) Tx.Draw(dc, shown, Faces.Path, 15, Pal.B(Pal.Cream), r.X + 44, r.Y + (r.Height - 20) / 2);
            if (IsFocus(id)) FocusRing(dc, r, r.Height / 2, t);
        }
    }
}
