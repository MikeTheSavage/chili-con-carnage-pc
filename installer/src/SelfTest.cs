// --selftest --iso <file> --home <folder>: the real window, shown without taking the focus, driven by made-up
// keys and a made-up controller through the same entry points as the player's (the keys the window gets,
// the controller state it polls, a dropped file). It goes from page 1 to page 6 with the given disc and
// installs into the test home; every check is a line of the log, and the exit code is the number of failed
// checks. "Start the game when done" is switched off on the way, so nothing is started.
using System;
using System.Collections.Generic;
using System.Linq;
using System.Windows.Input;
using System.Windows.Threading;
using ChiliLauncher;

namespace ChiliSetup
{
    public static class SelfTest
    {
        public static int Failures;
        static SetupWindow w;
        static Args args;
        static IEnumerator<Wait> script;
        static Wait current;
        static readonly System.Diagnostics.Stopwatch clock = System.Diagnostics.Stopwatch.StartNew();

        sealed class Wait { public Func<bool> Done; public double Until; public string What; }
        static Wait Until(Func<bool> done, double seconds, string what) { return new Wait { Done = done, Until = clock.Elapsed.TotalSeconds + seconds, What = what }; }
        static Wait Pause(double seconds) { double end = clock.Elapsed.TotalSeconds + seconds; return new Wait { Done = () => clock.Elapsed.TotalSeconds >= end, Until = end + 1 }; }
        static void Check(bool ok, string what)
        {
            if (!ok) Failures++;
            Log.Line("selftest " + (ok ? "PASS " : "FAIL ") + what);
        }
        static void Key(Key k, ModifierKeys mods = ModifierKeys.None) { w.HandleKey(k, mods); }

        public static void Attach(SetupWindow window, Args a)
        {
            w = window;
            args = a;
            // No sound and no picture in the way: the music runs without a device (ten times faster, so that the
            // 229 s track reaches its loop point), and the window stands off the screen, never in front.
            Player.NullAudio = true;
            window.WindowStartupLocation = System.Windows.WindowStartupLocation.Manual;
            window.Left = System.Windows.SystemParameters.VirtualScreenLeft - 4000;
            window.Top = System.Windows.SystemParameters.VirtualScreenTop;
            Remembered.Set("Volume", 60);
            Remembered.Set("Muted", 0);
            var timer = new DispatcherTimer(DispatcherPriority.Background) { Interval = TimeSpan.FromMilliseconds(40) };
            window.Loaded += (s, e) => { script = Script().GetEnumerator(); timer.Start(); };
            timer.Tick += (s, e) =>
            {
                if (current != null)
                {
                    bool done = false;
                    try { done = current.Done(); } catch { }
                    if (!done && clock.Elapsed.TotalSeconds < current.Until) return;
                    if (!done && current.What != null) Check(false, current.What + " (timed out)");
                    else if (current.What != null) Check(true, current.What);
                }
                bool more;
                try { more = script.MoveNext(); }
                catch (Exception ex) { Check(false, "the script: " + ex.Message); more = false; }
                if (!more) { timer.Stop(); current = null; if (w.IsVisible) w.Close(); return; }
                current = script.Current;
            };
        }

        static IEnumerable<Wait> PadPress(ushort button)
        {
            w.FakePad = new PadState { Connected = true, Kind = PadKind.Xbox, Buttons = button };
            yield return Pause(0.08);
            w.FakePad = new PadState { Connected = true, Kind = PadKind.Xbox };
            yield return Pause(0.08);
        }

        static IEnumerable<Wait> Script()
        {
            var m = w.M;
            yield return Until(() => w.View.Hots.Count > 0, 10, "the window is drawn");
            Check(m.Page == Page.Welcome && m.Damaged == null, "page 1, the payload is readable");
            Key(System.Windows.Input.Key.Enter);
            yield return Until(() => m.Page == Page.Disc, 2, "Enter: page 2");
            // A dropped file takes the same way as a chosen one.
            w.View.Dropped(args.Iso);
            yield return Until(() => !m.Checking && m.Disc != null, 60, "the disc check ends");
            Check(m.DiscOk, "the disc is accepted: " + (m.Disc != null ? m.Disc.Summary + " " + m.Disc.Message : "-"));
            Check(m.Icon0 != null && m.Pic1 != null, "the disc's two pictures are shown");
            bool music = w.M.DiscOk && HasMusic();
            if (music)
            {
                yield return Until(() => w.Music.Playing, 30, "the music plays");
                Key(System.Windows.Input.Key.M);
                yield return Pause(0.4);
                Check(Remembered.Get("Muted", 0) == 1 && m.Muted, "M mutes, and it is remembered");
                Key(System.Windows.Input.Key.M);
                yield return Pause(0.4);
                Check(Remembered.Get("Muted", 1) == 0 && !m.Muted, "M again: sound, remembered");
                for (int i = 0; i < 20 && w.View.FocusId != "volume"; i++) Key(System.Windows.Input.Key.Tab);
                Check(w.View.FocusId == "volume", "Tab reaches the volume");
                Key(System.Windows.Input.Key.Right);
                Key(System.Windows.Input.Key.Right);
                yield return Pause(0.3);
                Check(Remembered.Get("Volume", 0) == 70 && m.Volume == 70, "Right twice: volume 70, remembered");
                Key(System.Windows.Input.Key.Left);
                Key(System.Windows.Input.Key.Left);
                yield return Pause(0.2);
            }
            else Log.Line("selftest: the payload has no music tool; the music checks are skipped");
            foreach (var x in PadPress(0x10)) yield return x;               // Start
            yield return Until(() => m.Page == Page.Folder && !m.FolderChecking && m.Folder != null, 15, "the controller's Start: page 3, the folder checked");
            Check(m.FolderOk, "the folder is taken: " + m.Dir + " " + (m.Folder != null ? m.Folder.Verdict + " " + m.Folder.Message : ""));
            foreach (var x in PadPress(0x10)) yield return x;
            yield return Until(() => m.Page == Page.Options, 3, "Start again: page 4");
            for (int i = 0; i < 12 && w.View.FocusId != "toggle_launch"; i++)
                foreach (var x in PadPress(0x2)) yield return x;            // D-pad down
            Check(w.View.FocusId == "toggle_launch", "the D-pad reaches 'Start the game when done'");
            foreach (var x in PadPress(0x1000)) yield return x;             // A
            Check(!m.Launch && m.HdLoading && m.Desktop && m.StartMenu, "A switches it off; the other three stay on");
            Key(System.Windows.Input.Key.Enter);
            yield return Until(() => m.Page == Page.Install, 3, "Enter: the install starts");
            yield return Until(() => m.Page == Page.Done, 300, "the install ends");
            Check(m.Result != null && m.Result.Code == Exit.Done, "installed: " + (m.Result != null ? m.Result.Code + " " + m.Result.Message + " " + m.Result.Dir : "-"));
            if (music)
            {
                yield return Until(() => w.Music.Wraps >= 1, 60, "the music went through its loop point");
                Log.Line("selftest: the music went through its loop point " + w.Music.Wraps + " times");
            }
            Check(m.Ask == null && w.View.Hots.Any(h => h.Id == "next" && h.Enabled), "page 6 has its Finish button");
            Key(System.Windows.Input.Key.Enter);
            yield return Pause(1.0);
        }

        static bool HasMusic()
        {
            try { var p = Payload.Open(Env.SelfPath); return p != null && !string.IsNullOrEmpty(p.MusicTool) && p.HasPart("setup"); } catch { return false; }
        }
    }
}
