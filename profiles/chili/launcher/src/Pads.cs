// Controllers beside the Xbox kind: PlayStation pads, read as the game reads them (Raw Input for the
// reports, the system's HID parser in hid.dll for their meaning), and the rule for which of two
// controllers the launcher listens to (DESIGN.md, "Controllers").
//
// No PlayStation controller was at hand when this was written: the byte layouts in PsPad.Decode are
// general knowledge of the DualShock 4 and DualSense reports, checked only against hand-made reports
// (--test-ini). The path through hid.dll has never met a device.
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace ChiliLauncher
{
    /// <summary>A PlayStation controller's input report, decoded and mapped onto the sixteen buttons an Xbox pad has.</summary>
    public static class PsPad
    {
        public const ushort Vendor = 0x054C;

        /// <summary>
        /// What a report says before it is mapped: HID buttons 1 to 14 as bits 0 to 13 (1 Square, 2 Cross,
        /// 3 Circle, 4 Triangle, 5 L1, 6 R1, 7 L2, 8 R2, 9 Share or Create, 10 Options, 11 L3, 12 R3, 13 PS,
        /// 14 touchpad), the hat (0 up, clockwise to 7 up-left; 8 at rest), sticks and triggers 0 to 255
        /// (sticks: 128 at rest, Y grows downward).
        /// </summary>
        public struct Raw
        {
            public uint Buttons;
            public int Hat;
            public byte LX, LY, RX, RY, L2, R2;
        }

        public static bool IsDualSense(ushort pid) { return pid == 0x0CE6 || pid == 0x0DF2; }
        /// <summary>The products whose reports are known here: DualShock 4 (05C4, 09CC, its wireless adaptor 0BA0), DualSense (0CE6, Edge 0DF2).</summary>
        public static bool IsKnown(ushort pid) { return pid == 0x05C4 || pid == 0x09CC || pid == 0x0BA0 || IsDualSense(pid); }
        public const ushort MicrosoftVendor = 0x045E;

        public static Raw AtRest() { return new Raw { Hat = 8, LX = 128, LY = 128, RX = 128, RY = 128 }; }

        /// <summary>
        /// Decodes an input report by its byte layout; `report[offset]` is the report id and `length` counts
        /// from there. Known: report 01 of a DualShock 4 (USB, and the short one over Bluetooth), report 11
        /// (DualShock 4, Bluetooth), report 01 of a DualSense over USB (64 bytes: triggers before the
        /// buttons), its short report 01 over Bluetooth (laid out like a DualShock 4's) and report 31
        /// (DualSense, Bluetooth). False for anything else.
        /// </summary>
        public static bool Decode(ushort pid, byte[] report, int offset, int length, out Raw raw)
        {
            raw = AtRest();
            if (report == null || offset < 0 || length < 10 || offset + length > report.Length) return false;
            byte id = report[offset];
            int sticks;                 // where the four stick bytes begin
            bool triggersFirst = false; // the DualSense's full layout: sticks, triggers, a counter, then the buttons
            if (id == 0x01) { sticks = offset + 1; triggersFirst = IsDualSense(pid) && length >= 64; }
            else if (id == 0x11) sticks = offset + 3;
            else if (id == 0x31) { sticks = offset + 2; triggersFirst = true; }
            else return false;
            int buttons = triggersFirst ? sticks + 7 : sticks + 4;
            int last = triggersFirst ? buttons + 2 : sticks + 8;
            if (last >= offset + length) return false;
            raw.LX = report[sticks]; raw.LY = report[sticks + 1]; raw.RX = report[sticks + 2]; raw.RY = report[sticks + 3];
            if (triggersFirst) { raw.L2 = report[sticks + 4]; raw.R2 = report[sticks + 5]; }
            else { raw.L2 = report[sticks + 7]; raw.R2 = report[sticks + 8]; }
            raw.Hat = report[buttons] & 0x0F;
            if (raw.Hat > 8) raw.Hat = 8;
            raw.Buttons = (uint)((report[buttons] >> 4) | (report[buttons + 1] << 4) | ((report[buttons + 2] & 0x03) << 12));
            return true;
        }

        static short Axis(byte value, bool flip)
        {
            int v = (value - 128) * 258;
            if (flip) v = -v;
            return (short)Math.Max(-32768, Math.Min(32767, v));
        }

        /// <summary>
        /// The position map of the design: Square = X, Cross = A, Circle = B, Triangle = Y, L1 = LB, R1 = RB,
        /// L2 = LT, R2 = RT, Share (Create) = Back, Options = Start, L3 = LS, R3 = RS, the hat is the D-pad.
        /// The result is laid out as XInput lays out an Xbox pad, so everything after this is the same code.
        /// </summary>
        public static PadState ToState(Raw raw, ushort pid, int index = 0)
        {
            ushort buttons = 0;
            uint b = raw.Buttons;
            if ((b & 0x001) != 0) buttons |= Surface.PadX;
            if ((b & 0x002) != 0) buttons |= Surface.PadA;
            if ((b & 0x004) != 0) buttons |= Surface.PadB;
            if ((b & 0x008) != 0) buttons |= Surface.PadY;
            if ((b & 0x010) != 0) buttons |= Surface.PadLB;
            if ((b & 0x020) != 0) buttons |= Surface.PadRB;
            if ((b & 0x100) != 0) buttons |= Surface.PadBack;
            if ((b & 0x200) != 0) buttons |= Surface.PadStart;
            if ((b & 0x400) != 0) buttons |= Surface.PadLS;
            if ((b & 0x800) != 0) buttons |= Surface.PadRS;
            switch (raw.Hat)
            {
                case 0: buttons |= Surface.PadUp; break;
                case 1: buttons |= Surface.PadUp | Surface.PadRight; break;
                case 2: buttons |= Surface.PadRight; break;
                case 3: buttons |= Surface.PadRight | Surface.PadDown; break;
                case 4: buttons |= Surface.PadDown; break;
                case 5: buttons |= Surface.PadDown | Surface.PadLeft; break;
                case 6: buttons |= Surface.PadLeft; break;
                case 7: buttons |= Surface.PadLeft | Surface.PadUp; break;
            }
            byte lt = raw.L2, rt = raw.R2;
            // The triggers are buttons 7 and 8 too: a pad that reports no travel still pulls them.
            if ((b & 0x040) != 0 && lt == 0) lt = 255;
            if ((b & 0x080) != 0 && rt == 0) rt = 255;
            return new PadState
            {
                Connected = true, Kind = PadKind.PlayStation, Index = index, Pid = pid, Buttons = buttons, LT = lt, RT = rt,
                LX = Axis(raw.LX, false), LY = Axis(raw.LY, true), RX = Axis(raw.RX, false), RY = Axis(raw.RY, true),
            };
        }
    }

    /// <summary>
    /// One controller at a time. A PlayStation pad that a helper program also presents as an Xbox pad must
    /// not press twice, so the launcher listens to one of the two; the other takes over only when it moves
    /// while the one listened to has been at rest for a quarter of a second (the game's rule).
    /// </summary>
    public sealed class PadArbiter
    {
        int active = -1;           // 0: the Xbox kind (XInput), 1: the PlayStation pad, -1: none yet
        readonly double[] moved = { double.NegativeInfinity, double.NegativeInfinity };
        public const double Rest = 0.25;

        public static bool Moves(PadState s)
        {
            return s.Connected && (s.Buttons != 0 || s.LT > 40 || s.RT > 40 ||
                Math.Abs((int)s.LX) > 12000 || Math.Abs((int)s.LY) > 12000 || Math.Abs((int)s.RX) > 12000 || Math.Abs((int)s.RY) > 12000);
        }

        public PadState Choose(PadState xbox, PadState playStation, double now)
        {
            var both = new[] { xbox, playStation };
            for (int i = 0; i < 2; i++) if (Moves(both[i])) moved[i] = now;
            if (active >= 0 && !both[active].Connected) active = -1;
            if (active < 0)
            {
                if (Moves(playStation) && !Moves(xbox)) active = 1;
                else if (xbox.Connected) active = 0;
                else if (playStation.Connected) active = 1;
            }
            else
            {
                int other = 1 - active;
                if (Moves(both[other]) && now - moved[active] >= Rest) active = other;
            }
            return active < 0 ? new PadState() : both[active];
        }
    }

    /// <summary>
    /// One PlayStation pad at a time, as in the game: every pad keeps its own state, and the launcher
    /// listens to the one touched last. Another pad takes over only when it moves while the one listened
    /// to has been at rest for a quarter of a second, so a second pad that lies on the table and sends its
    /// reports changes nothing, and a button held on the first stays held. A pad counts as connected once
    /// it has sent a report: a device that is listed but says nothing is not a controller to use.
    /// </summary>
    public sealed class PadChooser
    {
        sealed class Entry { public PadState State; public double Moved = double.NegativeInfinity; }
        readonly Dictionary<long, Entry> pads = new Dictionary<long, Entry>();
        long listened;
        bool any;

        /// <summary>How many pads have reported.</summary>
        public int Count { get { return pads.Count; } }

        /// <summary>A report of the pad `id`, decoded.</summary>
        public void Report(long id, PadState state, double now)
        {
            Entry e;
            if (!pads.TryGetValue(id, out e)) pads[id] = e = new Entry();
            e.State = state;
            bool moves = PadArbiter.Moves(state);
            if (moves) e.Moved = now;
            if (!any) { listened = id; any = true; }
            else if (id != listened && moves && now - pads[listened].Moved >= PadArbiter.Rest) listened = id;
        }

        /// <summary>The pad was unplugged: what it held goes with it. Was it the one listened to, the pad touched last of the others is.</summary>
        public void Remove(long id)
        {
            pads.Remove(id);
            if (!any || id != listened) return;
            any = false;
            double latest = double.NegativeInfinity;
            foreach (var other in pads)
                if (!any || other.Value.Moved > latest) { listened = other.Key; latest = other.Value.Moved; any = true; }
        }
        public void Clear() { pads.Clear(); any = false; }

        /// <summary>Everything held is let go: no reports arrive while the launcher is not in front or is hidden.</summary>
        public void Rest()
        {
            foreach (var e in pads.Values) e.State = PsPad.ToState(PsPad.AtRest(), e.State.Pid);
        }

        /// <summary>The state of the pad listened to; not connected while no pad has reported.</summary>
        public PadState Current { get { return any ? pads[listened].State : new PadState(); } }
    }

    /// <summary>
    /// The PlayStation controllers on this PC, through Raw Input: vendor 054C, game pads and joysticks,
    /// leaving out the devices XInput reports already (their path holds "IG_"). Reports arrive as WM_INPUT
    /// at the launcher's window while it is the window in front.
    /// </summary>
    public sealed class HidPads
    {
        [StructLayout(LayoutKind.Sequential)]
        struct RawInputDevice { public ushort UsagePage, Usage; public uint Flags; public IntPtr Target; }
        [StructLayout(LayoutKind.Sequential)]
        struct RawInputDeviceList { public IntPtr Device; public uint Type; }

        [DllImport("user32.dll", SetLastError = true)]
        static extern bool RegisterRawInputDevices([In] RawInputDevice[] devices, uint count, uint size);
        [DllImport("user32.dll", SetLastError = true)]
        static extern uint GetRawInputDeviceList([In, Out] RawInputDeviceList[] list, ref uint count, uint size);
        [DllImport("user32.dll", SetLastError = true, EntryPoint = "GetRawInputDeviceInfoW")]
        static extern uint GetRawInputDeviceInfo(IntPtr device, uint command, IntPtr data, ref uint size);
        [DllImport("user32.dll", SetLastError = true)]
        static extern uint GetRawInputData(IntPtr rawInput, uint command, IntPtr data, ref uint size, uint headerSize);
        [DllImport("hid.dll")]
        static extern int HidP_GetCaps(IntPtr preparsed, IntPtr caps);
        [DllImport("hid.dll")]
        static extern int HidP_GetValueCaps(int reportType, IntPtr caps, ref ushort count, IntPtr preparsed);
        [DllImport("hid.dll")]
        static extern int HidP_GetUsages(int reportType, ushort usagePage, ushort collection, [In, Out] ushort[] usages, ref uint count, IntPtr preparsed, IntPtr report, uint reportLength);
        [DllImport("hid.dll")]
        static extern int HidP_GetUsageValue(int reportType, ushort usagePage, ushort collection, ushort usage, out uint value, IntPtr preparsed, IntPtr report, uint reportLength);

        const uint RimTypeHid = 2, RidInput = 0x10000003, RidiName = 0x20000007, RidiInfo = 0x2000000b, RidiPreparsed = 0x20000005, RidevDevNotify = 0x2000;
        const int HidpInput = 0, HidpSuccess = 0x00110000;
        public const int WmInput = 0x00FF, WmInputDeviceChange = 0x00FE;

        sealed class Device
        {
            public IntPtr Handle, Preparsed;
            public ushort Pid;
            public string Path;
            // Logical ranges of the values the report descriptor names: usage (X 30, Y 31, Z 32, Rx 33, Ry 34, Rz 35, hat 39) -> min, max.
            public readonly Dictionary<ushort, KeyValuePair<int, int>> Ranges = new Dictionary<ushort, KeyValuePair<int, int>>();
        }

        readonly Dictionary<IntPtr, Device> devices = new Dictionary<IntPtr, Device>();
        IntPtr window;
        // Each pad's own state, and which one is listened to. Reports are stamped with this clock.
        readonly PadChooser chooser = new PadChooser();
        readonly System.Diagnostics.Stopwatch clock = System.Diagnostics.Stopwatch.StartNew();
        double nextScan;
        /// <summary>What was found, for the log and the tests: one line per device looked at.</summary>
        public readonly List<string> Seen = new List<string>();

        public bool Attached { get { return window != IntPtr.Zero; } }

        /// <summary>Asks Windows to send the launcher's window the reports of game pads and joysticks, and looks what is there.</summary>
        public void Attach(IntPtr hwnd)
        {
            try
            {
                var wanted = new[]
                {
                    new RawInputDevice { UsagePage = 1, Usage = 4, Flags = RidevDevNotify, Target = hwnd },
                    new RawInputDevice { UsagePage = 1, Usage = 5, Flags = RidevDevNotify, Target = hwnd },
                };
                if (RegisterRawInputDevices(wanted, (uint)wanted.Length, (uint)Marshal.SizeOf(typeof(RawInputDevice)))) window = hwnd;
            }
            catch { }
            Scan();
        }

        static uint Info(IntPtr device, uint command, IntPtr data, uint size)
        {
            uint n = size;
            uint got = GetRawInputDeviceInfo(device, command, data, ref n);
            return got == uint.MaxValue ? 0 : (data == IntPtr.Zero ? n : got);
        }

        /// <summary>Looks at the devices again: at start, when one arrives or leaves, and on F5.</summary>
        public void Scan()
        {
            try
            {
                uint count = 0;
                uint size = (uint)Marshal.SizeOf(typeof(RawInputDeviceList));
                GetRawInputDeviceList(null, ref count, size);
                var list = new RawInputDeviceList[count];
                if (count > 0)
                {
                    uint got = GetRawInputDeviceList(list, ref count, size);
                    if (got == uint.MaxValue) return;
                    if (got < list.Length) Array.Resize(ref list, (int)got);
                }
                var present = new HashSet<IntPtr>();
                Seen.Clear();
                foreach (var entry in list)
                {
                    if (entry.Type != RimTypeHid) continue;
                    present.Add(entry.Device);
                    if (devices.ContainsKey(entry.Device)) { Seen.Add(devices[entry.Device].Path + " (kept)"); continue; }
                    var d = Open(entry.Device);
                    if (d != null) devices[entry.Device] = d;
                }
                foreach (var gone in new List<IntPtr>(devices.Keys))
                    if (!present.Contains(gone)) { Close(devices[gone]); devices.Remove(gone); chooser.Remove(gone.ToInt64()); }
                if (devices.Count == 0) chooser.Clear();
            }
            catch { }
        }

        Device Open(IntPtr handle)
        {
            IntPtr buffer = IntPtr.Zero;
            try
            {
                // The device's path: an Xbox pad's HID side has "IG_" in it and is XInput's to report.
                uint chars = Info(handle, RidiName, IntPtr.Zero, 0);
                if (chars == 0 || chars > 4096) return null;
                buffer = Marshal.AllocHGlobal((int)chars * 2 + 2);
                if (Info(handle, RidiName, buffer, chars) == 0) return null;
                string path = Marshal.PtrToStringUni(buffer) ?? "";
                Marshal.FreeHGlobal(buffer); buffer = IntPtr.Zero;
                // RID_DEVICE_INFO: size, type, then for a HID device vendor, product, version, usage page, usage.
                buffer = Marshal.AllocHGlobal(32);
                Marshal.WriteInt32(buffer, 0, 32);
                if (Info(handle, RidiInfo, buffer, 32) == 0) return null;
                if ((uint)Marshal.ReadInt32(buffer, 4) != RimTypeHid) return null;
                uint vendor = (uint)Marshal.ReadInt32(buffer, 8), product = (uint)Marshal.ReadInt32(buffer, 12);
                ushort page = (ushort)Marshal.ReadInt16(buffer, 20), usage = (ushort)Marshal.ReadInt16(buffer, 22);
                Marshal.FreeHGlobal(buffer); buffer = IntPtr.Zero;
                string what = vendor.ToString("X4") + ":" + product.ToString("X4") + " " + path;
                if (page != 1 || (usage != 4 && usage != 5)) { Seen.Add(what + " (usage " + page.ToString("X") + "/" + usage.ToString("X") + ": not a game controller)"); return null; }
                if (path.IndexOf("IG_", StringComparison.OrdinalIgnoreCase) >= 0) { Seen.Add(what + " (XInput reports it)"); return null; }
                // A controller of Microsoft's own is an Xbox pad whatever its path says: XInput reads it (as in the game).
                if (vendor == PsPad.MicrosoftVendor) { Seen.Add(what + " (an Xbox controller: read through XInput)"); return null; }
                if (vendor != PsPad.Vendor) { Seen.Add(what + " (neither an Xbox nor a PlayStation controller: not read)"); return null; }
                var d = new Device { Handle = handle, Pid = (ushort)product, Path = path };
                // What hid.dll needs to read a report: the device's parsed report descriptor.
                uint bytes = Info(handle, RidiPreparsed, IntPtr.Zero, 0);
                if (bytes > 0 && bytes < (1 << 20))
                {
                    d.Preparsed = Marshal.AllocHGlobal((int)bytes);
                    if (Info(handle, RidiPreparsed, d.Preparsed, bytes) == 0) { Marshal.FreeHGlobal(d.Preparsed); d.Preparsed = IntPtr.Zero; }
                }
                if (d.Preparsed != IntPtr.Zero) ReadRanges(d);
                // Any product of Sony's is read, as the game reads it; the five known ones also by their byte layout.
                Seen.Add(what + " (PlayStation controller" + (PsPad.IsKnown(d.Pid) ? "" : ", a product not known here") + (d.Preparsed == IntPtr.Zero ? ", read by byte layout" : "") + ")");
                return d;
            }
            catch { return null; }
            finally { if (buffer != IntPtr.Zero) Marshal.FreeHGlobal(buffer); }
        }

        static void Close(Device d)
        {
            if (d.Preparsed != IntPtr.Zero) { Marshal.FreeHGlobal(d.Preparsed); d.Preparsed = IntPtr.Zero; }
        }

        /// <summary>The logical range of each value (sticks, triggers, hat) from the report descriptor.</summary>
        static void ReadRanges(Device d)
        {
            IntPtr caps = Marshal.AllocHGlobal(64), values = IntPtr.Zero;
            try
            {
                // HIDP_CAPS is 64 bytes; NumberInputValueCaps is the USHORT at offset 48.
                if (HidP_GetCaps(d.Preparsed, caps) != HidpSuccess) return;
                ushort count = (ushort)Marshal.ReadInt16(caps, 48);
                if (count == 0 || count > 256) return;
                // HIDP_VALUE_CAPS is 72 bytes: UsagePage 0, IsRange 12, LogicalMin 40, LogicalMax 44, Usage (or UsageMin) 56, UsageMax 58.
                values = Marshal.AllocHGlobal(72 * count);
                if (HidP_GetValueCaps(HidpInput, values, ref count, d.Preparsed) != HidpSuccess) return;
                for (int i = 0; i < count; i++)
                {
                    IntPtr one = IntPtr.Add(values, 72 * i);
                    if ((ushort)Marshal.ReadInt16(one, 0) != 1) continue;           // Generic Desktop
                    ushort first = (ushort)Marshal.ReadInt16(one, 56);
                    ushort lastUsage = Marshal.ReadByte(one, 12) != 0 ? (ushort)Marshal.ReadInt16(one, 58) : first;
                    int min = Marshal.ReadInt32(one, 40), max = Marshal.ReadInt32(one, 44);
                    for (int u = first; u <= lastUsage && u < first + 16; u++) d.Ranges[(ushort)u] = new KeyValuePair<int, int>(min, max);
                }
            }
            catch { }
            finally { Marshal.FreeHGlobal(caps); if (values != IntPtr.Zero) Marshal.FreeHGlobal(values); }
        }

        static bool Value(Device d, ushort usage, IntPtr report, uint length, out int value, out int min, out int max)
        {
            value = 0; min = 0; max = 255;
            KeyValuePair<int, int> range;
            if (d.Ranges.TryGetValue(usage, out range)) { min = range.Key; max = range.Value; }
            uint v;
            if (HidP_GetUsageValue(HidpInput, 1, 0, usage, out v, d.Preparsed, report, length) != HidpSuccess) return false;
            value = (int)v;
            return true;
        }
        static byte Scale(int value, int min, int max)
        {
            if (max <= min) { min = 0; max = 255; }
            double t = (value - min) / (double)(max - min);
            return (byte)Math.Max(0, Math.Min(255, Math.Round(t * 255)));
        }

        /// <summary>One report through the system's HID parser: the buttons pressed, the hat, the six values.</summary>
        static bool Parse(Device d, IntPtr report, uint length, out PsPad.Raw raw)
        {
            raw = PsPad.AtRest();
            if (d.Preparsed == IntPtr.Zero) return false;
            var usages = new ushort[32];
            uint count = (uint)usages.Length;
            if (HidP_GetUsages(HidpInput, 9, 0, usages, ref count, d.Preparsed, report, length) != HidpSuccess) return false;
            for (int i = 0; i < count && i < usages.Length; i++)
                if (usages[i] >= 1 && usages[i] <= 14) raw.Buttons |= 1u << (usages[i] - 1);
            int v, min, max;
            if (Value(d, 0x30, report, length, out v, out min, out max)) raw.LX = Scale(v, min, max);
            if (Value(d, 0x31, report, length, out v, out min, out max)) raw.LY = Scale(v, min, max);
            if (Value(d, 0x32, report, length, out v, out min, out max)) raw.RX = Scale(v, min, max);
            if (Value(d, 0x35, report, length, out v, out min, out max)) raw.RY = Scale(v, min, max);
            if (Value(d, 0x33, report, length, out v, out min, out max)) raw.L2 = Scale(v, min, max);
            if (Value(d, 0x34, report, length, out v, out min, out max)) raw.R2 = Scale(v, min, max);
            // The hat counts from its logical minimum; a value outside its range is "at rest".
            if (Value(d, 0x39, report, length, out v, out min, out max)) raw.Hat = v >= min && v <= max && v - min < 8 ? v - min : 8;
            return true;
        }

        /// <summary>WM_INPUT: the reports of one device. (The caller lets the message go on to DefWindowProc.)</summary>
        public void Input(IntPtr rawInput)
        {
            IntPtr buffer = IntPtr.Zero;
            try
            {
                uint header = (uint)(8 + 2 * IntPtr.Size);       // RAWINPUTHEADER: type, size, device, wParam
                uint size = 0;
                GetRawInputData(rawInput, RidInput, IntPtr.Zero, ref size, header);
                if (size < header + 8 || size > (1 << 16)) return;
                buffer = Marshal.AllocHGlobal((int)size);
                if (GetRawInputData(rawInput, RidInput, buffer, ref size, header) != size) return;
                if ((uint)Marshal.ReadInt32(buffer, 0) != RimTypeHid) return;
                IntPtr handle = Marshal.ReadIntPtr(buffer, 8);
                Device d;
                if (!devices.TryGetValue(handle, out d)) return;
                // RAWHID: the size of one report, how many, then the bytes.
                int each = Marshal.ReadInt32(buffer, (int)header), reports = Marshal.ReadInt32(buffer, (int)header + 4);
                if (each <= 0 || reports <= 0 || header + 8 + (long)each * reports > size) return;
                IntPtr lastReport = IntPtr.Add(buffer, (int)header + 8 + each * (reports - 1));
                PsPad.Raw raw;
                if (!Parse(d, lastReport, (uint)each, out raw))
                {
                    // A report the descriptor does not describe (the long ones over Bluetooth): by its byte layout.
                    var bytes = new byte[each];
                    Marshal.Copy(lastReport, bytes, 0, each);
                    if (!PsPad.Decode(d.Pid, bytes, 0, each, out raw)) return;
                }
                chooser.Report(handle.ToInt64(), PsPad.ToState(raw, d.Pid), clock.Elapsed.TotalSeconds);
            }
            catch { }
            finally { if (buffer != IntPtr.Zero) Marshal.FreeHGlobal(buffer); }
        }

        /// <summary>
        /// The state of the PlayStation pad listened to. A pad is connected once it has sent a report. While
        /// the launcher is not the window in front no reports arrive, so what was held then is let go. With no
        /// device known, the list is looked at again every two seconds.
        /// </summary>
        public PadState Read(double now, bool front)
        {
            if (devices.Count == 0)
            {
                if (now >= nextScan) { nextScan = now + 2.0; Scan(); }
                if (devices.Count == 0) return new PadState();
            }
            if (!front) chooser.Rest();
            return chooser.Current;
        }
        /// <summary>Everything held is let go (the launcher hides for the game, or comes back from it).</summary>
        public void Rest() { chooser.Rest(); }
        public void Rescan() { nextScan = 0; Scan(); }
    }
}
