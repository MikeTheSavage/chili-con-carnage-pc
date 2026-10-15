// Setup compiles three files of the launcher unchanged (profiles/chili/launcher/src: Theme.cs, Pads.cs,
// Native.cs). They use a few types of the launcher's Surface.cs, which Setup does not compile; the lines
// below are copies of them. When the launcher's Surface.cs changes these, change them here too:
//   enum PadKind                     Surface.cs line 51
//   struct PadState                  Surface.cs lines 53-63
//   the Pad* button constants        Surface.cs lines 1978-1979 (Surface.PadUp ... Surface.PadY)
namespace ChiliLauncher
{
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

    /// <summary>The launcher's button bits (XInput's layout), under the name Pads.cs uses.</summary>
    public static class Surface
    {
        public const ushort PadUp = 0x1, PadDown = 0x2, PadLeft = 0x4, PadRight = 0x8, PadStart = 0x10, PadBack = 0x20, PadLS = 0x40, PadRS = 0x80,
            PadLB = 0x100, PadRB = 0x200, PadA = 0x1000, PadB = 0x2000, PadX = 0x4000, PadY = 0x8000;
    }
}
