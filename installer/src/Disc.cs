// The player's disc: the same check the game makes when it starts (host/launcher.cpp, open_disc), on an
// .iso read in place or on the folder of an unpacked disc. Nothing of the disc is written anywhere; the two
// pictures the PSP shows for the game (ICON0.PNG, PIC1.PNG) are read into memory for the pages.
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace ChiliSetup
{
    public enum DiscVerdict { None, Ok, NotFound, Cso, NotIso, NotPspFile, NotPspFolder, WrongId, OtherProgram, Incomplete, Unreadable }

    public sealed class DiscResult
    {
        public DiscVerdict Verdict;
        public string Path = "", Message = "", Id = "", Missing = "";
        public bool IsFolder;
        public long Size;
        public long BootSize;
        public ulong BootHash;
        public byte[] Icon0, Pic1;
        public bool Ok { get { return Verdict == DiscVerdict.Ok; } }
        /// <summary>The accepted line: "Chili Con Carnage (USA) · ULUS-10216 · 797,081,600 bytes".</summary>
        public string Summary { get { return "Chili Con Carnage (USA) · " + Disc.Id + " · " + (IsFolder ? "folder" : Util.Grouped(Size) + " bytes"); } }
    }

    public static class Disc
    {
        // host/launcher.hpp: the disc id, and the size and FNV-1a 64 hash of the BOOT.BIN this program was made from.
        public const string Id = "ULUS-10216";
        public const long BootSize = 8616992;
        public const ulong BootHash = 0xA18A6D5249286C4AUL;
        public static readonly string[] Needed = { "PSP_GAME/USRDIR/assets00.naz", "PSP_GAME/USRDIR/configpsp.txt", "PSP_GAME/USRDIR/module/psmf.prx" };

        public static ulong Fnv1a64(byte[] data, int length)
        {
            ulong h = 0xCBF29CE484222325UL;
            for (int i = 0; i < length; i++) { h ^= data[i]; h *= 0x100000001B3UL; }
            return h;
        }

        public const string TextNotIso = "This file cannot be read as an .iso disc image.";
        public const string TextCso = "A compressed .cso image must be converted to a plain .iso first.";
        public const string TextNotPspFile = "This file is not a PSP game disc.";
        public const string TextNotPspFolder = "This folder is not a PSP game disc.";
        public const string TextWrong = "This is not Chili Con Carnage (USA, ULUS-10216).";
        public const string TextOtherProgram = "This copy's game program differs from the one this program was built from.";

        /// <summary>A disc's files, whichever kind it is.</summary>
        interface ISource
        {
            long? Size(string inside);
            byte[] Read(string inside, long limit);
        }

        sealed class FolderSource : ISource
        {
            readonly string root;
            public FolderSource(string r) { root = r; }
            public long? Size(string inside)
            {
                string f = System.IO.Path.Combine(root, inside.Replace('/', '\\'));
                return File.Exists(f) ? new FileInfo(f).Length : (long?)null;
            }
            public byte[] Read(string inside, long limit)
            {
                var s = Size(inside);
                if (s == null || s > limit) return null;
                return File.ReadAllBytes(System.IO.Path.Combine(root, inside.Replace('/', '\\')));
            }
        }

        /// <summary>
        /// ISO 9660 as the game reads it (host/disc_image.hpp): a primary volume descriptor found from sector 16,
        /// 2048-byte blocks, the tree read once, each file in one extent, names without ";1", any case.
        /// </summary>
        public sealed class IsoReader : ISource, IDisposable
        {
            const int Block = 2048;
            readonly Stream s;
            readonly Dictionary<string, long[]> entries = new Dictionary<string, long[]>(StringComparer.OrdinalIgnoreCase);   // key -> offset, size, directory
            public string Problem;
            public IsoReader(Stream stream) { s = stream; }
            public void Dispose() { s.Dispose(); }

            byte[] ReadAt(long offset, int count)
            {
                if (offset < 0 || offset + count > s.Length) return null;
                var b = new byte[count];
                s.Position = offset;
                int done = 0;
                while (done < count) { int r = s.Read(b, done, count - done); if (r <= 0) return null; done += r; }
                return b;
            }

            /// <summary>False when the file is not a plain ISO 9660 image; Problem says why for the log.</summary>
            public bool Open(out bool cso)
            {
                cso = false;
                var head = ReadAt(0, 4);
                if (head != null && Encoding.ASCII.GetString(head) == "CISO") { cso = true; Problem = "CISO magic"; return false; }
                if (s.Length < 17L * Block) { Problem = "too small for a disc image"; return false; }
                byte[] pvd = null;
                for (long sector = 16; ; sector++)
                {
                    if (sector >= 64 || (sector + 1) * Block > s.Length) { Problem = "no ISO 9660 volume descriptor"; return false; }
                    var d = ReadAt(sector * Block, Block);
                    if (d == null || Encoding.ASCII.GetString(d, 1, 5) != "CD001") { Problem = "not ISO 9660"; return false; }
                    if (d[0] == 1) { pvd = d; break; }
                    if (d[0] == 255) { Problem = "no primary volume descriptor"; return false; }
                }
                if (BitConverter.ToUInt16(pvd, 128) != Block) { Problem = "block size is not 2048"; return false; }
                try { Walk(pvd, 156, "", 0); }
                catch (InvalidDataException e) { Problem = e.Message; return false; }
                return true;
            }

            void Walk(byte[] holder, int at, string parent, int depth)
            {
                long start = ((long)BitConverter.ToUInt32(holder, at + 2) + holder[at + 1]) * Block;
                long length = BitConverter.ToUInt32(holder, at + 10);
                if (depth > 8 || length > (4 << 20) || start + length > s.Length) throw new InvalidDataException("a directory lies outside the image");
                var data = ReadAt(start, (int)length);
                if (data == null) throw new InvalidDataException("a directory cannot be read");
                for (int p = 0; p < data.Length;)
                {
                    int size = data[p];
                    if (size == 0) { p = (p / Block + 1) * Block; continue; }
                    if (size < 34 || p + size > data.Length || 33 + data[p + 32] > size) throw new InvalidDataException("a directory record is damaged");
                    int nameLength = data[p + 32];
                    string name = Encoding.ASCII.GetString(data, p + 33, nameLength);
                    int record = p;
                    p += size;
                    if (name == "\0" || name == "\u0001") continue;
                    int semi = name.IndexOf(';');
                    if (semi >= 0) name = name.Substring(0, semi);
                    if (name.EndsWith(".")) name = name.Substring(0, name.Length - 1);
                    if (name.Length == 0 || name == "..") continue;
                    bool dir = (data[record + 25] & 2) != 0;
                    long offset = ((long)BitConverter.ToUInt32(data, record + 2) + data[record + 1]) * Block;
                    long fileSize = BitConverter.ToUInt32(data, record + 10);
                    string key = parent.Length == 0 ? name : parent + "/" + name;
                    if (entries.ContainsKey(key)) continue;
                    if (!dir && ((data[record + 25] & 0x80) != 0 || data[record + 26] != 0)) throw new InvalidDataException(name + " is stored in several pieces");
                    if (!dir && offset + fileSize > s.Length) throw new InvalidDataException(name + " runs past the end of the image");
                    if (entries.Count > 65536) throw new InvalidDataException("too many files");
                    entries[key] = new[] { offset, fileSize, dir ? 1L : 0L };
                    if (dir) Walk(data, record, key, depth + 1);
                }
            }

            public long? Size(string inside)
            {
                long[] e;
                return entries.TryGetValue(inside.Trim('/'), out e) && e[2] == 0 ? e[1] : (long?)null;
            }
            public byte[] Read(string inside, long limit)
            {
                long[] e;
                if (!entries.TryGetValue(inside.Trim('/'), out e) || e[2] != 0 || e[1] > limit) return null;
                return ReadAt(e[0], (int)e[1]);
            }
        }

        /// <summary>The check. `pictures` also reads ICON0.PNG and PIC1.PNG of an accepted disc.</summary>
        public static DiscResult Check(string given, bool pictures)
        {
            var r = new DiscResult();
            string path = Util.Full(given);
            r.Path = path ?? given ?? "";
            if (string.IsNullOrEmpty(path) || (!File.Exists(path) && !Directory.Exists(path)))
            {
                r.Verdict = DiscVerdict.NotFound;
                string name = string.IsNullOrEmpty(given) ? "The disc" : System.IO.Path.GetFileName(given.TrimEnd('\\', '/'));
                r.Message = (string.IsNullOrEmpty(name) ? given : name) + " was not found.";
                return r;
            }
            try
            {
                if (Directory.Exists(path))
                {
                    r.IsFolder = true;
                    string root = path.TrimEnd('\\', '/');
                    for (int up = 0; up < 2 && !Directory.Exists(System.IO.Path.Combine(root, "PSP_GAME")); up++)
                    {
                        string leaf = System.IO.Path.GetFileName(root).ToLowerInvariant();
                        if (leaf != "psp_game" && leaf != "sysdir" && leaf != "usrdir") break;
                        root = System.IO.Path.GetDirectoryName(root);
                    }
                    r.Path = root;
                    var source = new FolderSource(root);
                    if (source.Read("UMD_DATA.BIN", 4096) == null || source.Size("PSP_GAME/SYSDIR/BOOT.BIN") == null)
                    {
                        r.Verdict = DiscVerdict.NotPspFolder;
                        r.Message = TextNotPspFolder;
                        return r;
                    }
                    return Judge(r, source, pictures);
                }
                r.Size = new FileInfo(path).Length;
                using (var f = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite, 65536))
                using (var iso = new IsoReader(f))
                {
                    bool cso;
                    if (!iso.Open(out cso))
                    {
                        r.Verdict = cso ? DiscVerdict.Cso : DiscVerdict.NotIso;
                        r.Message = cso ? TextCso : TextNotIso;
                        Log.Line("disc " + path + ": " + iso.Problem);
                        return r;
                    }
                    if (iso.Read("UMD_DATA.BIN", 4096) == null || iso.Size("PSP_GAME/SYSDIR/BOOT.BIN") == null)
                    {
                        r.Verdict = DiscVerdict.NotPspFile;
                        r.Message = TextNotPspFile;
                        return r;
                    }
                    return Judge(r, iso, pictures);
                }
            }
            catch (Exception e)
            {
                r.Verdict = DiscVerdict.Unreadable;
                r.Message = r.IsFolder ? "This folder cannot be read." : TextNotIso;
                Log.Line("disc " + path + " cannot be read: " + e.Message);
                return r;
            }
        }

        static DiscResult Judge(DiscResult r, ISource disc, bool pictures)
        {
            var umd = disc.Read("UMD_DATA.BIN", 4096);
            var boot = disc.Read("PSP_GAME/SYSDIR/BOOT.BIN", 64L << 20);
            long bootSize = boot != null ? boot.Length : (disc.Size("PSP_GAME/SYSDIR/BOOT.BIN") ?? 0);
            ulong hash = boot != null ? Fnv1a64(boot, boot.Length) : 0;
            Verdict(r, umd, bootSize, hash, inside => (disc.Size(inside) ?? 0) > 0);
            if (r.Ok && pictures)
            {
                r.Icon0 = disc.Read("PSP_GAME/ICON0.PNG", 1 << 20);
                r.Pic1 = disc.Read("PSP_GAME/PIC1.PNG", 4 << 20);
            }
            return r;
        }

        /// <summary>The verdict on what was read, apart from the reading (the tests use it with made-up values).</summary>
        public static void Verdict(DiscResult r, byte[] umd, long bootSize, ulong bootHash, Func<string, bool> present)
        {
            int bar = umd == null ? -1 : Array.IndexOf(umd, (byte)'|');
            string id = umd == null ? "" : Encoding.ASCII.GetString(umd, 0, bar < 0 ? umd.Length : bar);
            r.Id = id;
            r.BootSize = bootSize;
            r.BootHash = bootHash;
            if (id != Id)
            {
                bool printable = id.Length > 0 && id.Length <= 16;
                foreach (char c in id) if (c <= 32 || c >= 127) printable = false;
                r.Verdict = DiscVerdict.WrongId;
                r.Message = printable ? "This is the disc " + id + ", not Chili Con Carnage (USA, ULUS-10216)." : TextWrong;
                return;
            }
            if (bootSize != BootSize || bootHash != BootHash)
            {
                r.Verdict = DiscVerdict.OtherProgram;
                r.Message = TextOtherProgram;
                return;
            }
            foreach (var needed in Needed)
                if (!present(needed))
                {
                    r.Verdict = DiscVerdict.Incomplete;
                    r.Missing = needed;
                    r.Message = "This disc is incomplete: " + needed.Substring(needed.LastIndexOf('/') + 1) + " is missing.";
                    return;
                }
            r.Verdict = DiscVerdict.Ok;
            r.Message = "";
        }

        /// <summary>Tests: a minimal ISO 9660 image with these files (paths with '/', directories made as needed).</summary>
        public static byte[] MakeIso(IList<KeyValuePair<string, byte[]>> files, bool terminator = true)
        {
            const int S = 2048;
            // Directory tree.
            var dirs = new List<string> { "" };
            foreach (var f in files)
            {
                string d = f.Key.Contains("/") ? f.Key.Substring(0, f.Key.LastIndexOf('/')) : "";
                var parts = d.Split(new[] { '/' }, StringSplitOptions.RemoveEmptyEntries);
                string acc = "";
                foreach (var p in parts) { acc = acc.Length == 0 ? p : acc + "/" + p; if (!dirs.Contains(acc)) dirs.Add(acc); }
            }
            int firstDir = 18;
            var dirLba = new Dictionary<string, int>();
            for (int i = 0; i < dirs.Count; i++) dirLba[dirs[i]] = firstDir + i;
            int next = firstDir + dirs.Count;
            var fileLba = new int[files.Count];
            for (int i = 0; i < files.Count; i++) { fileLba[i] = next; next += Math.Max(1, (files[i].Value.Length + S - 1) / S); }
            var img = new byte[(long)next * S];
            Func<int, string, int, int, bool, int> record = (offset, name, lba, size, dir) =>
            {
                int len = 33 + name.Length + ((name.Length % 2 == 0) ? 1 : 0);
                img[offset] = (byte)len;
                BitConverter.GetBytes((uint)lba).CopyTo(img, offset + 2);
                BitConverter.GetBytes((uint)size).CopyTo(img, offset + 10);
                img[offset + 25] = (byte)(dir ? 2 : 0);
                img[offset + 32] = (byte)name.Length;
                Encoding.ASCII.GetBytes(name).CopyTo(img, offset + 33);
                return offset + len;
            };
            img[16 * S] = 1;
            Encoding.ASCII.GetBytes("CD001").CopyTo(img, 16 * S + 1);
            BitConverter.GetBytes((ushort)S).CopyTo(img, 16 * S + 128);
            record(16 * S + 156, "\0", firstDir, S, true);
            if (terminator) { img[17 * S] = 255; Encoding.ASCII.GetBytes("CD001").CopyTo(img, 17 * S + 1); }
            foreach (var d in dirs)
            {
                int p = dirLba[d] * S;
                string parent = d.Contains("/") ? d.Substring(0, d.LastIndexOf('/')) : "";
                p = record(p, "\0", dirLba[d], S, true);
                p = record(p, "\u0001", dirLba[parent], S, true);
                foreach (var sub in dirs)
                {
                    if (sub.Length == 0) continue;
                    string subParent = sub.Contains("/") ? sub.Substring(0, sub.LastIndexOf('/')) : "";
                    if (subParent == d) p = record(p, sub.Substring(sub.LastIndexOf('/') + 1), dirLba[sub], S, true);
                }
                for (int i = 0; i < files.Count; i++)
                {
                    string fd = files[i].Key.Contains("/") ? files[i].Key.Substring(0, files[i].Key.LastIndexOf('/')) : "";
                    if (fd == d) p = record(p, files[i].Key.Substring(files[i].Key.LastIndexOf('/') + 1) + ";1", fileLba[i], files[i].Value.Length, false);
                }
            }
            for (int i = 0; i < files.Count; i++) files[i].Value.CopyTo(img, (long)fileLba[i] * S);
            return img;
        }
    }
}
