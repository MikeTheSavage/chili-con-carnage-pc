// The whole launcher picture: a 1280x720 logical canvas scaled uniformly into the window (DESIGN.md
// section 1), drawn in one pass, with its own focus model so that mouse, keyboard and gamepad drive
// the same thing (section 4).
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Linq;
using System.Windows;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace ChiliLauncher
{
    public sealed class Page
    {
        public string Name;
        public Func<string> Title;
        public Func<List<Row>> Build;
        public Action Defaults;            // null: the page has none
        public string DefaultsQuestion;
        public Func<double> Right;         // right edge of the rows (the Play page keeps room for its picture)
    }

    public sealed class CaptureState
    {
        public const double Seconds = 5.0;
        public Bindings Control;
        public int Index;
        public double Start;
        public string Note;
        public ushort PadHeld;
        public bool PadTriggersHeld;
        public double Elapsed(Surface ui) { return ui.Static ? 1.6 : ui.Now - Start; }
    }

    public sealed class DialogState
    {
        public string Title, Text;
        public string[] Buttons;
        public Action<int> Done;
        public int Focus;
        public int Cancel = 1;
        public bool Danger;
        public Rect[] Rects = new Rect[0];
    }

    /// <summary>What the player touched last: the prompts along the bottom follow it.</summary>
    public enum InputDevice { None, Keyboard, Mouse, Pad }
    /// <summary>A controller's family: which pictures its buttons get.</summary>
    public enum PadKind { Xbox, PlayStation }

    public struct PadState
    {
        public bool Connected;
        public PadKind Kind;
        public int Index;
        public ushort Buttons;
        public byte LT, RT;
        public short LX, LY, RX, RY;
        /// <summary>A PlayStation controller's product id (0CE6 and 0DF2 are DualSense), else 0.</summary>
        public ushort Pid;
    }

    public sealed partial class Surface : FrameworkElement
    {
        public readonly Model Model;
        public ArtFolder ArtFiles;
        /// <summary>Screenshot mode: nothing animates and the clock stands still.</summary>
        public bool Static;
        readonly Stopwatch clock = Stopwatch.StartNew();
        public double Now { get { return Static ? 1.3 : clock.Elapsed.TotalSeconds; } }

        // What the window does for the picture.
        public Action PlayRequested;
        public Action<bool> ChooseDiscRequested;     // true: an .iso file, false: a folder
        public Action<string> OpenPathRequested;
        public Action RescanRequested;
        public Action SaveRequested;
        /// <summary>The Rumble row's Test button: shake the connected controller briefly, this strong (0 to 1).</summary>
        public Action<double> RumbleRequested;

        readonly List<Page> pages = new List<Page>();
        int page;
        List<Row> rows = new List<Row>();
        double contentHeight;
        double scrollTarget;
        double pageShown = -10;

        enum Zone { Rail, Card, Bottom }
        Zone zone = Zone.Rail;
        int railFocus, rowFocus = -1, sub, bottomFocus = 1;
        bool focusShown;
        InputDevice device;
        bool fromPad;
        /// <summary>The device used last. Nothing touched yet counts as the controller when one is connected.</summary>
        public InputDevice Device { get { return device; } }
        bool padDrove { get { return device == InputDevice.Pad; } }
        void Used(InputDevice d)
        {
            if (device == d) return;
            // The ring that was drawn for a connected controller stays where it is when the first key or button comes.
            if (device == InputDevice.None && Pad.Connected && d != InputDevice.Mouse) focusShown = true;
            device = d;
            InvalidateVisual();
        }
        /// <summary>
        /// True when the focus is drawn: after a key or a controller's button, and from the start for a
        /// player whose controller is connected and who has touched nothing yet. That player is shown the
        /// controller's prompts along the bottom, and a prompt needs something to point at.
        /// </summary>
        bool FocusShown { get { return focusShown || (device == InputDevice.None && Pad.Connected); } }

        // RowBody: anywhere on a locked row that is not one of its control's parts (its name, the space between).
        enum HitKind { None, Rail, Row, RowBody, Defaults, Play, Scroll, More, Chip, MessageButton, DropItem, DropOutside, DialogButton, Blocked }
        struct Hit
        {
            public HitKind Kind; public int Index; public int Part;
            public bool Same(Hit o) { return Kind == o.Kind && Index == o.Index && Part == o.Part; }
        }
        Hit hover, pressed;
        bool dragging;

        // Overlays: at most one at a time.
        Dropdown dropControl;
        Rect dropRect;
        int dropHover = -1;
        public CaptureState Capture;
        DialogState dialog;

        // The message line. A message stands until the next one (or until its time is up). What a locked row
        // has to say when the player tries to use it is laid over the message for a few seconds and never
        // destroys it: the message is there again when the report has gone.
        string message = "";
        Color messageColor = Pal.Cream;
        double messageUntil = double.MaxValue;
        string messageButton;
        Action messageAction;
        Rect messageButtonRect;
        string report = "";
        double reportUntil;
        string reportRow;
        Func<string> reportNow;
        const double ReportSeconds = 6;

        public PadState Pad;
        PadState lastPad;
        double padRepeatAt;
        int padRepeatDir;
        bool padWaitRelease;

        double scale = 1, ox, oy;
        BitmapSource backdrop;
        int backdropW, backdropH;
        Geometry cardShape;
        Rect logoCrop;

        public Surface(Model model, ArtFolder art)
        {
            Model = model;
            ArtFiles = art;
            Focusable = true;
            FocusVisualStyle = null;
            ClipToBounds = true;
            cardShape = Backdrop.Card(new Rect(Lay.CardX, Lay.CardY, Lay.CardW, Lay.CardH), 8);
            layers = new[] { topVisual, railVisual, cardVisual, headVisual, bottomVisual, messageVisual, fireVisual, liveVisual, ringVisual, overlayVisual };
            foreach (var layer in layers) AddVisualChild(layer);
            // A drawn fire moves "on twos": about sixteen pictures a second (Windows' timers tick every 15.6 ms, so 50 ms is 62).
            fireTimer = new System.Windows.Threading.DispatcherTimer(System.Windows.Threading.DispatcherPriority.Render) { Interval = TimeSpan.FromMilliseconds(50) };
            fireTimer.Tick += (s, e) => { if (fire.On && Moving && !Hidden) DrawFire(); else fireTimer.Stop(); };
            ringTimer = new System.Windows.Threading.DispatcherTimer(System.Windows.Threading.DispatcherPriority.Render) { Interval = TimeSpan.FromMilliseconds(60) };
            ringTimer.Tick += (s, e) => { if (rings.Count > 0 && padDrove && Moving && !Hidden) DrawRings(); else ringTimer.Stop(); };
            liveTimer = new System.Windows.Threading.DispatcherTimer(System.Windows.Threading.DispatcherPriority.Render) { Interval = TimeSpan.FromMilliseconds(33) };
            liveTimer.Tick += (s, e) => { if (waiting.On && Capture != null && !Hidden) DrawLive(); else liveTimer.Stop(); };
            BuildPages();
            Model.Changed += () => InvalidateVisual();
            Rebuild();
            if (art != null && art.Logo != null) logoCrop = OpaqueBounds(art.Logo);
        }

        // ---- animation -------------------------------------------------------------------------

        sealed class Tween { public double From, To, Start, Ms; }
        readonly Dictionary<object, Tween> tweens = new Dictionary<object, Tween>();
        bool wantFrame, hooked;
        static bool AnimationsOn { get { try { return SystemParameters.ClientAreaAnimation; } catch { return true; } } }
        /// <summary>False while another window is in front: nothing moves by itself then.</summary>
        public bool WindowActive = true;
        /// <summary>True when something that moves by itself (the flames) may ask for frames.</summary>
        public bool Moving { get { return !Static && AnimationsOn && WindowActive; } }
        double fireClock = 1.3;
        /// <summary>The fire's time: it runs while the launcher may move, and stands where it stopped otherwise.</summary>
        public double FireClock { get { if (Moving) fireClock = Now; return Static ? StaticFireTime : fireClock; } }
        /// <summary>The moment of the fire that screenshots show (--fire-time).</summary>
        public static double StaticFireTime = 1.3;

        public void RequestFrame()
        {
            if (Static) return;
            wantFrame = true;
            if (!hooked) { CompositionTarget.Rendering += OnFrame; hooked = true; }
        }
        void OnFrame(object sender, EventArgs e)
        {
            if (Capture != null && Capture.Elapsed(this) >= CaptureState.Seconds) EndCapture();
            // (A message or a report whose time is up fades and is taken away by Tick, which draws the line alone.)
            if (!wantFrame) { CompositionTarget.Rendering -= OnFrame; hooked = false; return; }
            wantFrame = false;
            InvalidateVisual();
        }

        /// <summary>The value of something that eases to `target` over `ms` milliseconds.</summary>
        public double Animate(object key, double target, double ms)
        {
            if (Static || !AnimationsOn) return target;
            Tween t;
            double now = Now;
            if (!tweens.TryGetValue(key, out t)) { tweens[key] = new Tween { From = target, To = target, Start = now, Ms = ms }; return target; }
            double p = t.Ms <= 0 ? 1 : Math.Min(1, (now - t.Start) * 1000 / t.Ms);
            double e = 1 - (1 - p) * (1 - p);
            double current = t.From + (t.To - t.From) * e;
            if (Math.Abs(t.To - target) > 1e-9) { t.From = current; t.To = target; t.Start = now; t.Ms = ms; p = 0; }
            if (p < 1) RequestFrame();
            return current;
        }

        // ---- layers ---------------------------------------------------------------------------
        //
        // The picture is a stack of visuals, each with a small place of its own, so that what moves by itself
        // is drawn alone and the rest is left as it is: the fire of the HD pack choice (about sixteen times a
        // second while it burns), what follows the controller or a clock (the two stick dots of the Gamepad
        // tab, the waiting chip of a binding capture), the focus ring (it breathes after the controller was
        // used), and on top of them the open list and the question box. Under them the parts that stand still: title and
        // status chip, the rail, the card with its rows, the page's name, the bottom bar, and over the
        // bottom bar the message line (it fades when its time is up, and only it is drawn then). This
        // element itself draws only the backdrop.
        //
        // Why so many: when one visual changes, WPF works out the bounds of its parent's own drawing again
        // and draws every visual that touches the changed place again. With the whole picture in one
        // visual, each frame of the fire cost the outlined lettering of the title, the rail and PLAY once
        // more (measured: 18 % of a processor core against 7 % with the lettering in visuals the fire
        // does not touch).

        readonly DrawingVisual topVisual = new DrawingVisual(), railVisual = new DrawingVisual(), cardVisual = new DrawingVisual(), headVisual = new DrawingVisual(),
            bottomVisual = new DrawingVisual(), messageVisual = new DrawingVisual(), fireVisual = new DrawingVisual(), liveVisual = new DrawingVisual(), ringVisual = new DrawingVisual(),
            overlayVisual = new DrawingVisual();
        DrawingVisual[] layers;
        protected override int VisualChildrenCount { get { return layers == null ? 0 : layers.Length; } }
        protected override Visual GetVisualChild(int index) { return layers[index]; }
        readonly System.Windows.Threading.DispatcherTimer fireTimer, ringTimer, liveTimer;
        MatrixTransform viewTransform;
        /// <summary>Set by the window while it is hidden (the game runs): nothing is drawn then.</summary>
        public bool Hidden;

        // What is in force while the rows are drawn: the page's fade and slide, a row's own fade, the list's clip.
        double layerAlpha = 1, layerShift, layerRowY;
        Rect layerClip;
        bool layerClipOn, inOverlay, overlayShown;

        struct FireSpot { public bool On, Pressed; public Rect Box; public string Label; public double Alpha, Shift; public Geometry Clip; }
        FireSpot fire;
        /// <summary>
        /// A choice that burns (called while its row is drawn). The flames, the ember capsule and its name are
        /// drawn in the fire's own layer, so that only they are drawn again while the fire moves. The fire may
        /// rise up to 20 px above the list's top edge while its row sits at the top.
        /// </summary>
        public void Burn(Rect box, string label, bool pressed)
        {
            double rise = layerClipOn ? 20 * Math.Max(0, Math.Min(1, 1 - (layerClip.Top + 5 - layerRowY) / 6)) : 0;
            Geometry clip = null;
            if (layerClipOn) { clip = new RectangleGeometry(new Rect(layerClip.X, layerClip.Top - rise, layerClip.Width, layerClip.Height + rise)); clip.Freeze(); }
            fire = new FireSpot { On = true, Pressed = pressed, Box = box, Label = label, Alpha = layerAlpha, Shift = layerShift, Clip = clip };
        }
        void DrawFire()
        {
            using (var dc = fireVisual.RenderOpen())
            {
                if (fire.On)
                {
                    PartRenders++;
                    double t = FireClock;
                    dc.PushTransform(viewTransform);
                    if (fire.Shift != 0) dc.PushTransform(new TranslateTransform(0, fire.Shift));
                    if (fire.Clip != null) dc.PushClip(fire.Clip);
                    if (fire.Alpha < 1) dc.PushOpacity(fire.Alpha);
                    Art.Flames(dc, fire.Box, t);
                    var r = fire.Box;
                    if (fire.Pressed) r.Offset(1, 1);
                    Art.Capsule(dc, r, fire.Pressed ? Pal.B(Pal.RustDark) : Art.EmberFill(t), Pal.P(Pal.Ink, 2));
                    Tx.DrawCentered(dc, fire.Label, Fonts.Semi, 15, Pal.B(fire.Pressed ? Pal.PaleYellow : Pal.Ink), r, -1);
                }
            }
            // It moves only while the launcher may move by itself: the front window, animations on, not a screenshot.
            if (fire.On && Moving && !Hidden) { if (!fireTimer.IsEnabled) fireTimer.Start(); }
            else fireTimer.Stop();
        }

        // ---- the live layer: the stick dots and the waiting chip ----
        //
        // A stick in the hand never rests (its values jitter by a few counts) and a waiting chip pulses for
        // five seconds. Neither may cost the whole picture: the row draws what stands still (the dials, the
        // chip's fill and words) and names the place; the dots, the pulsing outline and the count-down are
        // drawn here, alone.

        struct DialSpot { public bool On; public Point Left, Right; public double Radius, DeadLeft, DeadRight, Alpha, Shift; public Geometry Clip; }
        struct WaitSpot { public bool On; public Rect Chip; public Point Dial; public double DialRadius, Alpha, Shift; public Geometry Clip; }
        DialSpot dials;
        WaitSpot waiting;
        bool liveShown;
        readonly int[] dotPixels = new int[4];
        Geometry LayerClip()
        {
            if (!layerClipOn) return null;
            var clip = new RectangleGeometry(layerClip);
            clip.Freeze();
            return clip;
        }
        /// <summary>The two stick dials of the Gamepad tab (called while their row is drawn): where the live dots go.</summary>
        public void Dials(Point left, Point right, double radius, double deadLeft, double deadRight)
        {
            dials = new DialSpot { On = true, Left = left, Right = right, Radius = radius, DeadLeft = deadLeft, DeadRight = deadRight, Alpha = layerAlpha, Shift = layerShift, Clip = LayerClip() };
        }
        /// <summary>The chip that waits for a key or a button (called while its row is drawn): its outline pulses and its five seconds drain round `dial`.</summary>
        public void Waiting(Rect chip, Point dial, double dialRadius)
        {
            waiting = new WaitSpot { On = true, Chip = chip, Dial = dial, DialRadius = dialRadius, Alpha = layerAlpha, Shift = layerShift, Clip = LayerClip() };
        }
        /// <summary>Where a stick's dot stands in its dial, and whether it is outside the dead zone.</summary>
        Point StickDot(Point centre, short x, short y, double dead, out bool outside)
        {
            double sx = Pad.Connected ? x / 32768.0 : 0, sy = Pad.Connected ? y / 32768.0 : 0;
            outside = Math.Sqrt(sx * sx + sy * sy) > dead;
            return new Point(centre.X + sx * (dials.Radius - 6), centre.Y - sy * (dials.Radius - 6));
        }
        /// <summary>True when a dot stands on another pixel of the monitor than when it was drawn last.</summary>
        bool DotsMoved(bool remember)
        {
            double k = scale * Tx.PixelsPerDip;
            bool unused;
            var l = StickDot(dials.Left, Pad.LX, Pad.LY, dials.DeadLeft, out unused);
            var r = StickDot(dials.Right, Pad.RX, Pad.RY, dials.DeadRight, out unused);
            int[] now = { (int)Math.Round(l.X * k), (int)Math.Round(l.Y * k), (int)Math.Round(r.X * k), (int)Math.Round(r.Y * k) };
            bool moved = false;
            for (int i = 0; i < 4; i++) { if (now[i] != dotPixels[i]) moved = true; if (remember) dotPixels[i] = now[i]; }
            return moved;
        }
        void DrawLive()
        {
            bool any = dials.On || (waiting.On && Capture != null);
            if (any || liveShown)
            {
                using (var dc = liveVisual.RenderOpen())
                {
                    if (any)
                    {
                        LiveRenders++;
                        dc.PushTransform(viewTransform);
                        if (dials.On)
                        {
                            int pushed = 0;
                            if (dials.Shift != 0) { dc.PushTransform(new TranslateTransform(0, dials.Shift)); pushed++; }
                            if (dials.Clip != null) { dc.PushClip(dials.Clip); pushed++; }
                            if (dials.Alpha < 1) { dc.PushOpacity(dials.Alpha); pushed++; }
                            for (int i = 0; i < 2; i++)
                            {
                                bool outside;
                                var at = i == 0 ? StickDot(dials.Left, Pad.LX, Pad.LY, dials.DeadLeft, out outside) : StickDot(dials.Right, Pad.RX, Pad.RY, dials.DeadRight, out outside);
                                dc.DrawEllipse(Pal.B(Pad.Connected ? (outside ? Pal.Gold : Pal.Cream) : Pal.LockedGrey), Pal.P(Pal.Ink, 2), at, 5, 5);
                            }
                            for (; pushed > 0; pushed--) dc.Pop();
                            DotsMoved(true);
                        }
                        if (waiting.On && Capture != null)
                        {
                            int pushed = 0;
                            if (waiting.Shift != 0) { dc.PushTransform(new TranslateTransform(0, waiting.Shift)); pushed++; }
                            if (waiting.Clip != null) { dc.PushClip(waiting.Clip); pushed++; }
                            if (waiting.Alpha < 1) { dc.PushOpacity(waiting.Alpha); pushed++; }
                            double pulse = Static || !AnimationsOn ? 1 : 0.55 + 0.45 * Math.Sin(Now * 2 * Math.PI / 0.9);
                            Art.Capsule(dc, waiting.Chip, null, Pal.P(Pal.Gold, 2.5, 0.55 + 0.45 * pulse));
                            double left = Math.Max(0, 1 - Capture.Elapsed(this) / CaptureState.Seconds), rad = waiting.DialRadius;
                            if (left > 0.001)
                            {
                                var arc = new StreamGeometry();
                                using (var g = arc.Open())
                                {
                                    double a = Math.Min(left, 0.999) * 2 * Math.PI;
                                    g.BeginFigure(new Point(waiting.Dial.X, waiting.Dial.Y - rad), false, false);
                                    g.ArcTo(new Point(waiting.Dial.X + Math.Sin(a) * rad, waiting.Dial.Y - Math.Cos(a) * rad), new Size(rad, rad), 0, a > Math.PI, SweepDirection.Clockwise, true, false);
                                }
                                arc.Freeze();
                                dc.DrawGeometry(null, Pal.P(Pal.Gold, 2.5), arc);
                            }
                            for (; pushed > 0; pushed--) dc.Pop();
                        }
                    }
                }
                liveShown = any;
            }
            // The waiting chip moves by the clock: about thirty times a second, for the five seconds it waits.
            if (waiting.On && Capture != null && !Hidden && !Static) { if (!liveTimer.IsEnabled) liveTimer.Start(); }
            else liveTimer.Stop();
        }
        /// <summary>How often only the stick dots or the waiting chip were drawn, in their own visual.</summary>
        public int LiveRenders;
        public bool HasDials { get { return dials.On; } }

        struct RingSpot { public Rect Box; public double Radius, Alpha, Shift; public Rect Clip; public bool Clipped; }
        readonly List<RingSpot> rings = new List<RingSpot>();
        /// <summary>The gold ring of the focus. It is drawn in its own layer: after the controller it breathes, and only it is drawn again.</summary>
        public void FocusRing(DrawingContext dc, Rect r, double radius)
        {
            if (inOverlay) { dc.DrawRoundedRectangle(null, Pal.P(Pal.Gold, 2), r, radius, radius); return; }
            rings.Add(new RingSpot { Box = r, Radius = radius, Alpha = layerAlpha, Shift = layerShift, Clip = layerClip, Clipped = layerClipOn });
        }
        void DrawRings()
        {
            bool breathe = rings.Count > 0 && padDrove && Moving && !Hidden;
            using (var dc = ringVisual.RenderOpen())
            {
                if (rings.Count > 0)
                {
                    RingRenders++;
                    double a = breathe ? 0.85 + 0.15 * Math.Sin(Now * 2 * Math.PI / 1.2) : 1;
                    dc.PushTransform(viewTransform);
                    foreach (var ring in rings)
                    {
                        int pushed = 0;
                        if (ring.Shift != 0) { dc.PushTransform(new TranslateTransform(0, ring.Shift)); pushed++; }
                        if (ring.Clipped) { dc.PushClip(new RectangleGeometry(new Rect(ring.Clip.X, ring.Clip.Y - 2, ring.Clip.Width, ring.Clip.Height + 4))); pushed++; }
                        dc.DrawRoundedRectangle(null, Pal.P(Pal.Gold, 2, a * ring.Alpha), ring.Box, ring.Radius, ring.Radius);
                        for (; pushed > 0; pushed--) dc.Pop();
                    }
                }
            }
            if (breathe) { if (!ringTimer.IsEnabled) ringTimer.Start(); }
            else ringTimer.Stop();
        }
        /// <summary>How often only the focus ring was drawn, in its own visual.</summary>
        public int RingRenders;
        /// <summary>True while something in the launcher moves by itself (frames, the fire, the breathing ring).</summary>
        public bool Animating { get { return hooked || fireTimer.IsEnabled || ringTimer.IsEnabled || liveTimer.IsEnabled; } }
        public bool FireRuns { get { return fireTimer.IsEnabled; } }
        public bool RingBreathes { get { return ringTimer.IsEnabled; } }

        // ---- pages and rows --------------------------------------------------------------------

        public int PageIndex { get { return page; } }
        public string PageName { get { return pages[page].Name; } }

        public void ShowPage(int index, bool animate = true)
        {
            index = Math.Max(0, Math.Min(pages.Count - 1, index));
            CloseOverlays();
            if (index != page)
            {
                DropStaleReport(true);
                page = index; scrollTarget = 0; tweens.Remove("scroll"); rowFocus = -1; sub = 0; if (animate) pageShown = Now;
            }
            railFocus = page;
            Rebuild();
        }
        public void ShowPage(string name)
        {
            for (int i = 0; i < pages.Count; i++) if (pages[i].Name == name) { ShowPage(i); return; }
        }

        /// <summary>Builds the rows of the page again, keeping the focus on the row it was on.</summary>
        public void Rebuild()
        {
            string id = rowFocus >= 0 && rowFocus < rows.Count ? rows[rowFocus].Id : null;
            foreach (var key in tweens.Keys.Where(k => k is Tuple<object, int> || k is Control).ToList()) tweens.Remove(key);
            rows = Prune(pages[page].Build());
            double right = pages[page].Right != null ? pages[page].Right() : Lay.RowRight;
            foreach (var r in rows) r.Right = right;
            if (id != null)
            {
                rowFocus = rows.FindIndex(r => r.Id == id);
                if (rowFocus < 0 && zone == Zone.Card) { rowFocus = FirstFocusable(); if (rowFocus < 0) zone = Zone.Rail; }
            }
            LayoutRows();
            InvalidateVisual();
        }

        /// <summary>Leaves out the rows the game beside the launcher has no setting for, and a heading with nothing under it.</summary>
        List<Row> Prune(List<Row> list)
        {
            list.RemoveAll(r => r.NeedsNewer(Model));
            for (int i = list.Count - 1; i >= 0; i--)
                if (list[i] is GroupRow && (i + 1 >= list.Count || list[i + 1] is GroupRow || list[i + 1] is AdvancedRow)) list.RemoveAt(i);
            return list;
        }

        public void MoveFocusWithRow(int delta)
        {
            // A mod moved up or down: the focus goes with it.
            if (zone != Zone.Card || rowFocus < 0 || rowFocus >= rows.Count) return;
            var mod = rows[rowFocus] as ModRow;
            if (mod == null) return;
            pendingFocusId = mod.Id;
        }
        string pendingFocusId;
        bool backFromPad;

        void LayoutRows()
        {
            double y = 0, gap = 0, pinned = 0;
            foreach (var r in rows)
            {
                r.Measure(this);
                // A pinned row (the Controls page's tabs) stands under the title and does not move with the list.
                if (r.Pinned) { r.Top = pinned; pinned += r.Height; continue; }
                r.Top = y;
                gap = r.Compact || r is ModRow ? 0 : 8;
                y += r.Height + gap;
            }
            listTop = Lay.RowsTop + (pinned > 0 ? pinned + 8 : 0);
            contentHeight = y - gap;        // the space under the last row is the card's own
            if (pendingFocusId != null)
            {
                int i = rows.FindIndex(r => r.Id == pendingFocusId);
                if (i >= 0) rowFocus = i;
                pendingFocusId = null;
            }
            // Where the list may rest: with a whole row at its top edge, never part of one. The last
            // rest is the first row top from which the end of the list is in view.
            stops.Clear();
            stops.Add(0);
            double need = contentHeight - (ViewHeight - 8);
            if (need > 0.5)
            {
                foreach (var r in rows)
                {
                    if (r.Pinned || r.Top <= 0 || r.Height < 20) continue;
                    stops.Add(r.Top);
                    if (r.Top >= need - 0.01) break;
                }
                if (stops[stops.Count - 1] < need - 0.01) stops.Add(need);
            }
            maxScroll = stops[stops.Count - 1];
            scrollTarget = Math.Max(0, Math.Min(maxScroll, scrollTarget));
            if (!freeScroll) scrollTarget = NearestStop(scrollTarget);
        }
        readonly List<double> stops = new List<double>();
        double maxScroll;
        /// <summary>True while the thumb is dragged or the right stick scrolls: the list follows, and settles on a row when let go.</summary>
        bool freeScroll;
        bool stickScroll;
        int wheelRest;
        /// <summary>Where the list begins: under the title, or under the rows that are pinned there.</summary>
        double listTop = Lay.RowsTop;
        double ViewHeight { get { return Lay.RowsBottom - listTop; } }
        double MaxScroll { get { return maxScroll; } }
        double RowY(Row r, double scroll) { return r.Pinned ? Lay.RowsTop + 5 + r.Top : listTop + 5 + r.Top - scroll; }
        /// <summary>Tests: the top edge of the list (it lies under the pinned tabs on the Controls page).</summary>
        public double ListTop { get { LayoutRows(); return listTop; } }

        double NearestStop(double value)
        {
            double best = 0, far = double.MaxValue;
            foreach (double stop in stops) { double d = Math.Abs(stop - value); if (d < far) { far = d; best = stop; } }
            return best;
        }
        /// <summary>The next rest position beyond `from` in a direction (1 down the list, -1 up), or the end.</summary>
        double NextStop(double from, int dir)
        {
            if (dir > 0) { foreach (double stop in stops) if (stop > from + 0.5) return stop; return maxScroll; }
            for (int i = stops.Count - 1; i >= 0; i--) if (stops[i] < from - 0.5) return stops[i];
            return 0;
        }
        /// <summary>About `distance` px further in a direction, on a rest position, and at least one row.</summary>
        double StepScroll(double from, int dir, double distance)
        {
            double to = NearestStop(Math.Max(0, Math.Min(maxScroll, from + dir * distance)));
            return (dir > 0 ? to > from + 0.5 : to < from - 0.5) ? to : NextStop(from, dir);
        }
        /// <summary>The thumb or the stick was let go: the list eases onto the nearest whole row.</summary>
        void SettleScroll()
        {
            freeScroll = false; stickScroll = false;
            scrollTarget = NearestStop(scrollTarget);
            InvalidateVisual();
        }

        int FirstFocusable() { for (int i = 0; i < rows.Count; i++) if (CanFocus(i)) return i; return -1; }
        int FirstInList() { for (int i = 0; i < rows.Count; i++) if (CanFocus(i) && !rows[i].Pinned) return i; return -1; }
        int LastFocusable() { for (int i = rows.Count - 1; i >= 0; i--) if (CanFocus(i)) return i; return -1; }
        bool CanFocus(int i) { return i >= 0 && i < rows.Count && rows[i].Focusable; }
        bool Usable(int i) { return CanFocus(i) && rows[i].IsEnabled(Model); }

        void EnsureVisible(int index)
        {
            if (index < 0 || index >= rows.Count) return;
            var r = rows[index];
            if (r.Pinned) return;       // always in view
            // Above the list's top edge: the row goes to the top, and a group heading over it comes along.
            double top = r.Top;
            if (index > 0 && !rows[index - 1].Focusable && !rows[index - 1].Pinned && rows[index - 1].Height >= 20) top = rows[index - 1].Top;
            if (index == FirstInList()) top = 0;
            if (top < scrollTarget) scrollTarget = top;
            // Below its bottom edge: the first rest position that shows the row whole (RowY adds 5, the ring 2).
            double least = r.Top + r.Height + 7 - ViewHeight;
            if (least > scrollTarget + 0.01)
            {
                scrollTarget = maxScroll;
                foreach (double stop in stops) if (stop >= least - 0.01) { scrollTarget = stop; break; }
            }
            scrollTarget = Math.Max(0, Math.Min(MaxScroll, scrollTarget));
        }

        public void ScrollTo(double y) { scrollTarget = Math.Max(0, Math.Min(MaxScroll, y)); tweens.Remove("scroll"); InvalidateVisual(); }
        public void FocusRow(string id, int part = 0)
        {
            LayoutRows();
            int i = rows.FindIndex(r => r.Id == id);
            if (i < 0) return;
            zone = Zone.Card; rowFocus = i; sub = part; focusShown = true;
            EnsureVisible(i);
            tweens.Remove("scroll");
            InvalidateVisual();
        }
        public void FocusBottom(int which) { zone = Zone.Bottom; bottomFocus = which; focusShown = true; InvalidateVisual(); }
        public void FocusRail(int index) { zone = Zone.Rail; railFocus = index; focusShown = true; InvalidateVisual(); }
        /// <summary>Screenshots and tests: as if this device had been used last.</summary>
        public void SetDevice(InputDevice d) { Used(d); }
        public void SetPadDrove(bool on) { Used(on ? InputDevice.Pad : InputDevice.None); }

        // ---- for the window and the screenshot mode --------------------------------------------

        /// <summary>What the game wrote before it stopped with an error; shown on the Play page.</summary>
        public string LastError = "";

        /// <summary>Called by the window's timer: things that end by themselves.</summary>
        public void Tick()
        {
            if (Capture != null && Capture.Elapsed(this) >= CaptureState.Seconds) EndCapture();
            // A line whose time is up fades and goes. Only the line is drawn again meanwhile; the whole picture
            // once at the end, and only when the prompts change with it (a message's button that comes or goes).
            if (message.Length > 0 && Now > messageUntil + 0.6)
            {
                bool hadButton = ShownButton != null;
                message = ""; messageButton = null;
                if (hadButton) InvalidateVisual(); else DrawMessage();
            }
            else if (message.Length > 0 && report.Length == 0 && Now > messageUntil) DrawMessage();      // fading out
            if (report.Length > 0 && Now > reportUntil + 0.6)
            {
                report = "";
                if (ShownButton != null) InvalidateVisual(); else DrawMessage();
            }
            else if (report.Length > 0 && Now > reportUntil) DrawMessage();
            DropStaleReport(false);
        }
        public Row FindRow(string id) { return rows.FirstOrDefault(r => r.Id == id); }
        /// <summary>
        /// Tests: true when the list is where it rests, a whole row sits at its top edge, and every other
        /// row is either whole or left out (a block much taller than a row may fade at the bottom edge).
        /// </summary>
        public bool ListRestsOnWholeRows(out string detail)
        {
            LayoutRows();
            var view = new Rect(Lay.CardX + 2, listTop, Lay.CardW - 4, ViewHeight);
            double scroll = Animate("scroll", scrollTarget, 110);
            detail = "scroll " + scroll.ToString("0.#") + " of " + MaxScroll.ToString("0.#");
            if (Math.Abs(scroll - scrollTarget) > 0.5) { detail += ", still moving to " + scrollTarget.ToString("0.#"); return false; }
            bool first = false;
            for (int i = 0; i < rows.Count; i++)
            {
                if (rows[i].Pinned) continue;
                double y = RowY(rows[i], scroll);
                if (Math.Abs(y - (view.Top + 5)) < 0.5) first = true;
                if (y + rows[i].Height <= view.Top || y >= view.Bottom) continue;
                Brush mask;
                double alpha = RowAlpha(i, scroll, view, out mask);
                bool cut = y < view.Top - 0.5 || y + rows[i].Height + 2 > view.Bottom + 0.5;
                if (cut && alpha > 0.01 && (mask == null || y < view.Top)) { detail += ", " + rows[i].Id + " is cut"; return false; }
            }
            if (!first) detail += ", no row at the top edge";
            return first;
        }
        public double ListScroll { get { return scrollTarget; } }
        /// <summary>Tests: where the arrow under (or above) the list is clicked.</summary>
        public Rect MoreButton(bool down) { return MoreRect(down); }
        public bool ListScrolls { get { return MaxScroll > 0.5; } }
        public bool HasDialog { get { return dialog != null; } }
        public string DialogText { get { return dialog != null ? dialog.Text : ""; } }
        public bool HasDropdown { get { return dropControl != null; } }
        /// <summary>What the message line shows: a row's report while there is one, else the standing message.</summary>
        public string MessageText { get { return report.Length > 0 ? report : message; } }
        /// <summary>The message under a report (or the one shown, when there is no report).</summary>
        public string StandingMessage { get { return message; } }
        public string ReportText { get { return report; } }
        public string MessageButton { get { return ShownButton; } }
        /// <summary>The message's button, while the message is what the line shows.</summary>
        string ShownButton { get { return report.Length == 0 && message.Length > 0 ? messageButton : null; } }
        /// <summary>What a click on the message line's button does.</summary>
        public void PressMessageButton() { if (messageButton != null && messageAction != null) messageAction(); }
        /// <summary>Tests: true when the message line was drawn last with the warning triangle in front of it.</summary>
        public bool MessageHasTriangle;
        /// <summary>Tests: what the prompts along the bottom showed when they were drawn last.</summary>
        public string PromptLog = "";
        /// <summary>Tests: where a row was drawn last (its top), or NaN when it was not.</summary>
        public double RowShownAt(string id) { var r = FindRow(id); return r == null ? double.NaN : r.ShownY; }
        public bool IsRowLocked(string id) { var r = FindRow(id); return r != null && !r.IsEnabled(Model); }
        public Rect RowPadlock(string id) { var r = FindRow(id); return r == null ? Rect.Empty : r.Padlock; }
        public string FocusedRow { get { return zone == Zone.Card && rowFocus >= 0 && rowFocus < rows.Count ? rows[rowFocus].Id : null; } }
        public Rect PlayButton { get { return PlayRect; } }
        public Rect DefaultsButton { get { return DefaultsRect; } }
        /// <summary>Where a part of a row's control was drawn last (logical coordinates), or Rect.Empty.</summary>
        public Rect PartRect(string id, int part)
        {
            var r = FindRow(id);
            if (r == null || r.Control == null || part < 0 || part >= r.Control.Parts.Count) return Rect.Empty;
            return r.Control.Parts[part];
        }
        public void OpenDropdownFor(string id)
        {
            var r = FindRow(id);
            var d = r != null ? r.Control as Dropdown : null;
            if (d != null) OpenDropdown(d);
        }
        public void BeginCaptureFor(string id, int part)
        {
            var r = FindRow(id);
            var b = r != null ? r.Control as Bindings : null;
            if (b == null) return;
            b.Layout(this, Lay.ControlX, 0, 44);
            BeginCapture(b, part);
        }
        public void ScrollToRow(string id)
        {
            LayoutRows();
            var r = FindRow(id);
            if (r != null) ScrollTo(r.Top - 6);
        }

        // ---- messages --------------------------------------------------------------------------

        public void Say(string text, Color color, double seconds = 0, string button = null, Action action = null)
        {
            message = text ?? "";
            messageColor = color;
            messageUntil = seconds > 0 ? Now + seconds : double.MaxValue;
            messageButton = button;
            messageAction = action;
            report = "";
            InvalidateVisual();
        }
        public void SaySaved() { Say("Settings saved.", Pal.ChalkGreen, 3); }
        public void SayError(string text, string button = null, Action action = null) { Say(text, Pal.Problem, 0, button, action); }
        /// <summary>
        /// A problem: one short sentence in red with the warning triangle. What the system said about it, or
        /// where it happened, is behind a Details button and is not written into the line.
        /// </summary>
        public void SayProblem(string sentence, string title, string detail)
        {
            if (string.IsNullOrWhiteSpace(detail)) { SayError(sentence); return; }
            string text = detail.Trim();
            SayError(sentence, "Details", () => Tell(title, text));
        }
        public const string CannotWrite = "Could not write ChiliNative.ini.";
        /// <summary>The settings file could not be written: one line here, what Windows said behind the button.</summary>
        public void SayCannotWrite(string reason)
        {
            SayProblem(CannotWrite, "Could not write", Tx.MiddleEllipsis(Model.IniPath, Fonts.Ui, 16, 430) + "\n\n" + reason.TrimEnd('.') + ".");
        }

        /// <summary>Saves the settings; says so, or why not. True when the file is as wanted.</summary>
        public bool Save(bool quietWhenNothing = true)
        {
            bool had = Model.AnyChanged;
            // The frame rate is a choice of two, not a default: game builds differ in what they do without
            // the line (30 or 60), so the one the row shows is written, and what is shown is what runs.
            Model.Pin(S.FrameLock);
            Model.Config.Save();
            string error = Model.Save();
            if (error != null)
            {
                SayCannotWrite(error);
                return false;
            }
            if (had || !quietWhenNothing) SaySaved();
            return true;
        }

        // ---- overlays --------------------------------------------------------------------------

        public bool IsDropdownOpen(Dropdown d) { return dropControl == d; }
        public void OpenDropdown(Dropdown d)
        {
            if (dropControl == d) { dropControl = null; InvalidateVisual(); return; }
            CloseOverlays();
            dropControl = d;
            dropHover = Math.Max(0, d.Selected());
            InvalidateVisual();
        }
        public void Ask(string title, string text, string yes, string no, Action<int> done, bool danger = false)
        {
            CloseOverlays();
            dialog = new DialogState { Title = title, Text = text, Buttons = new[] { yes, no }, Done = done, Focus = danger ? 1 : 0, Danger = danger };
            InvalidateVisual();
        }
        /// <summary>Something to read, with one button that closes it.</summary>
        public void Tell(string title, string text)
        {
            CloseOverlays();
            dialog = new DialogState { Title = title, Text = text, Buttons = new[] { "Close" }, Focus = 0, Cancel = 0 };
            InvalidateVisual();
        }
        public bool HasOverlay { get { return dialog != null || dropControl != null || Capture != null; } }
        void CloseOverlays()
        {
            dropControl = null;
            if (Capture != null) EndCapture();
        }
        void CloseDialog(int choice)
        {
            var d = dialog;
            dialog = null;
            if (d != null && d.Done != null) d.Done(choice);
            InvalidateVisual();
        }

        // ---- binding capture -------------------------------------------------------------------

        public void BeginCapture(Bindings c, int part)
        {
            if (Capture != null) EndCapture();
            dropControl = null;
            int index = c.PlusPart(part) ? c.ChipCount : part;
            if (index > c.ChipCount) return;
            Capture = new CaptureState { Control = c, Index = index, Start = Now, PadHeld = Pad.Buttons, PadTriggersHeld = Pad.LT > 80 || Pad.RT > 80 };
            for (int i = 0; i < rows.Count; i++) if (rows[i].Control == c) { rowFocus = i; zone = Zone.Card; }
            InvalidateVisual();
        }
        public void EndCapture()
        {
            if (Capture == null) return;
            var c = Capture.Control;
            Capture = null;
            sub = Math.Min(sub, Math.Max(0, c.Chips(Model).Count));
            padWaitRelease = true;
            InvalidateVisual();
        }

        public void RemoveBinding(Bindings c, ChipInfo chip)
        {
            if (chip == null || chip.Fixed) return;
            if (chip.Pad) Model.Set(S.Pad(chip.Name), "None");
            else if (chip.Mouse) Model.Set(S.Mouse(chip.Name), "None");
            else Model.SetKeys(c.Action.Target, Model.KeysOf(c.Action.Target).Where(k => !KeyNames.Same(k, chip.Name)));
            sub = Math.Max(0, sub - 1);
            InvalidateVisual();
        }

        /// <summary>The capture got a key, a mouse input or a pad button.</summary>
        void Captured(string name, bool mouse, bool pad)
        {
            var cap = Capture;
            if (cap == null) return;
            var c = cap.Control;
            var action = c.Action;
            var old = c.ChipAt(cap.Index);
            EndCapture();
            string scheme = Model.Scheme;
            var chips = c.Chips(Model);
            if (chips.Any(ch => ch.Mouse == mouse && ch.Pad == pad && KeyNames.Same(ch.Name, name))) return;
            ActionDef owner = null;
            if (pad || mouse)
            {
                string t = pad ? Model.PadTarget(name) : Model.MouseTarget(name);
                if (!string.Equals(t, "None", StringComparison.OrdinalIgnoreCase) && !string.Equals(t, action.Target, StringComparison.OrdinalIgnoreCase)) owner = Actions.Find(t);
            }
            else owner = Model.KeyOwner(name, action.Target);
            Action assign = () =>
            {
                if (owner != null && !pad && !mouse)
                    Model.SetKeys(owner.Target, Model.KeysOf(owner.Target).Where(k => !KeyNames.Same(k, name)));
                if (pad)
                {
                    if (old != null) Model.Set(S.Pad(old.Name), "None");
                    Model.Set(S.Pad(name), action.Target);
                    return;
                }
                var keys = Model.KeysOf(action.Target);
                int at = keys.Count;
                if (old != null)
                {
                    if (old.Mouse) Model.Set(S.Mouse(old.Name), "None");
                    else { at = keys.FindIndex(k => KeyNames.Same(k, old.Name)); if (at >= 0) keys.RemoveAt(at); else at = keys.Count; }
                }
                if (mouse) { Model.SetKeys(action.Target, keys); Model.Set(S.Mouse(name), action.Target); }
                else
                {
                    if (action.Toggle) keys.Clear();
                    keys.Insert(Math.Min(at, keys.Count), name);
                    Model.SetKeys(action.Target, keys);
                }
            };
            if (owner != null)
                Ask("Already used", TakenQuestion(pad ? KeyNames.PadDisplay(name, Look, PadSaysCreate) : KeyNames.Display(name), owner.Name(scheme), action.Name(scheme)),
                    "Move", "Keep", choice => { if (choice == 0) assign(); });
            else assign();
            InvalidateVisual();
        }

        /// <summary>The question when a key or button already does something else: what it does now, then what is asked, on a line each.</summary>
        public static string TakenQuestion(string input, string does, string wanted) { return input + " is already " + does + ".\nMove it to " + wanted + "?"; }

        // ---- drawing ---------------------------------------------------------------------------

        static Rect OpaqueBounds(BitmapSource bmp)
        {
            try
            {
                var conv = new FormatConvertedBitmap(bmp, PixelFormats.Bgra32, null, 0);
                int w = conv.PixelWidth, h = conv.PixelHeight;
                var px = new byte[w * h * 4];
                conv.CopyPixels(px, w * 4, 0);
                int x0 = w, y0 = h, x1 = -1, y1 = -1;
                for (int y = 0; y < h; y++)
                    for (int x = 0; x < w; x++)
                        if (px[(y * w + x) * 4 + 3] > 24) { if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; }
                if (x1 < x0) return new Rect(0, 0, w, h);
                return new Rect(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
            }
            catch { return new Rect(0, 0, bmp.PixelWidth, bmp.PixelHeight); }
        }

        void EnsureBackdrop(double w, double h)
        {
            double dpi = Tx.PixelsPerDip;
            double f = Math.Min(1.0, 1600.0 / Math.Max(1, w * dpi));
            int bw = Math.Max(16, (int)Math.Round(w * dpi * f)), bh = Math.Max(16, (int)Math.Round(h * dpi * f));
            if (backdrop != null && Math.Abs(bw - backdropW) < 3 && Math.Abs(bh - backdropH) < 3) return;
            // While a window is dragged to a new size the old picture is stretched; it is drawn again when the size settles.
            if (backdrop != null && Mouse.LeftButton == MouseButtonState.Pressed && !Static && !IsMouseCaptured) { resizePending = true; RequestFrame(); return; }
            backdrop = Backdrop.Render(bw, bh, ArtFiles != null ? ArtFiles.Background : null);
            backdropW = bw; backdropH = bh;
            resizePending = false;
        }
        bool resizePending;

        protected override void OnDpiChanged(DpiScale oldDpi, DpiScale newDpi)
        {
            base.OnDpiChanged(oldDpi, newDpi);
            InvalidateVisual();
        }

        /// <summary>How often the whole picture was drawn (tests and --measure).</summary>
        public int Renders;
        /// <summary>How often only the fire was drawn, in its own visual.</summary>
        public int PartRenders;

        protected override void OnRender(DrawingContext dc)
        {
            double w = ActualWidth, h = ActualHeight;
            if (w < 1 || h < 1) return;
            Renders++;
            if (!Static)
            {
                try { if (PresentationSource.FromVisual(this) != null) Tx.PixelsPerDip = VisualTreeHelper.GetDpi(this).PixelsPerDip; }
                catch { }
            }
            scale = Math.Min(w / 1280.0, h / 720.0);
            ox = (w - 1280 * scale) / 2; oy = (h - 720 * scale) / 2;
            EnsureBackdrop(w, h);
            if (resizePending) RequestFrame();
            dc.DrawRectangle(Pal.B(Pal.RustDark), null, new Rect(0, 0, w, h));
            dc.DrawImage(backdrop, new Rect(0, 0, w, h));
            // The backdrop is all this element draws itself.
            if (viewTransform == null || viewTransform.Matrix.M11 != scale || viewTransform.Matrix.OffsetX != ox || viewTransform.Matrix.OffsetY != oy)
            {
                viewTransform = new MatrixTransform(scale, 0, 0, scale, ox, oy);
                viewTransform.Freeze();
            }
            fire.On = false;
            dials.On = false;
            waiting.On = false;
            rings.Clear();
            layerAlpha = 1; layerShift = 0; layerClipOn = false;
            LayoutRows();
            // Each part of the picture in its own visual (see "layers" above).
            using (var d = topVisual.RenderOpen()) { d.PushTransform(viewTransform); DrawTitle(d); DrawStatusChip(d); d.Pop(); }
            using (var d = railVisual.RenderOpen()) { d.PushTransform(viewTransform); DrawRail(d); d.Pop(); }
            using (var d = cardVisual.RenderOpen())
            using (var head = headVisual.RenderOpen())
            {
                d.PushTransform(viewTransform);
                head.PushTransform(viewTransform);
                DrawCard(d, head);
                head.Pop();
                d.Pop();
            }
            using (var d = bottomVisual.RenderOpen()) { d.PushTransform(viewTransform); DrawBottom(d); d.Pop(); }
            DrawMessage();
            DrawFire();
            DrawLive();
            DrawRings();
            bool overlay = dropControl != null || dialog != null;
            if (overlay || overlayShown)
            {
                using (var over = overlayVisual.RenderOpen())
                {
                    if (overlay)
                    {
                        over.PushTransform(viewTransform);
                        inOverlay = true;
                        if (dropControl != null) DrawDropdown(over);
                        if (dialog != null) { DrawDialog(over); DrawPrompts(over); }
                        inOverlay = false;
                    }
                }
                overlayShown = overlay;
            }
        }

        void DrawTitle(DrawingContext dc)
        {
            if (ArtFiles != null && ArtFiles.Logo != null)
            {
                // The game's own logo, from the player's disc: its opaque part, fitted where the drawn title is.
                var logo = ArtFiles.Logo;
                double k = Math.Min(264.0 / logoCrop.Width, 132.0 / logoCrop.Height);
                if (logoPart == null)
                {
                    logoPart = new CroppedBitmap(logo, new Int32Rect((int)logoCrop.X, (int)logoCrop.Y, (int)logoCrop.Width, (int)logoCrop.Height));
                    logoPart.Freeze();
                }
                dc.DrawImage(logoPart, new Rect(36, 8, logoCrop.Width * k, logoCrop.Height * k));
                double lx = 36 + logoCrop.Width * k + 4;
                Tx.Outlined(dc, "PC", Fonts.Heading, 22, lx, 96, Pal.B(Pal.Chili), 3, 2, 2, 0.5);
                return;
            }
            Art.CrossedChilies(dc, 43, 10, 70, true);
            double size = 40 / 0.716;
            var probe = Tx.Shape("CHILI CON", Fonts.Display, size);
            double topGap = probe.Bounds.Top;
            Tx.Logo(dc, "CHILI CON", size, 40 - probe.Bounds.Left, 30 - topGap);
            var second = Tx.Shape("CARNAGE", Fonts.Display, size);
            Tx.Logo(dc, "CARNAGE", size, 40 - second.Bounds.Left, 76 - topGap);
            Tx.Outlined(dc, "PC", Fonts.Heading, 22, 40 + second.Bounds.Width + 12, 70, Pal.B(Pal.Chili), 3, 2, 2, 0.5);
        }

        BitmapSource logoPart, heroPart;     // made once: the logo's opaque part, the Play picture's left 480 columns

        Rect chipRect;
        void DrawStatusChip(DrawingContext dc)
        {
            bool ok = Model.Disc.Ok;
            string text = ok ? "Game data: found" : "Game data: not found";
            double tw = Tx.Width(text, Fonts.Semi, 14);
            double w = tw + 50;
            chipRect = new Rect(1240 - w, 38, w, 30);
            bool hov = hover.Kind == HitKind.Chip;
            Art.Capsule(dc, chipRect, Pal.B(Pal.Soot, hov ? 0.9 : 0.72), Pal.P(ok ? Pal.ChalkGreen : Pal.Chili, 1.5, ok ? 0.55 : 0.95));
            if (ok) Art.Check(dc, chipRect.X + 13, chipRect.Y + 8, 14, Pal.ChalkGreen);
            else Art.Warning(dc, chipRect.X + 12, chipRect.Y + 7, 15, Pal.Flame);
            Tx.Draw(dc, text, Fonts.Semi, 14, Pal.B(ok ? Pal.ChalkGreen : Pal.Hex("#FF7A6A")), chipRect.X + 36, chipRect.Y + 5);
        }

        Rect RailRect(int i) { return new Rect(40, 162 + 62 * i, 262, 52); }
        void DrawRail(DrawingContext dc)
        {
            bool railFocusShown = FocusShown && zone == Zone.Rail && !HasOverlay;
            int pointer = railFocusShown ? railFocus : page;
            for (int i = 0; i < pages.Count; i++)
            {
                double y = 168 + 62 * i;
                bool selected = i == page;
                bool hov = hover.Kind == HitKind.Rail && hover.Index == i;
                Color c = selected ? Pal.Gold : hov ? Pal.PaleYellow : Pal.HotOrange;
                double size = selected ? 30 : 26;
                Tx.Heading(dc, pages[i].Name, size, 56, y + (selected ? -3 : 0), c);
                // One ring for every entry: round the pointer, the name and the place of the chalk underline.
                if (railFocusShown && railFocus == i) FocusRing(dc, new Rect(20, y - 8, 284, 60), 10);
            }
            double py = Animate("pointer", 168 + 62 * pointer, 120);
            Art.Pointer(dc, 28, py + 11, 17, Pal.Gold);
            double uy = Animate("underline", 168 + 62 * page + 40, 150);
            dc.DrawRoundedRectangle(Pal.B(Pal.Ink, 0.5), null, new Rect(45, uy + 2, 170, 6), 3, 3);
            dc.DrawRoundedRectangle(Pal.B(Pal.Cream, 0.85), null, new Rect(44, uy, 170, 6), 3, 3);
        }

        /// <summary>The card and its rows into `dc`, the page's name into `head` (a visual of its own).</summary>
        void DrawCard(DrawingContext dc, DrawingContext head)
        {
            var pg = pages[page];
            dc.DrawGeometry(Pal.B(Pal.Soot, 0.78), null, cardShape);
            double p = Static || !AnimationsOn ? 1 : Math.Min(1, (Now - pageShown) / 0.12);
            if (p < 1) RequestFrame();
            dc.PushOpacity(p);
            dc.PushTransform(new TranslateTransform(0, (1 - p) * 12));
            if (pg.Name == "Play") DrawPlayPicture(dc);
            head.PushOpacity(p);
            head.PushTransform(new TranslateTransform(0, (1 - p) * 12));
            Tx.Heading(head, pg.Title(), 34, 360, 176, Pal.Gold);
            head.Pop();
            head.Pop();

            double scroll = Animate("scroll", scrollTarget, 110);
            var view = new Rect(Lay.CardX + 2, listTop, Lay.CardW - 4, ViewHeight);
            layerAlpha = p; layerShift = (1 - p) * 12;
            Action<int, double, double, Brush> draw = (i, y, alpha, mask) =>
            {
                var r = rows[i];
                r.Layout(this, y);
                var st = new RowState { Hover = -1, Pressed = -1, Sub = sub };
                // (The ring of a row whose list is open would run under the list: the list's own line has the ring then.)
                st.Focused = FocusShown && zone == Zone.Card && rowFocus == i && dialog == null && (dropControl == null || dropControl != r.Control);
                if (hover.Kind == HitKind.Row && hover.Index == i) st.Hover = hover.Part;
                if (pressed.Kind == HitKind.Row && pressed.Index == i && (hover.Same(pressed) || dragging)) st.Pressed = pressed.Part;
                if (Capture != null && Capture.Control != r.Control) { st.Hover = -1; st.Pressed = -1; }
                if (mask != null) dc.PushOpacityMask(mask); else if (alpha < 1) dc.PushOpacity(alpha);
                layerAlpha = p * Math.Min(1, alpha); layerClip = view; layerClipOn = !r.Pinned; layerRowY = y;
                r.Draw(dc, this, y, st);
                layerAlpha = p; layerClipOn = false;
                if (mask != null || alpha < 1) dc.Pop();
            };
            dc.PushClip(new RectangleGeometry(view));
            for (int i = 0; i < rows.Count; i++)
            {
                var r = rows[i];
                if (r.Pinned) continue;
                double y = RowY(r, scroll);
                Brush mask = null;
                double alpha = y + r.Height < view.Top - 4 || y > view.Bottom + 4 ? 0 : RowAlpha(i, scroll, view, out mask);
                r.ShownY = alpha <= 0.01 ? double.NaN : y;
                if (alpha <= 0.01) { if (r.Control != null) r.Control.Parts.Clear(); continue; }
                draw(i, y, alpha, mask);
            }
            dc.Pop();
            // What is pinned under the title stays where it is while the list moves under it.
            for (int i = 0; i < rows.Count; i++)
                if (rows[i].Pinned) { rows[i].ShownY = RowY(rows[i], 0); draw(i, rows[i].ShownY, 1, null); }
            if (MaxScroll > 0.5)
            {
                // The list goes on above or below: one thing at the card's right edge says so, the scrollbar
                // with the menu's own arrow at each end of it. An arrow is there while there is more that way;
                // a click on it moves the list by a screen.
                double up = Math.Max(0, Math.Min(1, scroll / 20)), down = Math.Max(0, Math.Min(1, (MaxScroll - scroll) / 20));
                bool onArrow = hover.Kind == HitKind.More;
                if (up > 0.01) Art.More(dc, ScrollX, listTop + 9, false, up, onArrow && hover.Index == 0);
                if (down > 0.01) Art.More(dc, ScrollX, Lay.RowsBottom - 9, true, down, onArrow && hover.Index == 1);
                double track = ScrollTrack;
                double thumb = Math.Max(36, track * (ViewHeight / (contentHeight + 8)));
                double ty = listTop + ScrollInset + (track - thumb) * (scroll / MaxScroll);
                bool hot = hover.Kind == HitKind.Scroll || (dragging && pressed.Kind == HitKind.Scroll);
                dc.DrawRoundedRectangle(Pal.B(Pal.Cream, hot ? 0.75 : 0.4), null, new Rect(ScrollX - (hot ? 3 : 2), ty, hot ? 6 : 4, thumb), 2, 2);
            }
            dc.Pop();
            dc.Pop();
            layerAlpha = 1; layerShift = 0;
        }

        // The scrollbar's column, in the gutter between the rows and the card's right edge: an arrow at each
        // end (18 px of the column each) and the thumb's track between them.
        const double ScrollX = 1231, ScrollInset = 20;
        double ScrollTrack { get { return ViewHeight - 2 * ScrollInset; } }

        /// <summary>Where the arrow at the top of the scrollbar (or the one at its foot) can be clicked.</summary>
        Rect MoreRect(bool down)
        {
            return down ? new Rect(ScrollX - 12, Lay.RowsBottom - 20, 24, 22) : new Rect(ScrollX - 12, listTop - 2, 24, 22);
        }

        /// <summary>
        /// How much of a row is shown. At rest the list holds whole rows only: its first row sits at the top
        /// edge (see LayoutRows), and a row the bottom edge would cut through is left out, as is a heading
        /// whose first row is. While the list moves, a row that will be whole stays solid and one that will
        /// not fades out before the list comes to rest. While the thumb or the stick moves the list freely,
        /// a row fades over the first 10 px it loses at either edge. A block much taller than a row (the
        /// Play page's summary, "What the game said") fades at the edge itself instead: `mask`.
        /// </summary>
        double RowAlpha(int index, double scroll, Rect view, out Brush mask)
        {
            mask = null;
            var r = rows[index];
            // A heading counts down to the foot of the row it stands over.
            double depth = r.Height;
            if (r is GroupRow && index + 1 < rows.Count) depth = rows[index + 1].Top + rows[index + 1].Height - r.Top;
            double y = RowY(r, scroll), rest = RowY(r, scrollTarget);
            bool whole = !freeScroll && rest >= view.Top - 0.5 && rest + depth + 2 <= view.Bottom + 0.5;
            if (whole) return 1;
            double lostTop = view.Top - y, lostBottom = y + depth + 2 - view.Bottom;
            if (r.Height > 72)
            {
                if (lostTop > 0 && y + r.Height > view.Top + 4) { mask = EdgeMask(view, true); return 1; }
                if (lostBottom > 0 && y < view.Bottom - 4) { mask = EdgeMask(view, false); return 1; }
            }
            double edge = Math.Max(0, 1 - Math.Max(0, Math.Max(lostTop, lostBottom)) / 10);
            return freeScroll ? edge : Math.Min(edge, Math.Min(1, Math.Abs(scroll - scrollTarget) / 24));
        }

        static Brush topMask, bottomMask;
        /// <summary>For a tall block at an edge of the list: nothing at the edge itself, whole 30 px further in.</summary>
        static Brush EdgeMask(Rect view, bool top)
        {
            if (topMask == null)
            {
                var t = new LinearGradientBrush { MappingMode = BrushMappingMode.Absolute, StartPoint = new Point(0, view.Top), EndPoint = new Point(0, view.Top + 30) };
                t.GradientStops.Add(new GradientStop(Color.FromArgb(0, 0, 0, 0), 0));
                t.GradientStops.Add(new GradientStop(Color.FromArgb(0, 0, 0, 0), 0.2));
                t.GradientStops.Add(new GradientStop(Colors.Black, 1));
                t.Freeze();
                topMask = t;
                var b = new LinearGradientBrush { MappingMode = BrushMappingMode.Absolute, StartPoint = new Point(0, view.Bottom - 30), EndPoint = new Point(0, view.Bottom) };
                b.GradientStops.Add(new GradientStop(Colors.Black, 0));
                b.GradientStops.Add(new GradientStop(Color.FromArgb(0, 0, 0, 0), 0.8));
                b.GradientStops.Add(new GradientStop(Color.FromArgb(0, 0, 0, 0), 1));
                b.Freeze();
                bottomMask = b;
            }
            return top ? topMask : bottomMask;
        }

        Rect DefaultsRect { get { return new Rect(860, 636, 140, 46); } }
        Rect PlayRect { get { return new Rect(1020, 626, 220, 66); } }
        public bool CanPlay { get { return Model.Disc.Ok && Model.Game.Exists; } }
        bool HasDefaults { get { return pages[page].Defaults != null; } }

        void DrawBottom(DrawingContext dc)
        {
            dc.DrawRectangle(Pal.B(Pal.Cream, 0.55), null, new Rect(40, 617.5, 1200, 1.5));
            // Status line.
            string disc = Model.Disc.Ok ? Tx.MiddleEllipsis(Model.Disc.Path, Fonts.Ui, 14, 430) : "not chosen";
            string saves = Model.SavesDir.StartsWith(Model.GameDir, StringComparison.OrdinalIgnoreCase) ? "beside the program" : Tx.MiddleEllipsis(Model.SavesDir, Fonts.Ui, 14, 220);
            double x = 40;
            x += Tx.Draw(dc, "Disc: ", Fonts.Ui, 14, Pal.B(Pal.Cream, 0.7), x, 634);
            x += Tx.Draw(dc, disc, Fonts.Ui, 14, Pal.B(Pal.Cream), x, 634);
            x += Tx.Draw(dc, "   •   Saves: ", Fonts.Ui, 14, Pal.B(Pal.Cream, 0.7), x, 634);
            Tx.Draw(dc, saves, Fonts.Ui, 14, Pal.B(Pal.Cream), x, 634);
            // (The message line is drawn in a visual of its own, DrawMessage: a line that fades draws only itself.)
            // Defaults.
            bool bottomFocusShown = FocusShown && zone == Zone.Bottom && !HasOverlay;
            var d = DefaultsRect;
            bool dh = HasDefaults && hover.Kind == HitKind.Defaults, dp = dh && pressed.Kind == HitKind.Defaults;
            Buttons.DrawButton(dc, d, "Defaults", dh, dp, false, HasDefaults, this, "defaults");
            if (bottomFocusShown && bottomFocus == 0) FocusRing(dc, new Rect(d.X - 5, d.Y - 5, d.Width + 10, d.Height + 10), d.Height / 2 + 5);
            // PLAY.
            var pr = PlayRect;
            bool can = CanPlay;
            bool ph = can && hover.Kind == HitKind.Play, pp = ph && pressed.Kind == HitKind.Play;
            double grow = Animate("playshadow", ph && !pp ? 2 : 0, 80);
            dc.DrawRoundedRectangle(Pal.B(Pal.Ink, 0.6), null, new Rect(pr.X + 4 + grow, pr.Y + 5 + grow, pr.Width, pr.Height), 10, 10);
            if (pp) pr.Offset(1, 1);
            Color face = !can ? Pal.RustDark : pp ? Pal.RustDark : ph ? Pal.Lighten(Pal.Ember, 0.12) : Pal.Ember;
            dc.DrawRoundedRectangle(Pal.B(face), Pal.P(Pal.Ink, 4), pr, 10, 10);
            dc.DrawRoundedRectangle(null, Pal.P(can ? Pal.Gold : Pal.LockedGrey, 2, can ? 1 : 0.6), new Rect(pr.X + 5, pr.Y + 5, pr.Width - 10, pr.Height - 10), 6, 6);
            var shape = Tx.Shape("PLAY", Fonts.Display, 38 / 0.716 * 0.72);
            double sx = pr.X + (pr.Width - shape.Bounds.Width) / 2 - shape.Bounds.Left, sy = pr.Y + (pr.Height - shape.Bounds.Height) / 2 - shape.Bounds.Top - 1;
            if (can) Tx.Logo(dc, "PLAY", 38 / 0.716 * 0.72, sx, sy);
            else Tx.Outlined(dc, "PLAY", Fonts.Display, 38 / 0.716 * 0.72, sx, sy, Pal.B(Pal.LockedGrey), 3.5, 0, 0, 0);
            if (bottomFocusShown && bottomFocus == 1) FocusRing(dc, new Rect(pr.X - 6, pr.Y - 6, pr.Width + 12, pr.Height + 12), 14);
            // While a question is open the prompts are its own: they are drawn over its veil (OnRender).
            if (dialog == null) DrawPrompts(dc);
        }

        /// <summary>
        /// The message line, in a visual of its own: a locked row's report while there is one, else the
        /// standing message. A line whose time is up fades over half a second, and only this is drawn again
        /// meanwhile (Tick), not the whole picture.
        /// </summary>
        void DrawMessage()
        {
            if (viewTransform == null) return;
            bool any = report.Length > 0 || message.Length > 0;
            if (!any && !messageShown) return;
            using (var dc = messageVisual.RenderOpen())
            {
                if (!any) { messageShown = false; return; }
                MessageRenders++;
                dc.PushTransform(viewTransform);
                if (report.Length > 0)
                {
                    double a = Now > reportUntil ? Math.Max(0, 1 - (Now - reportUntil) / 0.5) : 1;
                    dc.DrawText(Tx.Make(report, Fonts.Semi, 14, Pal.B(Pal.PaleYellow, a), 800, 1), new Point(40, 658));
                }
                else
                {
                    double a = Now > messageUntil ? Math.Max(0, 1 - (Now - messageUntil) / 0.5) : 1;
                    // One line, cut with an ellipsis when it is longer: the prompts sit right under it. A problem
                    // (the red ones) begins with the warning triangle, as every problem in the launcher does.
                    bool problem = messageColor == Pal.Problem;
                    double mx = problem ? 62 : 40;
                    MessageHasTriangle = problem;
                    if (problem)
                    {
                        if (a < 1) dc.PushOpacity(a);
                        Art.Warning(dc, 40, 660, 15);
                        if (a < 1) dc.Pop();
                    }
                    var mt = Tx.Make(message, Fonts.Semi, 14, Pal.B(messageColor, a), (messageButton != null ? 640 : 800) - (mx - 40), 1);
                    dc.DrawText(mt, new Point(mx, 658));
                    double mw = mt.WidthIncludingTrailingWhitespace;
                    if (messageButton != null)
                    {
                        mw = Math.Min(mw, 640 - (mx - 40));
                        double bw = Tx.Width(messageButton, Fonts.Semi, 14) + 26;
                        messageButtonRect = new Rect(mx + mw + 14, 654, bw, 28);
                        bool hov = hover.Kind == HitKind.MessageButton;
                        Art.Capsule(dc, messageButtonRect, Pal.B(Pal.Ink, hov ? 0.6 : 0.45), Pal.P(Pal.Cream, 1.5, hov ? 0.9 : 0.5));
                        Tx.DrawCentered(dc, messageButton, Fonts.Semi, 14, Pal.B(hov ? Pal.PaleYellow : Pal.Cream), messageButtonRect, -1);
                    }
                }
                messageShown = true;
            }
        }
        bool messageShown;
        /// <summary>How often only the message line was drawn, in its own visual.</summary>
        public int MessageRenders;

        /// <summary>
        /// The look of the controller's buttons in the launcher's own pictures: what "Gamepad buttons"
        /// ([Input.Gamepad] Glyphs) says, and with Automatic the family of the controller in use (Xbox when
        /// there is none).
        /// </summary>
        public PadKind Look
        {
            get
            {
                if (Has(S.PadGlyphs))
                {
                    string g = Model.Get(S.PadGlyphs).Trim().ToLowerInvariant();
                    if (g == "xbox") return PadKind.Xbox;
                    if (g == "playstation") return PadKind.PlayStation;
                }
                return Pad.Connected ? Pad.Kind : PadKind.Xbox;
            }
        }
        /// <summary>True when the connected controller is a DualSense: its Share button is called Create.</summary>
        public bool PadSaysCreate { get { return Pad.Connected && Pad.Kind == PadKind.PlayStation && (Pad.Pid == 0x0CE6 || Pad.Pid == 0x0DF2); } }

        /// <summary>A dimmed part of a row that is not locked was pressed: say why it cannot be used.</summary>
        public void SayPartReason(Control c, int part)
        {
            var row = rows.FirstOrDefault(r => r.Control == c);
            string why = c.PartReason(part);
            if (row != null && !string.IsNullOrEmpty(why)) SayReport(row, why, () => c.PartReason(part));
        }

        /// <summary>
        /// What the buttons do here, along the bottom: a picture of the button (or the cap of the key) and a
        /// word or two. They follow the device used last: the controller's pictures after the controller,
        /// key caps after the keyboard, nothing after the mouse.
        /// </summary>
        void DrawPrompts(DrawingContext dc)
        {
            bool byPad = device == InputDevice.Pad || (device == InputDevice.None && Pad.Connected);
            bool byKeys = device == InputDevice.Keyboard;
            var log = new List<string>();
            double gx = 40, gy = 692;
            var look = Look;
            Action<string> word = t => { gx += 2 + Tx.Draw(dc, t, Fonts.Ui, 13, Pal.B(Pal.Cream, 0.85), gx + 2, gy) + 18; };
            Action<string, string> keys = (k, t) =>
            {
                foreach (var cap in k.Split('+')) gx += Art.KeyCap(dc, cap, gx, gy, 18) + 4;
                log.Add("key:" + k + " " + t);
                word(t);
            };
            Action<string, string, string> prompt = (button, k, t) =>
            {
                if (byPad && button != null)
                {
                    foreach (var b in button.Split('+')) gx += Art.PadGlyph(dc, b, gx, gy, 18, 1, look) + 4;
                    log.Add((look == PadKind.PlayStation ? "ps:" : "xbox:") + button + " " + t);
                    word(t);
                }
                else if (byKeys && k != null) keys(k, t);
            };
            if (Capture != null)
            {
                // Waiting for a controller button, every button is an answer. The player holding the controller
                // is shown nothing here (a keyboard key is no use to that player; the row has its own Cancel and
                // the count-down); the player at the keyboard is shown the key that cancels.
                // Waiting for a key, every key is an answer: the controller's B cancels.
                if (Capture.Control.PadView) { if (byKeys) keys("Esc", "Cancel"); }
                else prompt("B", null, "Cancel");
            }
            else if (!byPad && !byKeys) { }
            else if (dialog != null && dialog.Buttons.Length == 1) prompt("A", "Enter", dialog.Buttons[0]);
            else if (dialog != null) { prompt("A", "Enter", "Choose"); prompt("B", "Esc", dialog.Buttons[dialog.Cancel]); }
            else if (dropControl != null) { prompt("A", "Enter", "Choose"); prompt("B", "Esc", "Close"); }
            else
            {
                Row row = zone == Zone.Card && rowFocus >= 0 && rowFocus < rows.Count ? rows[rowFocus] : null;
                if (row != null && !row.IsEnabled(Model)) row = null;       // a locked row: its control does nothing
                // What A (or Enter) does here, or null when it does nothing: then there is no prompt for it.
                string a = null;
                if (zone == Zone.Rail) { if (railFocus != page) a = "Open page"; }
                else if (zone == Zone.Bottom) { if (bottomFocus == 1 ? CanPlay : HasDefaults) a = bottomFocus == 1 ? "Play" : "Defaults"; }
                else if (row != null && row.Control is Slider)
                {
                    prompt("DpadLR", "\u2190+\u2192", "Change");
                    var extra = ((Slider)row.Control).Button;
                    if (extra != null && (extra.EnabledFn == null || extra.EnabledFn())) a = extra.Label;
                }
                else if (row != null) a = FocusVerb(row);
                if (a != null) prompt("A", "Enter", a);
                // B on the rail goes to the Play page: not offered on that page itself.
                if (zone == Zone.Rail) { if (page != 0) prompt("B", null, "Play page"); }
                else prompt("B", "Esc", "Back");
                if (HasDefaults) prompt("Y", null, "Defaults");
                // X opens a path row's folder: left out where A is said to do the same.
                if (row != null && HasOpen(row) && a != "Open") prompt("X", null, "Open");
                var chips = row != null ? row.Control as Bindings : null;
                if (chips != null && sub < chips.ChipCount) prompt("X", "Del", "Remove");
                prompt("LB+RB", "Ctrl+Tab", "Page");
                if (CanPlay && !(zone == Zone.Bottom && bottomFocus == 1)) prompt("Start", null, "Play");
                if (ShownButton != null) prompt("Back", null, ShownButton);
            }
            PromptLog = string.Join(" | ", log);
        }
        /// <summary>What A does on the focused row, in a word or two; null when it does nothing there.</summary>
        string FocusVerb(Row row)
        {
            var c = row.Control;
            var chips = c as Bindings;
            if (chips != null) return sub < chips.ChipCount ? "Change" : "Add";
            if (row is ModRow) return sub == 1 ? "Move up" : sub == 2 ? "Move down" : "Switch";
            var advanced = row as AdvancedRow;
            if (advanced != null) return advanced.IsOpen ? "Close" : "Open";
            if (c is Toggle) return "Switch";
            if (c is Dropdown) return "Choose";
            var choices = c as Segmented;
            if (choices != null) return choices.HasNext ? "Next choice" : null;
            // A button row: the focused button's own name (the path field opens its folder).
            var buttons = c as Buttons;
            if (buttons != null) return buttons.Verb(sub);
            return null;
        }
        static bool HasOpen(Row r)
        {
            var b = r.Control as Buttons;
            return b != null && (b.PathAction != null || b.Items.Any(i => i.IsOpen));
        }

        void DrawDropdown(DrawingContext dc)
        {
            var d = dropControl;
            if (d.Parts.Count == 0) { dropControl = null; return; }
            var anchor = d.Parts[0];
            double h = d.Options.Count * 36 + 8;
            double w = anchor.Width;
            bool ticks = d.Options.Any(o => o.OnFn != null);
            foreach (var o in d.Options) w = Math.Max(w, Tx.Width(o.Label, Fonts.Semi, 15) + (ticks ? 64 : 40));
            double y = anchor.Bottom + 6;
            if (y + h > 608) y = anchor.Top - 6 - h;
            dropRect = new Rect(anchor.X, y, w, h);
            dc.DrawRoundedRectangle(Pal.B(Pal.Ink, 0.5), null, new Rect(dropRect.X + 3, dropRect.Y + 4, w, h), 8, 8);
            dc.DrawRoundedRectangle(Pal.B(Pal.Soot, 0.96), Pal.P(Pal.Cream, 1.5, 0.35), dropRect, 8, 8);
            int sel = d.Selected();
            for (int i = 0; i < d.Options.Count; i++)
            {
                var r = new Rect(dropRect.X + 4, dropRect.Y + 4 + i * 36, w - 8, 36);
                bool on = i == sel, hov = i == dropHover;
                if (on) dc.DrawRoundedRectangle(Pal.B(Pal.Flame), null, r, 6, 6);
                else if (hov) dc.DrawRoundedRectangle(Pal.B(Pal.Ember, 0.6), null, r, 6, 6);
                if (hov && FocusShown) dc.DrawRoundedRectangle(null, Pal.P(Pal.Gold, 2), r, 6, 6);
                Tx.Draw(dc, d.Options[i].Label, Fonts.Semi, 15, Pal.B(on ? Pal.Ink : hov ? Pal.PaleYellow : Pal.Cream), r.X + 12, r.Y + 7);
                // More than one thing of the list can be on (texture packs): each that is on carries a tick.
                if (d.Options[i].OnFn != null && d.Options[i].OnFn()) Art.Check(dc, r.Right - 28, r.Y + 11, 14, on ? Pal.Ink : Pal.ChalkGreen, !on);
            }
        }

        void DrawDialog(DrawingContext dc)
        {
            dc.DrawRectangle(Pal.B(Pal.Ink, 0.6), null, new Rect(-4000, -4000, 9280, 8720));
            var text = Tx.Make(dialog.Text, Fonts.Ui, 16, Pal.B(Pal.Cream), 440);
            double w = 500, h = 64 + text.Height + 86;
            var box = new Rect((1280 - w) / 2, (720 - h) / 2 - 20, w, h);
            dc.DrawRoundedRectangle(Pal.B(Pal.Ink, 0.6), null, new Rect(box.X + 5, box.Y + 7, w, h), 10, 10);
            dc.DrawRoundedRectangle(Pal.B(Pal.Soot, 0.97), Pal.P(Pal.Cream, 1.5, 0.4), box, 10, 10);
            dc.DrawRoundedRectangle(null, Pal.P(Pal.Ember, 2, 0.8), new Rect(box.X + 5, box.Y + 5, w - 10, h - 10), 7, 7);
            Tx.Heading(dc, dialog.Title, 24, box.X + 30, box.Y + 20, Pal.PaleYellow);
            dc.DrawText(text, new Point(box.X + 30, box.Y + 64));
            double bx = box.Right - 30;
            dialog.Rects = new Rect[dialog.Buttons.Length];
            for (int i = dialog.Buttons.Length - 1; i >= 0; i--)
            {
                double bw = Math.Max(96, Tx.Width(dialog.Buttons[i], Fonts.Semi, 15) + 40);
                var r = new Rect(bx - bw, box.Bottom - 62, bw, 40);
                dialog.Rects[i] = r;
                bx -= bw + 12;
                bool hov = hover.Kind == HitKind.DialogButton && hover.Index == i;
                bool pr = hov && pressed.Kind == HitKind.DialogButton && pressed.Index == i;
                var rr = r;
                if (pr) rr.Offset(1, 1);
                if (i == 0)
                {
                    Color face = dialog.Danger ? Pal.Chili : Pal.Flame;
                    Art.Capsule(dc, rr, Pal.B(pr ? Pal.RustDark : hov ? Pal.Lighten(face, 0.12) : face), Pal.P(Pal.Ink, 2));
                    Tx.DrawCentered(dc, dialog.Buttons[i], Fonts.Semi, 15, Pal.B(pr ? Pal.Cream : dialog.Danger ? Pal.White : Pal.Ink), rr, -1);
                }
                else
                {
                    Art.Capsule(dc, rr, Pal.B(pr ? Pal.RustDark : Pal.Ink, hov ? 0.6 : 0.45), Pal.P(Pal.Cream, 2, hov ? 0.8 : 0.5));
                    Tx.DrawCentered(dc, dialog.Buttons[i], Fonts.Semi, 15, Pal.B(hov ? Pal.PaleYellow : Pal.Cream), rr, -1);
                }
                if (FocusShown && dialog.Focus == i) FocusRing(dc, new Rect(r.X - 5, r.Y - 5, r.Width + 10, r.Height + 10), 25);
            }
        }

        // ---- hit testing and the mouse ---------------------------------------------------------

        Point ToLogical(Point p) { return new Point((p.X - ox) / scale, (p.Y - oy) / scale); }

        Hit HitTest(Point p)
        {
            if (dialog != null)
            {
                for (int i = 0; i < dialog.Rects.Length; i++) if (dialog.Rects[i].Contains(p)) return new Hit { Kind = HitKind.DialogButton, Index = i };
                return new Hit { Kind = HitKind.Blocked };
            }
            if (dropControl != null)
            {
                if (dropRect.Contains(p))
                {
                    int i = (int)((p.Y - dropRect.Y - 4) / 36);
                    if (i >= 0 && i < dropControl.Options.Count) return new Hit { Kind = HitKind.DropItem, Index = i };
                    return new Hit { Kind = HitKind.Blocked };
                }
                if (dropControl.Parts.Count > 0 && dropControl.Parts[0].Contains(p))
                    for (int i = 0; i < rows.Count; i++) if (rows[i].Control == dropControl) return new Hit { Kind = HitKind.Row, Index = i, Part = 0 };
                return new Hit { Kind = HitKind.DropOutside };
            }
            if (chipRect.Contains(p)) return new Hit { Kind = HitKind.Chip };
            for (int i = 0; i < pages.Count; i++) if (RailRect(i).Contains(p)) return new Hit { Kind = HitKind.Rail, Index = i };
            if (PlayRect.Contains(p)) return new Hit { Kind = HitKind.Play };
            if (DefaultsRect.Contains(p)) return new Hit { Kind = HitKind.Defaults };
            if (ShownButton != null && messageButtonRect.Contains(p)) return new Hit { Kind = HitKind.MessageButton };
            if (MaxScroll > 0.5 && scrollTarget > 0.5 && MoreRect(false).Contains(p)) return new Hit { Kind = HitKind.More, Index = 0 };
            if (MaxScroll > 0.5 && scrollTarget < MaxScroll - 0.5 && MoreRect(true).Contains(p)) return new Hit { Kind = HitKind.More, Index = 1 };
            if (MaxScroll > 0.5 && p.X >= 1222 && p.X <= 1240 && p.Y >= listTop && p.Y <= Lay.RowsBottom) return new Hit { Kind = HitKind.Scroll };
            if (p.Y >= Lay.RowsTop && p.Y <= Lay.RowsBottom && p.X >= Lay.CardX && p.X <= Lay.CardX + Lay.CardW)
            {
                for (int i = 0; i < rows.Count; i++)
                {
                    var c = rows[i].Control;
                    if (c == null) continue;
                    if (!rows[i].Pinned && p.Y < listTop) continue;      // under the pinned tabs: not to be hit
                    for (int k = 0; k < c.Parts.Count; k++)
                        if (c.Parts[k].Contains(p)) return new Hit { Kind = HitKind.Row, Index = i, Part = k };
                }
                // A locked row answers a click anywhere on it, its name included.
                for (int i = 0; i < rows.Count; i++)
                {
                    var r = rows[i];
                    if (!r.Focusable || double.IsNaN(r.ShownY) || r.IsEnabled(Model)) continue;
                    if (p.Y >= r.ShownY && p.Y <= r.ShownY + r.Height && p.X >= Lay.RowLeft && p.X <= r.Right) return new Hit { Kind = HitKind.RowBody, Index = i };
                }
            }
            return new Hit();
        }

        bool Clickable(Hit h)
        {
            switch (h.Kind)
            {
                case HitKind.Rail: case HitKind.Chip: case HitKind.MessageButton: case HitKind.DropItem: case HitKind.DialogButton: case HitKind.Scroll: case HitKind.More: return true;
                case HitKind.Play: return CanPlay;
                case HitKind.Defaults: return HasDefaults;
                case HitKind.Row:
                    if (h.Index >= rows.Count) return false;
                    var r = rows[h.Index];
                    if (Capture != null && Capture.Control != r.Control) return false;
                    return r.IsEnabled(Model) && r.Control.PartEnabled(h.Part);
            }
            return false;
        }

        /// <summary>
        /// Tests and --measure: the real mouse is not listened to (the person at the PC may move it across
        /// the test's window); the test moves a pointer of its own through PointerMove and its fellows.
        /// </summary>
        public bool IgnoreMouse;
        protected override void OnMouseMove(MouseEventArgs e) { if (!IgnoreMouse) PointerMove(ToLogical(e.GetPosition(this))); }
        protected override void OnMouseDown(MouseButtonEventArgs e) { if (IgnoreMouse) return; Focus(); PointerDown(ToLogical(e.GetPosition(this)), e.ChangedButton); }
        protected override void OnMouseUp(MouseButtonEventArgs e) { if (!IgnoreMouse && e.ChangedButton == MouseButton.Left) PointerUp(ToLogical(e.GetPosition(this))); }
        protected override void OnMouseWheel(MouseWheelEventArgs e) { if (!IgnoreMouse) PointerWheel(ToLogical(e.GetPosition(this)), e.Delta); e.Handled = true; }

        /// <summary>The pointer moved to p (logical 1280x720 coordinates).</summary>
        public void PointerMove(Point p)
        {
            if (dragging && pressed.Kind == HitKind.Row && pressed.Index < rows.Count)
            {
                rows[pressed.Index].Control.OnDrag(this, pressed.Part, p);
                InvalidateVisual();
                return;
            }
            if (dragging && pressed.Kind == HitKind.Scroll) { DragScroll(p); return; }
            var h = HitTest(p);
            if (h.Kind == HitKind.DropItem && dropHover != h.Index) { dropHover = h.Index; InvalidateVisual(); }
            if (!h.Same(hover)) { hover = h; InvalidateVisual(); }
            Cursor = Clickable(h) ? Cursors.Hand : Cursors.Arrow;
        }

        protected override void OnMouseLeave(MouseEventArgs e)
        {
            if (!dragging && !IgnoreMouse) { hover = new Hit(); InvalidateVisual(); }
        }

        void DragScroll(Point p)
        {
            double track = ScrollTrack;
            double thumb = Math.Max(36, track * (ViewHeight / (contentHeight + 8)));
            double t = (p.Y - (listTop + ScrollInset) - thumb / 2) / Math.Max(1, track - thumb);
            scrollTarget = Math.Max(0, Math.Min(MaxScroll, t * MaxScroll));
            tweens.Remove("scroll");
            InvalidateVisual();
        }

        public void PointerDown(Point p, MouseButton button)
        {
            var h = HitTest(p);
            hover = h;
            focusShown = false;
            Used(InputDevice.Mouse);
            if (Capture != null)
            {
                // A click on the waiting chip binds that mouse button; Cancel and Remove are text buttons; anything else cancels.
                var cap = Capture;
                bool own = h.Kind == HitKind.Row && h.Index < rows.Count && rows[h.Index].Control == cap.Control;
                if (own && h.Part == 0 && !cap.Control.PadView && !cap.Control.Action.Toggle)
                {
                    string name = button == MouseButton.Left ? "Mouse1" : button == MouseButton.Right ? "Mouse2" :
                        button == MouseButton.Middle ? "Mouse3" : button == MouseButton.XButton1 ? "Mouse4" : "Mouse5";
                    Captured(name, true, false);
                }
                else if (own && h.Part == 2 && button == MouseButton.Left) { var chip = cap.Control.ChipAt(cap.Index); EndCapture(); RemoveBinding(cap.Control, chip); }
                else if (own && h.Part == 0) { }
                else EndCapture();
                InvalidateVisual();
                return;
            }
            if (button == MouseButton.Right)
            {
                if (h.Kind == HitKind.Row && Clickable(h)) rows[h.Index].Control.OnClick(this, h.Part, p, true);
                InvalidateVisual();
                return;
            }
            if (button != MouseButton.Left) return;
            if (h.Kind == HitKind.DropOutside) { dropControl = null; hover = HitTest(p); InvalidateVisual(); return; }
            pressed = h;
            // Trying to use something that is locked says why, in the message line: a click anywhere on a
            // locked row, or on a dimmed choice of a row that is not locked (then that choice's reason).
            if ((h.Kind == HitKind.Row || h.Kind == HitKind.RowBody) && h.Index < rows.Count)
            {
                var row = rows[h.Index];
                if (!row.IsEnabled(Model)) { zone = Zone.Card; rowFocus = h.Index; sub = 0; SayLocked(h.Index); }
                else if (h.Kind == HitKind.Row && !row.Control.PartEnabled(h.Part))
                {
                    int part = h.Part;
                    string why = row.Control.PartReason(part);
                    if (!string.IsNullOrEmpty(why)) { zone = Zone.Card; rowFocus = h.Index; sub = 0; SayReport(row, why, () => row.Control.PartReason(part)); }
                }
            }
            else if (h.Kind == HitKind.None && p.Y >= Lay.RowsTop && p.Y <= Lay.RowsBottom)
            {
                // Something dimmed that is no button at all (the stick that always moves the player): it says why too.
                for (int i = 0; i < rows.Count; i++)
                {
                    var row = rows[i];
                    if (row.Control == null || double.IsNaN(row.ShownY) || !row.IsEnabled(Model)) continue;
                    string why = row.Control.ReasonAt(p);
                    if (string.IsNullOrEmpty(why)) continue;
                    zone = Zone.Card; rowFocus = i; sub = 0;
                    SayReport(row, why, () => why);
                    break;
                }
            }
            if (h.Kind == HitKind.Row && Clickable(h))
            {
                zone = Zone.Card; rowFocus = h.Index; sub = h.Part;
                if (rows[h.Index].Control is Slider && h.Part == 0)
                {
                    dragging = true;
                    CaptureMouse();
                    rows[h.Index].Control.OnClick(this, h.Part, p, false);
                }
            }
            else if (h.Kind == HitKind.Scroll) { dragging = true; freeScroll = true; CaptureMouse(); DragScroll(p); }
            DropStaleReport(false);
            InvalidateVisual();
        }

        public void PointerUp(Point p)
        {
            bool wasDragging = dragging;
            if (dragging) { dragging = false; ReleaseMouseCapture(); }
            if (wasDragging && pressed.Kind == HitKind.Scroll) SettleScroll();
            var h = HitTest(p);
            var was = pressed;
            pressed = new Hit();
            hover = h;
            if (!wasDragging && h.Same(was) && Clickable(h)) Activate(h, p);
            InvalidateVisual();
        }

        void Activate(Hit h, Point p)
        {
            switch (h.Kind)
            {
                case HitKind.Rail: zone = Zone.Rail; ShowPage(h.Index); break;
                case HitKind.Chip: ShowPage("Game data"); break;
                case HitKind.Play: zone = Zone.Bottom; bottomFocus = 1; if (PlayRequested != null) PlayRequested(); break;
                case HitKind.Defaults: zone = Zone.Bottom; bottomFocus = 0; AskDefaults(); break;
                case HitKind.MessageButton: if (messageAction != null) messageAction(); break;
                case HitKind.More: scrollTarget = StepScroll(scrollTarget, h.Index == 1 ? 1 : -1, ViewHeight - 64); break;
                case HitKind.DropItem:
                    var d = dropControl;
                    dropControl = null;
                    if (d != null && h.Index < d.Options.Count) d.Set(d.Options[h.Index].Value);
                    break;
                case HitKind.DialogButton: CloseDialog(h.Index); break;
                case HitKind.Row: rows[h.Index].Control.OnClick(this, h.Part, p, false); break;
            }
        }

        public void PointerWheel(Point p, int delta)
        {
            if (Capture != null)
            {
                var h = HitTest(p);
                bool own = h.Kind == HitKind.Row && h.Index < rows.Count && rows[h.Index].Control == Capture.Control && h.Part == 0;
                if (own && !Capture.Control.PadView && !Capture.Control.Action.Toggle) Captured(delta > 0 ? "WheelUp" : "WheelDown", true, false);
                return;
            }
            if (dialog != null || dropControl != null || delta == 0) return;
            Used(InputDevice.Mouse);
            // A notch moves the list by a row or two and it comes to rest on a whole row. Small turns of a
            // smooth wheel or a touchpad add up to a notch.
            if (Math.Sign(delta) != Math.Sign(wheelRest)) wheelRest = 0;
            wheelRest += delta;
            int notches = wheelRest / 120;
            if (notches == 0) return;
            wheelRest -= notches * 120;
            if (freeScroll) return;
            for (int i = 0; i < Math.Abs(notches); i++) scrollTarget = StepScroll(scrollTarget, notches > 0 ? -1 : 1, 80);
            InvalidateVisual();
        }

        void AskDefaults()
        {
            var pg = pages[page];
            if (pg.Defaults == null) return;
            Ask("Defaults", pg.DefaultsQuestion ?? ("Reset " + pg.Name + " to the defaults?"), "Reset", "Cancel", choice =>
            {
                if (choice != 0) return;
                pg.Defaults();
                Rebuild();
            }, true);
        }

        // ---- keyboard and gamepad --------------------------------------------------------------

        /// <summary>A key went down. Returns true when the launcher used it.</summary>
        public bool HandleKey(Key key, ModifierKeys mods, bool repeat)
        {
            if (!fromPad) Used(InputDevice.Keyboard);
            if (Capture != null)
            {
                if (repeat) return true;
                if (Capture.Control.PadView)
                {
                    if (key == Key.Escape) { EndCapture(); return true; }
                    return key != Key.F4 && key != Key.LeftAlt && key != Key.RightAlt && key != Key.System;
                }
                if (key == Key.F11) { Capture.Note = "F11 cannot be used"; Capture.Start = Now; InvalidateVisual(); return true; }
                string name = KeyMap.Name(key);
                if (name == null)
                {
                    if (key == Key.LWin || key == Key.RWin || key == Key.F4) return false;
                    Capture.Note = "That key cannot be used";
                    Capture.Start = Now;
                    InvalidateVisual();
                    return true;
                }
                Captured(name, false, false);
                return true;
            }
            bool ctrl = (mods & ModifierKeys.Control) != 0, shift = (mods & ModifierKeys.Shift) != 0;
            if ((mods & ModifierKeys.Alt) != 0) return false;
            if (dialog != null)
            {
                switch (key)
                {
                    case Key.Left: case Key.Right: case Key.Tab: case Key.Up: case Key.Down:
                        dialog.Focus = (dialog.Focus + 1) % dialog.Buttons.Length; focusShown = true; break;
                    case Key.Enter: case Key.Space: CloseDialog(dialog.Focus); break;
                    case Key.Escape: CloseDialog(dialog.Cancel); break;
                    default: return true;
                }
                InvalidateVisual();
                return true;
            }
            if (dropControl != null)
            {
                int n = dropControl.Options.Count;
                switch (key)
                {
                    case Key.Up: dropHover = Math.Max(0, dropHover - 1); focusShown = true; break;
                    case Key.Down: dropHover = Math.Min(n - 1, dropHover + 1); focusShown = true; break;
                    case Key.Enter: case Key.Space:
                        var d = dropControl; dropControl = null;
                        if (dropHover >= 0 && dropHover < n) d.Set(d.Options[dropHover].Value);
                        break;
                    case Key.Escape: case Key.Tab: dropControl = null; break;
                    default: return true;
                }
                InvalidateVisual();
                return true;
            }
            if (ctrl && key == Key.Tab) { ShowPage((page + (shift ? pages.Count - 1 : 1)) % pages.Count); return true; }
            if (ctrl && key == Key.S) { Save(false); return true; }
            if (ctrl) return false;
            switch (key)
            {
                case Key.F5: if (RescanRequested != null) RescanRequested(); return true;
                case Key.Up: DoNav(Nav.Up, shift); return true;
                case Key.Down: DoNav(Nav.Down, shift); return true;
                case Key.Left: DoNav(Nav.Left, shift); return true;
                case Key.Right: DoNav(Nav.Right, shift); return true;
                case Key.Enter: DoNav(Nav.Activate, shift); return true;
                case Key.Space: DoNav(Nav.Space, shift); return true;
                case Key.Escape: DoNav(Nav.Back, shift); return true;
                case Key.Tab: DoNav(shift ? Nav.ShiftTab : Nav.Tab, false); return true;
                case Key.Delete: case Key.Back: DoNav(Nav.Delete, shift); return true;
                case Key.PageDown: scrollTarget = StepScroll(scrollTarget, 1, ViewHeight - 64); InvalidateVisual(); return true;
                case Key.PageUp: scrollTarget = StepScroll(scrollTarget, -1, ViewHeight - 64); InvalidateVisual(); return true;
            }
            return false;
        }

        void GoCard(int index, int part = 0)
        {
            zone = Zone.Card; rowFocus = index; sub = part;
            EnsureVisible(index);
        }
        /// <summary>
        /// Why a locked row cannot be used: said in the message line when the player tries to use it (a click
        /// anywhere on it, or A / Enter with the focus on it). Moving the focus says nothing.
        /// </summary>
        void SayLocked(int index)
        {
            if (index < 0 || index >= rows.Count) return;
            var row = rows[index];
            string why = row.LockedReason(Model);
            if (!string.IsNullOrEmpty(why)) SayReport(row, why, () => row.LockedReason(Model));
        }
        /// <summary>Lays a report over the message line. `now` gives the report as it is at any later moment (null: no longer true).</summary>
        void SayReport(Row row, string why, Func<string> now)
        {
            report = string.IsNullOrEmpty(row.Label) ? why : row.Label + ": " + why;
            reportRow = row.Id;
            reportNow = () => { string w = now(); return string.IsNullOrEmpty(w) ? null : string.IsNullOrEmpty(row.Label) ? w : row.Label + ": " + w; };
            reportUntil = Now + ReportSeconds;
            InvalidateVisual();
        }
        /// <summary>A report goes when it is no longer true, when the focus leaves its row and when the page is left.</summary>
        void DropStaleReport(bool pageLeft)
        {
            if (report.Length == 0) return;
            bool onRow = zone == Zone.Card && rowFocus >= 0 && rowFocus < rows.Count && rows[rowFocus].Id == reportRow;
            if (!pageLeft && onRow && reportNow != null && reportNow() == report) return;
            report = "";
            InvalidateVisual();
        }
        void GoBottom(bool preferDefaults)
        {
            zone = Zone.Bottom;
            bottomFocus = preferDefaults && HasDefaults ? 0 : 1;
        }
        int NextRow(int from, int dir)
        {
            for (int i = from + dir; i >= 0 && i < rows.Count; i += dir) if (CanFocus(i)) return i;
            return -1;
        }

        public void DoNav(Nav nav, bool shift)
        {
            bool wasShown = FocusShown;
            focusShown = true;
            InvalidateVisual();
            if (zone == Zone.Card && !CanFocus(rowFocus))
            {
                // The page changed under the focus (LB / RB, Ctrl+Tab): the next key lands on the first row.
                int f = FirstFocusable();
                if (f < 0) zone = Zone.Rail;
                else
                {
                    rowFocus = f; sub = 0;
                    EnsureVisible(f);
                    if (nav == Nav.Up || nav == Nav.Down || nav == Nav.Tab || nav == Nav.ShiftTab) return;
                }
            }
            // The first key after the mouse only shows where the focus is.
            if (!wasShown && (nav == Nav.Up || nav == Nav.Down || nav == Nav.Left || nav == Nav.Right || nav == Nav.Tab || nav == Nav.ShiftTab))
            {
                if (zone == Zone.Card) EnsureVisible(rowFocus);
                return;
            }
            switch (zone)
            {
                case Zone.Rail:
                    if (nav == Nav.Up) railFocus = Math.Max(0, railFocus - 1);
                    else if (nav == Nav.Down) railFocus = Math.Min(pages.Count - 1, railFocus + 1);
                    else if (nav == Nav.Activate || nav == Nav.Space) ShowPage(railFocus);
                    else if (nav == Nav.Right || nav == Nav.Tab)
                    {
                        if (railFocus != page) ShowPage(railFocus);
                        int f = FirstFocusable();
                        if (f >= 0) GoCard(f); else GoBottom(true);
                    }
                    else if (nav == Nav.ShiftTab) GoBottom(false);
                    else if (nav == Nav.Back && backFromPad) { ShowPage(0); railFocus = 0; }
                    break;
                case Zone.Card:
                    {
                        var c = rows[rowFocus].Control;
                        bool locked = !rows[rowFocus].IsEnabled(Model);
                        if (nav == Nav.Up || nav == Nav.ShiftTab)
                        {
                            int i = NextRow(rowFocus, -1);
                            if (i >= 0) GoCard(i, KeepSub(i)); else if (nav == Nav.ShiftTab) { zone = Zone.Rail; railFocus = page; } else scrollTarget = 0;
                        }
                        else if (nav == Nav.Down || nav == Nav.Tab)
                        {
                            int i = NextRow(rowFocus, 1);
                            if (i >= 0) GoCard(i, KeepSub(i)); else { scrollTarget = MaxScroll; GoBottom(true); }
                        }
                        else if (nav == Nav.Back) { zone = Zone.Rail; railFocus = page; }
                        else if (locked)
                        {
                            // The control of a locked row does nothing; trying to use it says why.
                            if (nav == Nav.Activate || nav == Nav.Space) SayLocked(rowFocus);
                            else if (nav == Nav.Left) { zone = Zone.Rail; railFocus = page; }
                        }
                        else
                        {
                            bool used = c.OnNav(this, nav, shift, ref sub);
                            if (!used && nav == Nav.Left) { zone = Zone.Rail; railFocus = page; }
                        }
                        DropStaleReport(false);
                        break;
                    }
                case Zone.Bottom:
                    if (nav == Nav.Left) { if (bottomFocus == 1 && HasDefaults) bottomFocus = 0; else { zone = Zone.Rail; railFocus = page; } }
                    else if (nav == Nav.Right) bottomFocus = 1;
                    else if (nav == Nav.Up) { int l = LastFocusable(); if (l >= 0) GoCard(l); else { zone = Zone.Rail; railFocus = page; } }
                    else if (nav == Nav.Tab) { if (bottomFocus == 0) bottomFocus = 1; else { zone = Zone.Rail; railFocus = page; } }
                    else if (nav == Nav.ShiftTab)
                    {
                        if (bottomFocus == 1 && HasDefaults) bottomFocus = 0;
                        else { int l = LastFocusable(); if (l >= 0) GoCard(l); else { zone = Zone.Rail; railFocus = page; } }
                    }
                    else if (nav == Nav.Back) { zone = Zone.Rail; railFocus = page; }
                    else if (nav == Nav.Activate || nav == Nav.Space)
                    {
                        if (bottomFocus == 0) AskDefaults();
                        else if (CanPlay && PlayRequested != null) PlayRequested();
                    }
                    break;
            }
        }
        int KeepSub(int index)
        {
            var c = rows[index].Control;
            if (c == null || !c.HasSubFocus) return 0;
            // Mods: stay in the same column (switch, up, down).
            return rows[index] is ModRow && rowFocus >= 0 && rowFocus < rows.Count && rows[rowFocus] is ModRow ? sub : 0;
        }

        /// <summary>The controller's buttons count again only after all of them were let go (the launcher comes back from the game).</summary>
        public void WaitForPadRelease() { padWaitRelease = true; padRepeatDir = 0; }

        static bool Down(PadState s, ushort mask) { return (s.Buttons & mask) != 0; }
        public const ushort PadUp = 0x1, PadDown = 0x2, PadLeft = 0x4, PadRight = 0x8, PadStart = 0x10, PadBack = 0x20, PadLS = 0x40, PadRS = 0x80,
            PadLB = 0x100, PadRB = 0x200, PadA = 0x1000, PadB = 0x2000, PadX = 0x4000, PadY = 0x8000;

        static string PadButtonName(ushort bit)
        {
            switch (bit)
            {
                case PadUp: return "DpadUp"; case PadDown: return "DpadDown"; case PadLeft: return "DpadLeft"; case PadRight: return "DpadRight";
                case PadStart: return "Start"; case PadBack: return "Back"; case PadLS: return "LS"; case PadRS: return "RS";
                case PadLB: return "LB"; case PadRB: return "RB"; case PadA: return "A"; case PadB: return "B"; case PadX: return "X"; case PadY: return "Y";
            }
            return null;
        }

        /// <summary>Called every 16 ms with the controller's state while the launcher has the focus.</summary>
        public void PadUpdate(PadState s, bool active)
        {
            bool changedConnection = s.Connected != Pad.Connected || s.Index != Pad.Index || s.Kind != Pad.Kind;
            var prev = lastPad;
            Pad = s;
            lastPad = s;
            if (changedConnection)
            {
                if (!s.Connected && device == InputDevice.Pad) Used(InputDevice.None);
                Rebuild();
            }
            if (!s.Connected || !active) { padRepeatDir = 0; if (stickScroll) SettleScroll(); return; }
            // The stick dots of the Gamepad tab: drawn again alone, only while the dials are in view, and only
            // when a dot has moved by a pixel of the monitor (a stick in the hand never reports the same value twice).
            if (dials.On && (s.LX != prev.LX || s.LY != prev.LY || s.RX != prev.RX || s.RY != prev.RY) && DotsMoved(false)) DrawLive();
            ushort pressedNow = (ushort)(s.Buttons & ~prev.Buttons);
            bool lt = s.LT > 128 && prev.LT <= 128, rt = s.RT > 128 && prev.RT <= 128;
            if (padWaitRelease)
            {
                if (s.Buttons == 0 && s.LT < 60 && s.RT < 60) padWaitRelease = false;
                return;
            }
            if (Capture != null)
            {
                var cap = Capture;
                cap.PadHeld &= s.Buttons;
                if (s.LT < 60 && s.RT < 60) cap.PadTriggersHeld = false;
                if (!cap.Control.PadView)
                {
                    if ((pressedNow & PadB) != 0) { EndCapture(); }
                    return;
                }
                ushort fresh = (ushort)(pressedNow & ~cap.PadHeld);
                for (ushort bit = 1; bit != 0; bit <<= 1)
                    if ((fresh & bit) != 0 && PadButtonName(bit) != null) { Used(InputDevice.Pad); Captured(PadButtonName(bit), false, true); return; }
                if (!cap.PadTriggersHeld && lt) { Captured("LT", false, true); return; }
                if (!cap.PadTriggersHeld && rt) { Captured("RT", false, true); return; }
                return;
            }
            // Directions: the D-pad or the left stick, with repeat.
            int dir = 0;
            const int T = 16000;
            if (Down(s, PadUp) || s.LY > T) dir = 1; else if (Down(s, PadDown) || s.LY < -T) dir = 2;
            else if (Down(s, PadLeft) || s.LX < -T) dir = 3; else if (Down(s, PadRight) || s.LX > T) dir = 4;
            double now = clock.Elapsed.TotalSeconds;
            bool fire = false;
            if (dir != padRepeatDir) { padRepeatDir = dir; padRepeatAt = now + 0.40; fire = dir != 0; }
            else if (dir != 0 && now >= padRepeatAt) { padRepeatAt = now + 0.11; fire = true; }
            if (fire || pressedNow != 0 || lt || rt) Used(InputDevice.Pad);
            fromPad = true;
            try
            {
                if (fire) HandleKey(dir == 1 ? Key.Up : dir == 2 ? Key.Down : dir == 3 ? Key.Left : Key.Right, ModifierKeys.None, false);
                if ((pressedNow & PadA) != 0) HandleKey(Key.Enter, ModifierKeys.None, false);
                if ((pressedNow & PadB) != 0) { backFromPad = true; HandleKey(Key.Escape, ModifierKeys.None, false); backFromPad = false; }
            }
            finally { fromPad = false; }
            if (dialog == null && dropControl == null)
            {
                if ((pressedNow & PadLB) != 0) ShowPage((page + pages.Count - 1) % pages.Count);
                if ((pressedNow & PadRB) != 0) ShowPage((page + 1) % pages.Count);
                if ((pressedNow & PadY) != 0 && HasDefaults) { focusShown = true; AskDefaults(); }
                if ((pressedNow & PadX) != 0 && zone == Zone.Card && Usable(rowFocus)) { focusShown = true; rows[rowFocus].Control.OnNav(this, Nav.Open, false, ref sub); }
                if ((pressedNow & PadStart) != 0 && CanPlay && PlayRequested != null) PlayRequested();
                // The message line's button (Open logs, Details) is the View button's.
                if ((pressedNow & PadBack) != 0 && ShownButton != null && messageAction != null) { focusShown = true; messageAction(); }
                // The right stick scrolls the card; let go, the list settles on a whole row.
                if (Math.Abs((int)s.RY) > 9000 && MaxScroll > 0)
                {
                    Used(InputDevice.Pad);
                    freeScroll = true; stickScroll = true;
                    scrollTarget = Math.Max(0, Math.Min(MaxScroll, scrollTarget - s.RY / 32768.0 * 14));
                    tweens.Remove("scroll");
                    InvalidateVisual();
                }
                else if (stickScroll) SettleScroll();
            }
            else if (stickScroll) SettleScroll();
            if (pressedNow != 0) InvalidateVisual();
        }
    }

    /// <summary>WPF keys as ChiliNative.ini names them. Letters go by position (US layout), as in the game.</summary>
    public static class KeyMap
    {
        [System.Runtime.InteropServices.DllImport("user32.dll")]
        static extern uint MapVirtualKeyW(uint code, uint mapType);

        static readonly byte[] scan = { 0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C };

        public static string Name(Key key)
        {
            if (key >= Key.A && key <= Key.Z)
            {
                try
                {
                    uint sc = MapVirtualKeyW((uint)KeyInterop.VirtualKeyFromKey(key), 0);
                    for (int i = 0; i < 26; i++) if (scan[i] == sc) return ((char)('A' + i)).ToString();
                }
                catch { }
                return key.ToString();
            }
            if (key >= Key.D0 && key <= Key.D9) return ((char)('0' + (key - Key.D0))).ToString();
            if (key >= Key.NumPad0 && key <= Key.NumPad9) return "Numpad" + (key - Key.NumPad0);
            if (key >= Key.F1 && key <= Key.F12) return key == Key.F11 ? null : "F" + (key - Key.F1 + 1);
            switch (key)
            {
                case Key.Space: return "Space";
                case Key.Enter: return "Enter";
                case Key.Escape: return "Escape";
                case Key.Tab: return "Tab";
                case Key.Back: return "Backspace";
                case Key.Up: return "Up";
                case Key.Down: return "Down";
                case Key.Left: return "Left";
                case Key.Right: return "Right";
                case Key.LeftShift: return "LShift";
                case Key.RightShift: return "RShift";
                case Key.LeftCtrl: return "LCtrl";
                case Key.RightCtrl: return "RCtrl";
                case Key.LeftAlt: return "LAlt";
                case Key.RightAlt: return "RAlt";
                case Key.Insert: return "Insert";
                case Key.Delete: return "Delete";
                case Key.Home: return "Home";
                case Key.End: return "End";
                case Key.PageUp: return "PageUp";
                case Key.PageDown: return "PageDown";
                case Key.CapsLock: return "CapsLock";
                case Key.OemComma: return "Comma";
                case Key.OemPeriod: return "Period";
                case Key.OemQuestion: return "Slash";
                case Key.OemSemicolon: return "Semicolon";
                case Key.OemQuotes: return "Quote";
                case Key.OemOpenBrackets: return "LBracket";
                case Key.OemCloseBrackets: return "RBracket";
                case Key.OemPipe: return "Backslash";
                case Key.OemMinus: return "Minus";
                case Key.OemPlus: return "Equals";
                case Key.OemTilde: return "Grave";
            }
            return null;
        }
    }
}
