// Windows pieces the launcher calls directly: XInput for the Xbox kind of gamepad (PlayStation pads are in
// Pads.cs), the system's folder picker, the window's dark title bar.
using System;
using System.Runtime.InteropServices;

namespace ChiliLauncher
{
    public static class XInput
    {
        [StructLayout(LayoutKind.Sequential)]
        struct Gamepad { public ushort Buttons; public byte LeftTrigger, RightTrigger; public short LX, LY, RX, RY; }
        [StructLayout(LayoutKind.Sequential)]
        struct State { public uint Packet; public Gamepad Pad; }

        [DllImport("xinput1_4.dll", EntryPoint = "XInputGetState")]
        static extern uint Get14(uint index, out State state);
        [DllImport("xinput9_1_0.dll", EntryPoint = "XInputGetState")]
        static extern uint Get910(uint index, out State state);

        [StructLayout(LayoutKind.Sequential)]
        struct Vibration { public ushort Left, Right; }
        [DllImport("xinput1_4.dll", EntryPoint = "XInputSetState")]
        static extern uint Set14(uint index, ref Vibration vibration);
        [DllImport("xinput9_1_0.dll", EntryPoint = "XInputSetState")]
        static extern uint Set910(uint index, ref Vibration vibration);

        /// <summary>Both motors of the controller in this slot at this strength (0 to 1; 0 stops them). True when the controller took it.</summary>
        public static bool Vibrate(int index, double strength)
        {
            if (index < 0 || index > 3) return false;
            ushort speed = (ushort)Math.Round(Math.Max(0, Math.Min(1, strength)) * 65535);
            var v = new Vibration { Left = speed, Right = speed };
            try
            {
                if (library == 0) { State unused; Get(0, out unused); }
                if (library == 1) return Set14((uint)index, ref v) == 0;
                if (library == 2) return Set910((uint)index, ref v) == 0;
            }
            catch { }
            return false;
        }

        static int library;      // 0 not tried, 1 xinput1_4, 2 xinput9_1_0, -1 none
        static int slot = -1;
        static double nextScan;

        static bool Get(uint index, out State state)
        {
            state = new State();
            try
            {
                if (library == 0)
                {
                    try { Get14(0, out state); library = 1; }
                    catch (DllNotFoundException) { try { Get910(0, out state); library = 2; } catch { library = -1; } }
                    catch (EntryPointNotFoundException) { library = -1; }
                }
                if (library == 1) return Get14(index, out state) == 0;
                if (library == 2) return Get910(index, out state) == 0;
            }
            catch { library = -1; }
            return false;
        }

        /// <summary>The first connected controller. An empty slot is slow to ask, so all four are tried only every two seconds.</summary>
        public static PadState Read(double now)
        {
            State s;
            if (slot >= 0)
            {
                if (Get((uint)slot, out s)) return Make(slot, s);
                slot = -1;
                nextScan = now;
            }
            if (now >= nextScan)
            {
                nextScan = now + 2.0;
                for (int i = 0; i < 4; i++)
                    if (Get((uint)i, out s)) { slot = i; return Make(i, s); }
            }
            return new PadState();
        }
        public static void Rescan() { nextScan = 0; }
        /// <summary>True when a controller answers in this slot (0 to 3).</summary>
        public static bool Connected(int index) { State s; return Get((uint)index, out s); }

        static PadState Make(int index, State s)
        {
            return new PadState { Connected = true, Kind = PadKind.Xbox, Index = index, Buttons = s.Pad.Buttons, LT = s.Pad.LeftTrigger, RT = s.Pad.RightTrigger, LX = s.Pad.LX, LY = s.Pad.LY, RX = s.Pad.RX, RY = s.Pad.RY };
        }
    }

    public static class Win32
    {
        [DllImport("dwmapi.dll")]
        static extern int DwmSetWindowAttribute(IntPtr hwnd, int attribute, ref int value, int size);
        [DllImport("user32.dll")]
        public static extern short GetAsyncKeyState(int key);
        [DllImport("user32.dll")]
        public static extern bool SetForegroundWindow(IntPtr hwnd);
        [DllImport("user32.dll")]
        public static extern IntPtr GetForegroundWindow();
        [DllImport("kernel32.dll")]
        public static extern bool AttachConsole(int processId);

        /// <summary>A title bar in the launcher's own dark rust instead of the system's grey or white.</summary>
        public static void DarkTitleBar(IntPtr hwnd)
        {
            try
            {
                int on = 1;
                DwmSetWindowAttribute(hwnd, 20, ref on, 4);              // DWMWA_USE_IMMERSIVE_DARK_MODE
                int caption = 0x00050C3C;                                // COLORREF of #3C0C05 (BGR)
                DwmSetWindowAttribute(hwnd, 35, ref caption, 4);         // DWMWA_CAPTION_COLOR (Windows 11)
                int text = 0x00C8E8F6;                                   // #F6E8C8
                DwmSetWindowAttribute(hwnd, 36, ref text, 4);            // DWMWA_TEXT_COLOR
                int border = 0x0003050A;
                DwmSetWindowAttribute(hwnd, 34, ref border, 4);          // DWMWA_BORDER_COLOR
            }
            catch { }
        }

        public static bool ShiftHeld() { return (GetAsyncKeyState(0x10) & 0x8000) != 0; }
    }

    public static class Native
    {
        [DllImport("kernel32.dll")]
        static extern uint GetCurrentThreadId();
        public static int CurrentThreadId() { return (int)GetCurrentThreadId(); }
    }

    /// <summary>The system's own "choose a folder" window (the same one the game uses).</summary>
    public static class FolderPicker
    {
        [ComImport, Guid("DC1C5A9C-E88A-4dde-A5A1-60F82A20AEF7")]
        class FileOpenDialog { }

        [ComImport, Guid("43826D1E-E718-42EE-BC55-A1E261C37BFE"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        interface IShellItem
        {
            void BindToHandler(IntPtr pbc, ref Guid bhid, ref Guid riid, out IntPtr ppv);
            void GetParent(out IShellItem ppsi);
            void GetDisplayName(uint sigdnName, [MarshalAs(UnmanagedType.LPWStr)] out string ppszName);
            void GetAttributes(uint sfgaoMask, out uint psfgaoAttribs);
            void Compare(IShellItem psi, uint hint, out int piOrder);
        }

        [ComImport, Guid("42f85136-db7e-439c-85f1-e4075d135fc8"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        interface IFileDialog
        {
            [PreserveSig] int Show(IntPtr parent);
            void SetFileTypes(uint cFileTypes, IntPtr rgFilterSpec);
            void SetFileTypeIndex(uint iFileType);
            void GetFileTypeIndex(out uint piFileType);
            void Advise(IntPtr pfde, out uint pdwCookie);
            void Unadvise(uint dwCookie);
            void SetOptions(uint fos);
            void GetOptions(out uint pfos);
            void SetDefaultFolder(IShellItem psi);
            void SetFolder(IShellItem psi);
            void GetFolder(out IShellItem ppsi);
            void GetCurrentSelection(out IShellItem ppsi);
            void SetFileName([MarshalAs(UnmanagedType.LPWStr)] string pszName);
            void GetFileName([MarshalAs(UnmanagedType.LPWStr)] out string pszName);
            void SetTitle([MarshalAs(UnmanagedType.LPWStr)] string pszTitle);
            void SetOkButtonLabel([MarshalAs(UnmanagedType.LPWStr)] string pszText);
            void SetFileNameLabel([MarshalAs(UnmanagedType.LPWStr)] string pszLabel);
            void GetResult(out IShellItem ppsi);
            void AddPlace(IShellItem psi, int fdap);
            void SetDefaultExtension([MarshalAs(UnmanagedType.LPWStr)] string pszDefaultExtension);
            void Close(int hr);
            void SetClientGuid(ref Guid guid);
            void ClearClientData();
            void SetFilter(IntPtr pFilter);
        }

        /// <summary>The chosen folder, or null.</summary>
        public static string Show(IntPtr owner, string title)
        {
            try
            {
                var dialog = (IFileDialog)new FileOpenDialog();
                uint options;
                dialog.GetOptions(out options);
                dialog.SetOptions(options | 0x20 /* PICKFOLDERS */ | 0x40 /* FORCEFILESYSTEM */ | 0x800 /* PATHMUSTEXIST */);
                dialog.SetTitle(title);
                if (dialog.Show(owner) != 0) return null;
                IShellItem item;
                dialog.GetResult(out item);
                string path;
                item.GetDisplayName(0x80058000 /* SIGDN_FILESYSPATH */, out path);
                return path;
            }
            catch { return null; }
        }
    }
}
