// Tests that need no window and no controller (run with the settings-file tests by --test-ini):
// PlayStation reports made by hand through the decoder and the position map, the rule for which of two
// controllers is listened to, the names and pictures of the buttons, and that the program holds none of
// the explanatory sentences that were taken out.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace ChiliLauncher
{
    public static class PadTests
    {
        /// <summary>"01 80 FF ..." as bytes, padded with zeros to `length`.</summary>
        public static byte[] Report(string hex, int length)
        {
            var parts = hex.Split(new[] { ' ' }, StringSplitOptions.RemoveEmptyEntries);
            var bytes = new byte[Math.Max(length, parts.Length)];
            for (int i = 0; i < parts.Length; i++) bytes[i] = byte.Parse(parts[i], NumberStyles.HexNumber, CultureInfo.InvariantCulture);
            return bytes;
        }

        // Hand-made reports. The layouts are general knowledge of these controllers, not captures of a device.
        public const string Ds4UsbCrossStickUp = "01 80 00 FF 80 28 00 00 00 00";            // left stick up, right stick right, Cross
        public const string Ds4UsbHatShoulders = "01 80 80 80 80 05 25 00 C0 00";            // hat down-left, L1, L2 (three quarters), Options
        public const string Ds4BluetoothLong = "11 C0 00 80 80 80 80 48 80 01 00 FF";        // Circle, R3, PS, R2 full
        public const string Ds4UsbL2NoTravel = "01 80 80 80 80 08 04 00 00 00";              // button 7 without trigger travel
        public const string DualSenseUsb = "01 80 80 80 80 00 FF 07 18 02 00";               // Square, R1, R2 full
        public const string DualSenseBluetoothLong = "31 01 7F 81 80 80 40 00 05 82 10 00";  // hat right, Triangle, Create, L2 a quarter
        public const string DualSenseBluetoothShort = "01 80 80 80 80 08 20 00 00 00";       // Options
        public const string DualSenseUsbL3 = "01 80 80 80 80 00 00 09 08 40 00";             // L3

        public static PadState State(ushort pid, string hex, int length)
        {
            PsPad.Raw raw;
            var bytes = Report(hex, length);
            return PsPad.Decode(pid, bytes, 0, bytes.Length, out raw) ? PsPad.ToState(raw, pid) : new PadState();
        }

        static byte[] Picture(string button, PadKind look)
        {
            var visual = new DrawingVisual();
            using (var dc = visual.RenderOpen())
            {
                dc.DrawRectangle(Brushes.Black, null, new Rect(0, 0, 40, 28));
                Art.PadGlyph(dc, button, 4, 4, 20, 1, look);
            }
            var bitmap = new RenderTargetBitmap(80, 56, 192, 192, PixelFormats.Pbgra32);
            bitmap.Render(visual);
            var px = new byte[80 * 56 * 4];
            bitmap.CopyPixels(px, 80 * 4, 0);
            return px;
        }

        static int Count(byte[] hay, byte[] needle)
        {
            int n = 0;
            for (int i = 0; i + needle.Length <= hay.Length; i++)
            {
                if (hay[i] != needle[0]) continue;
                int j = 1;
                while (j < needle.Length && hay[i + j] == needle[j]) j++;
                if (j == needle.Length) n++;
            }
            return n;
        }

        public static void Run(Action<string, bool, string> check)
        {
            const ushort ds4 = 0x09CC, ds5 = 0x0CE6;
            // ---- reports ----
            var s = State(ds4, Ds4UsbCrossStickUp, 64);
            check("DualShock 4, USB: Cross is A, the left stick up is up, the right stick right is right",
                s.Connected && s.Kind == PadKind.PlayStation && s.Buttons == Surface.PadA && s.LY == 32767 && s.LX == 0 && s.RX == 32766 && s.RY == 0 && s.LT == 0 && s.RT == 0,
                "buttons " + s.Buttons.ToString("X4") + " LX " + s.LX + " LY " + s.LY + " RX " + s.RX + " RY " + s.RY);
            s = State(ds4, Ds4UsbHatShoulders, 64);
            check("DualShock 4, USB: the hat down-left is two D-pad buttons, L1 is LB, Options is Start, L2 keeps its travel",
                s.Buttons == (Surface.PadDown | Surface.PadLeft | Surface.PadLB | Surface.PadStart) && s.LT == 0xC0 && s.RT == 0, "buttons " + s.Buttons.ToString("X4") + " LT " + s.LT);
            s = State(ds4, Ds4BluetoothLong, 78);
            check("DualShock 4, Bluetooth (report 11): Circle is B, R3 is RS, R2 is the right trigger, the PS button is nothing",
                s.Buttons == (Surface.PadB | Surface.PadRS) && s.RT == 255 && s.LT == 0 && s.LX == 0, "buttons " + s.Buttons.ToString("X4") + " RT " + s.RT);
            s = State(ds4, Ds4UsbL2NoTravel, 64);
            check("a trigger that is pressed as a button without travel still counts as pulled", s.LT == 255 && s.Buttons == 0, "LT " + s.LT);
            s = State(ds5, DualSenseUsb, 64);
            check("DualSense, USB: Square is X, R1 is RB, R2 full", s.Buttons == (Surface.PadX | Surface.PadRB) && s.RT == 255 && s.LT == 0 && s.Pid == ds5, "buttons " + s.Buttons.ToString("X4") + " RT " + s.RT);
            s = State(ds5, DualSenseBluetoothLong, 78);
            check("DualSense, Bluetooth (report 31): hat right, Triangle is Y, Create is Back, L2 a quarter, sticks one step off centre",
                s.Buttons == (Surface.PadRight | Surface.PadY | Surface.PadBack) && s.LT == 0x40 && s.LX == -258 && s.LY == -258, "buttons " + s.Buttons.ToString("X4") + " LT " + s.LT + " LX " + s.LX + " LY " + s.LY);
            s = State(ds5, DualSenseBluetoothShort, 10);
            check("DualSense, Bluetooth, the short report 01 (laid out like a DualShock 4's): Options is Start", s.Buttons == Surface.PadStart && s.LT == 0, "buttons " + s.Buttons.ToString("X4"));
            s = State(ds5, DualSenseUsbL3, 64);
            check("DualSense, USB: L3 is LS", s.Buttons == Surface.PadLS, "buttons " + s.Buttons.ToString("X4"));
            PsPad.Raw raw;
            check("an unknown report id, a report that is too short and a cut one are refused",
                !PsPad.Decode(ds4, Report("05 80 80 80 80 08 00 00 00 00", 64), 0, 64, out raw) && !PsPad.Decode(ds4, Report("01 80 80", 3), 0, 3, out raw)
                && !PsPad.Decode(ds5, Report("31 01 80 80 80 80 00 00 00 08", 10), 0, 10, out raw) && !PsPad.Decode(ds4, null, 0, 64, out raw), "");
            bool hats = true;
            ushort[] expect = { Surface.PadUp, Surface.PadUp | Surface.PadRight, Surface.PadRight, Surface.PadRight | Surface.PadDown, Surface.PadDown,
                                Surface.PadDown | Surface.PadLeft, Surface.PadLeft, Surface.PadLeft | Surface.PadUp, 0 };
            for (int hat = 0; hat <= 8; hat++)
            {
                var r = PsPad.AtRest();
                r.Hat = hat;
                if (PsPad.ToState(r, ds4).Buttons != expect[hat]) hats = false;
            }
            check("the hat's eight directions and its rest are the D-pad", hats, "");
            bool positions = true;
            ushort[] byButton = { Surface.PadX, Surface.PadA, Surface.PadB, Surface.PadY, Surface.PadLB, Surface.PadRB, 0, 0, Surface.PadBack, Surface.PadStart, Surface.PadLS, Surface.PadRS, 0, 0 };
            for (int n = 1; n <= 14; n++)
            {
                var r = PsPad.AtRest();
                r.Buttons = 1u << (n - 1);
                var st = PsPad.ToState(r, ds4);
                if (st.Buttons != byButton[n - 1]) positions = false;
                if ((n == 7) != (st.LT == 255) || (n == 8) != (st.RT == 255)) positions = false;
            }
            check("the position map: buttons 1 to 12 are X A B Y LB RB LT RT Back Start LS RS; 13 (PS) and 14 (touchpad) are nothing", positions, "");

            // ---- one controller at a time ----
            var rest = new PadState { Connected = true, Kind = PadKind.Xbox };
            var psRest = PsPad.ToState(PsPad.AtRest(), ds5);
            var psPress = State(ds5, DualSenseUsb, 64);
            var press = new PadState { Connected = true, Kind = PadKind.Xbox, Buttons = Surface.PadA };
            var a = new PadArbiter();
            check("no controller: nothing is listened to", !a.Choose(new PadState(), new PadState(), 0).Connected, "");
            check("both at rest: the Xbox kind is listened to", a.Choose(rest, psRest, 0.1).Kind == PadKind.Xbox, "");
            check("the PlayStation pad moves while the other rests: it takes over", a.Choose(rest, psPress, 1.0).Kind == PadKind.PlayStation, "");
            check("the other moves a tenth of a second after it: no change yet", a.Choose(press, psRest, 1.1).Kind == PadKind.PlayStation, "");
            check("the other moves after a quarter of a second of rest: it takes over", a.Choose(press, psRest, 1.4).Kind == PadKind.Xbox, "");
            var twin = new PadArbiter();
            bool steady = true;
            for (int i = 0; i < 40; i++) if (twin.Choose(i % 4 < 2 ? press : rest, i % 4 < 2 ? psPress : psRest, i * 0.016).Kind != PadKind.Xbox) steady = false;
            check("one pad that is reported twice (a helper presents it as an Xbox pad too) presses once: the choice stays", steady, "");
            var gone = new PadArbiter();
            gone.Choose(rest, psRest, 0);
            check("the controller listened to is unplugged: the other one is taken", gone.Choose(new PadState(), psRest, 0.1).Kind == PadKind.PlayStation && gone.Choose(new PadState(), psRest, 0.1).Connected, "");
            check("only a PlayStation pad: it is listened to", new PadArbiter().Choose(new PadState(), psRest, 0).Kind == PadKind.PlayStation, "");

            // ---- two PlayStation pads: each has its own state, one is listened to ----
            var two = new PadChooser();
            check("no PlayStation pad has reported: none is connected", !two.Current.Connected, "");
            two.Report(1, psRest, 0.000);
            check("a pad counts as connected once it has sent a report", two.Current.Connected && two.Current.Kind == PadKind.PlayStation && two.Current.Buttons == 0, "");
            var holdCross = State(ds5, "01 80 80 80 80 00 00 00 28 00 00", 64);
            bool held = true;
            for (int i = 0; i < 60; i++)
            {
                // The pad in the hand holds Cross; a second one lies at rest and sends its reports in between.
                two.Report(1, holdCross, 1 + i * 0.004);
                if (two.Current.Buttons != Surface.PadA) held = false;
                two.Report(2, psRest, 1 + i * 0.004 + 0.002);
                if (two.Current.Buttons != Surface.PadA) held = false;
            }
            check("a second pad at rest does not let go what the first one holds (no press again and again)", held, "buttons " + two.Current.Buttons.ToString("X4"));
            two.Report(1, psRest, 1.30);
            two.Report(2, psPress, 1.40);
            check("the second pad is touched a tenth of a second after the first: the first is still listened to", two.Current.Buttons == 0, "buttons " + two.Current.Buttons.ToString("X4"));
            two.Report(2, psPress, 1.60);
            check("touched after the first has rested a quarter of a second: the second is listened to", two.Current.Buttons == psPress.Buttons, "buttons " + two.Current.Buttons.ToString("X4"));
            two.Report(2, State(ds5, "01 80 80 80 80 00 00 00 04 00 00", 64), 1.70);    // D-pad down, held
            two.Remove(2);
            check("the pad listened to is unplugged while a direction is held: nothing stays held, the other pad is listened to",
                two.Current.Connected && two.Current.Buttons == 0, "buttons " + two.Current.Buttons.ToString("X4"));
            two.Report(1, holdCross, 2.0);
            two.Rest();
            check("the launcher is not in front (or hides for the game): what was held is let go", two.Current.Connected && two.Current.Buttons == 0 && two.Current.Pid == ds5, "");
            two.Remove(1);
            check("the last pad is unplugged: none is connected", !two.Current.Connected, "");
            check("the products known here are the DualShock 4 (05C4, 09CC, 0BA0) and the DualSense (0CE6, 0DF2)",
                PsPad.IsKnown(0x05C4) && PsPad.IsKnown(0x09CC) && PsPad.IsKnown(0x0BA0) && PsPad.IsKnown(0x0CE6) && PsPad.IsKnown(0x0DF2) && !PsPad.IsKnown(0x0268), "");

            // ---- names and pictures ----
            check("the PlayStation names of the sixteen places",
                KeyNames.PadDisplay("A", PadKind.PlayStation) == "Cross" && KeyNames.PadDisplay("B", PadKind.PlayStation) == "Circle" && KeyNames.PadDisplay("X", PadKind.PlayStation) == "Square"
                && KeyNames.PadDisplay("Y", PadKind.PlayStation) == "Triangle" && KeyNames.PadDisplay("LB", PadKind.PlayStation) == "L1" && KeyNames.PadDisplay("RT", PadKind.PlayStation) == "R2"
                && KeyNames.PadDisplay("LS", PadKind.PlayStation) == "L3" && KeyNames.PadDisplay("Start", PadKind.PlayStation) == "Options" && KeyNames.PadDisplay("Back", PadKind.PlayStation) == "Share"
                && KeyNames.PadDisplay("Back", PadKind.PlayStation, true) == "Create" && KeyNames.PadDisplay("DpadUp", PadKind.PlayStation) == "D-pad up"
                && KeyNames.PadDisplay("LT") == "Left trigger" && KeyNames.PadDisplay("A") == "A", "");
            var same = new List<string>();
            var blank = Picture("?", PadKind.PlayStation);       // an empty disc
            var empty = new List<string>();
            foreach (var b in new[] { "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "LS", "RS", "Start", "Back" })
            {
                var ps = Picture(b, PadKind.PlayStation);
                if (ps.SequenceEqual(Picture(b, PadKind.Xbox))) same.Add(b);
                if (ps.SequenceEqual(blank)) empty.Add(b);
            }
            check("each of the twelve PlayStation pictures is drawn and differs from the Xbox picture of the same place", same.Count == 0 && empty.Count == 0,
                "same as Xbox: " + string.Join(" ", same) + "; empty: " + string.Join(" ", empty));
            check("the D-pad is the same picture for both", Picture("DpadLeft", PadKind.PlayStation).SequenceEqual(Picture("DpadLeft", PadKind.Xbox))
                && !Picture("DpadLeft", PadKind.Xbox).SequenceEqual(Picture("DpadRight", PadKind.Xbox)), "");

            // ---- no explanatory sentence left in the program ----
            // The sentences that were written under names, beside headings and into the status lines. A C#
            // program keeps its texts as UTF-16; each of these is looked for in the launcher's own file.
            string[] removed =
            {
                "Not played yet", "Have a chili", "Nothing is copied", "frees the mouse", "No mods yet", "Left / right changes", "Press the new button",
                "Controller connected", "Changes are saved when", "Sets the four rows below", "Smooths jagged edges", "Only Auto follows it", "Leave it on",
                "Choose a chip, then press", "A later mod wins", "Drop a mod folder in here", "Where your progress lives", "Everything on these pages",
                "Send this when something goes wrong", "Named in launcher.ini", "The game has no volume setting", "Raise it if the sound crackles",
                "lines the launcher does not know", "Not in this version of the game", "Push the stick gently", "the later one wins", "Advanced has the limit",
                "(empty)", "is also ", "This is Chili Con Carnage (USA), ",
                // Taken out on review.
                "to see this window again", "Looked again", "Is the game running", "The log is in", "The disc id must be", "(move + aim)", "(hold)",
                "Then press F5", "Put the launcher into", "Choose a pack", "(the one that holds", "Keep it there", "Reload / back", "Weapon wheel / cancel",
                "Fast 40", "Normal 70", "Safe 150", "starts the game program beside it", "is not supported; use an",
            };
            var found = new List<string>();
            try
            {
                byte[] self = File.ReadAllBytes(System.Reflection.Assembly.GetExecutingAssembly().Location);
                // The list above is in the program too, as the texts of this test: each must be there exactly once.
                foreach (var text in removed)
                    if (Count(self, Encoding.Unicode.GetBytes(text)) != 1) found.Add(text);
            }
            catch (Exception e) { found.Add(e.Message); }
            check("none of " + removed.Length + " explanatory sentences that were taken out is in the program any more", found.Count == 0, string.Join(" | ", found));

            // ---- the icon ----
            int pictures = 0, largest = 0;
            string sha = "";
            try
            {
                using (var stream = typeof(PadTests).Assembly.GetManifestResourceStream("icon"))
                {
                    var bytes = new byte[stream.Length];
                    stream.Read(bytes, 0, bytes.Length);
                    using (var sha1 = System.Security.Cryptography.SHA1.Create()) sha = string.Concat(sha1.ComputeHash(bytes).Select(b => b.ToString("x2")));
                    var decoder = new IconBitmapDecoder(new MemoryStream(bytes), BitmapCreateOptions.None, BitmapCacheOption.OnLoad);
                    pictures = decoder.Frames.Count;
                    foreach (var frame in decoder.Frames) largest = Math.Max(largest, frame.PixelWidth);
                }
            }
            catch { }
            check("the program carries its icon, up to 256 pixels", pictures >= 4 && largest >= 256, pictures + " pictures, largest " + largest);
            Console.WriteLine("icon: " + pictures + " pictures, largest " + largest + " px, sha1 " + sha);
        }
    }
}
