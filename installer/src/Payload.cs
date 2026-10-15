// The payload Setup carries after its own program: compressed files, a manifest that names them with their
// sha256, and a trailer at the very end. Setup.exe = [the program][blob 0][blob 1]...[manifest][trailer].
// Windows runs the program and ignores what follows it; Setup reads its own file to find the rest.
//
// The trailer (72 bytes): "CHILISETUP1\0", the payload's start, the manifest's offset and length (three
// 64-bit numbers, little-endian), the sha256 of the manifest, 4 zero bytes. Blob offsets in the manifest
// count from the payload's start. Each blob is one file compressed whole with LZMS (the Windows Compression
// API in cabinet.dll, part of Windows since Windows 8), or stored when that would not make it smaller. Two
// files with the same bytes share one blob.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace ChiliSetup
{
    public static class Lzms
    {
        const uint Algorithm = 5;      // COMPRESS_ALGORITHM_LZMS
        [DllImport("cabinet.dll", SetLastError = true)]
        static extern bool CreateCompressor(uint algorithm, IntPtr allocation, out IntPtr handle);
        [DllImport("cabinet.dll", SetLastError = true)]
        static extern bool Compress(IntPtr handle, byte[] source, UIntPtr sourceSize, byte[] target, UIntPtr targetSize, out UIntPtr written);
        [DllImport("cabinet.dll")]
        static extern bool CloseCompressor(IntPtr handle);
        [DllImport("cabinet.dll", SetLastError = true)]
        static extern bool CreateDecompressor(uint algorithm, IntPtr allocation, out IntPtr handle);
        [DllImport("cabinet.dll", SetLastError = true)]
        static extern bool Decompress(IntPtr handle, byte[] source, UIntPtr sourceSize, byte[] target, UIntPtr targetSize, out UIntPtr written);
        [DllImport("cabinet.dll")]
        static extern bool CloseDecompressor(IntPtr handle);

        /// <summary>The bytes compressed; null when compression failed.</summary>
        public static byte[] Pack(byte[] raw)
        {
            IntPtr h;
            if (!CreateCompressor(Algorithm, IntPtr.Zero, out h)) throw new IOException("The compressor could not be made (error " + Marshal.GetLastWin32Error() + ").");
            try
            {
                var target = new byte[raw.Length + raw.Length / 8 + 65536];
                UIntPtr written;
                if (!Compress(h, raw, (UIntPtr)raw.Length, target, (UIntPtr)target.Length, out written))
                    throw new IOException("Compression failed (error " + Marshal.GetLastWin32Error() + ").");
                var result = new byte[(long)written];
                Buffer.BlockCopy(target, 0, result, 0, result.Length);
                return result;
            }
            finally { CloseCompressor(h); }
        }

        /// <summary>Decompresses `packed` into `raw`, which must have exactly the original length.</summary>
        public static void Unpack(byte[] packed, byte[] raw)
        {
            IntPtr h;
            if (!CreateDecompressor(Algorithm, IntPtr.Zero, out h)) throw new IOException("The decompressor could not be made (error " + Marshal.GetLastWin32Error() + ").");
            try
            {
                UIntPtr written;
                if (!Decompress(h, packed, (UIntPtr)packed.Length, raw, (UIntPtr)raw.Length, out written))
                    throw new InvalidDataException("Decompression failed (error " + Marshal.GetLastWin32Error() + ").");
                if ((long)written != raw.Length) throw new InvalidDataException("Decompression gave " + (long)written + " bytes, " + raw.Length + " expected.");
            }
            finally { CloseDecompressor(h); }
        }
    }

    public sealed class PayloadFile
    {
        public string Path, Part, Sha256;
        public long Raw;
        public int Blob = -1;
    }

    public sealed class PayloadBlob
    {
        public long Offset, Length, Raw;
        public string Algorithm, Sha256;
    }

    /// <summary>The payload's damage: Setup says "The installer's data is damaged" and the log says what.</summary>
    public sealed class DamagedException : Exception { public DamagedException(string what) : base(what) { } }

    public sealed class Payload
    {
        public const string Magic = "CHILISETUP1\0";
        public const int TrailerSize = 72;
        public string Source;
        public long Start;
        public string Version = "", Built = "", Publisher = "";
        public string MusicTool, MusicArgs;
        public readonly List<PayloadFile> Files = new List<PayloadFile>();
        public readonly List<PayloadBlob> Blobs = new List<PayloadBlob>();

        public IEnumerable<PayloadFile> Part(string part) { return Files.Where(f => f.Part == part); }
        public bool HasPart(string part) { return Files.Any(f => f.Part == part); }
        public long PartBytes(string part) { return Files.Where(f => f.Part == part).Sum(f => f.Raw); }

        /// <summary>How long the program before the payload is (the whole file when there is no payload).</summary>
        public static long StubLength(string exe)
        {
            using (var f = new FileStream(exe, FileMode.Open, FileAccess.Read, FileShare.Read))
            {
                long start, manifestAt, manifestLength;
                byte[] hash;
                return ReadTrailer(f, out start, out manifestAt, out manifestLength, out hash) ? start : f.Length;
            }
        }

        static bool ReadTrailer(Stream f, out long start, out long manifestAt, out long manifestLength, out byte[] hash)
        {
            start = manifestAt = manifestLength = 0;
            hash = null;
            if (f.Length < TrailerSize) return false;
            var t = new byte[TrailerSize];
            f.Position = f.Length - TrailerSize;
            if (ReadFully(f, t, 0, TrailerSize) != TrailerSize) return false;
            if (Encoding.ASCII.GetString(t, 0, 12) != Magic) return false;
            start = BitConverter.ToInt64(t, 12);
            manifestAt = BitConverter.ToInt64(t, 20);
            manifestLength = BitConverter.ToInt64(t, 28);
            hash = new byte[32];
            Buffer.BlockCopy(t, 36, hash, 0, 32);
            return true;
        }

        static int ReadFully(Stream s, byte[] b, int offset, int count)
        {
            int done = 0;
            while (done < count) { int r = s.Read(b, offset + done, count - done); if (r <= 0) break; done += r; }
            return done;
        }

        /// <summary>
        /// The payload of this file: null when the file carries none (the stub, Uninstall.exe). Throws
        /// DamagedException when it carries one that does not hold together.
        /// </summary>
        public static Payload Open(string exe)
        {
            using (var f = new FileStream(exe, FileMode.Open, FileAccess.Read, FileShare.Read))
            {
                long start, at, length;
                byte[] hash;
                if (!ReadTrailer(f, out start, out at, out length, out hash)) return null;
                if (start < 512 || at < start || length <= 0 || length > (64L << 20) || at + length > f.Length - TrailerSize)
                    throw new DamagedException("the trailer's offsets do not fit the file (" + f.Length + " bytes)");
                var text = new byte[length];
                f.Position = at;
                if (ReadFully(f, text, 0, (int)length) != length) throw new DamagedException("the manifest is cut short");
                using (var sha = SHA256.Create())
                    if (!sha.ComputeHash(text).SequenceEqual(hash)) throw new DamagedException("the manifest's sha256 differs from the trailer's");
                var p = Parse(Encoding.UTF8.GetString(text));
                p.Source = exe;
                p.Start = start;
                foreach (var b in p.Blobs)
                    if (b.Offset < 0 || b.Length < 0 || start + b.Offset + b.Length > at) throw new DamagedException("a blob lies outside the payload");
                return p;
            }
        }

        public static Payload Parse(string json)
        {
            object root;
            try { root = Json.Read(json); }
            catch (Exception e) { throw new DamagedException("the manifest is not JSON: " + e.Message); }
            var p = new Payload
            {
                Version = Json.S(root, "version", ""),
                Built = Json.S(root, "built", ""),
                Publisher = Json.S(root, "publisher", ""),
            };
            var music = Json.Obj(root, "music");
            if (music != null) { p.MusicTool = Json.S(music, "tool"); p.MusicArgs = Json.S(music, "args"); }
            var blobs = Json.Arr(root, "blobs") ?? new List<object>();
            foreach (var b in blobs)
                p.Blobs.Add(new PayloadBlob { Offset = Json.L(b, "offset"), Length = Json.L(b, "length"), Raw = Json.L(b, "raw"), Algorithm = Json.S(b, "algorithm", "lzms"), Sha256 = Json.S(b, "sha256", "") });
            var files = Json.Arr(root, "files") ?? new List<object>();
            foreach (var f in files)
            {
                var pf = new PayloadFile { Path = Json.S(f, "path", ""), Part = Json.S(f, "part", ""), Raw = Json.L(f, "raw"), Sha256 = Json.S(f, "sha256", ""), Blob = (int)Json.L(f, "blob", -1) };
                if (pf.Path.Length == 0 || pf.Path.StartsWith("/") || pf.Path.Contains("..") || pf.Path.Contains(":") || pf.Path.Contains("\\"))
                    throw new DamagedException("a file has a path that is not allowed: " + pf.Path);
                if (pf.Raw > 0 && (pf.Blob < 0 || pf.Blob >= p.Blobs.Count)) throw new DamagedException(pf.Path + " names no blob");
                if (pf.Raw > 0 && p.Blobs[pf.Blob].Raw != pf.Raw) throw new DamagedException(pf.Path + " and its blob differ in size");
                p.Files.Add(pf);
            }
            return p;
        }

        // The last blob read, kept for the next file that shares it.
        int cachedBlob = -1;
        byte[] cached;

        /// <summary>A file's bytes, checked: the blob's sha256 as read, the file's sha256 after decompression.</summary>
        public byte[] Read(PayloadFile file)
        {
            if (file.Raw == 0)
            {
                if (file.Sha256 != Util.Sha256(new byte[0])) throw new DamagedException(file.Path + ": the empty file's sha256 differs");
                return new byte[0];
            }
            if (file.Blob == cachedBlob && cached != null) return cached;
            var blob = Blobs[file.Blob];
            var packed = new byte[blob.Length];
            using (var f = new FileStream(Source, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20))
            {
                f.Position = Start + blob.Offset;
                if (ReadFully(f, packed, 0, packed.Length) != packed.Length) throw new DamagedException(file.Path + ": its blob is cut short");
            }
            if (Util.Sha256(packed) != blob.Sha256) throw new DamagedException(file.Path + ": its blob's sha256 differs");
            byte[] raw;
            if (blob.Algorithm == "store") raw = packed;
            else if (blob.Algorithm == "lzms")
            {
                raw = new byte[blob.Raw];
                try { Lzms.Unpack(packed, raw); }
                catch (Exception e) { throw new DamagedException(file.Path + ": " + e.Message); }
            }
            else throw new DamagedException(file.Path + ": unknown compression " + blob.Algorithm);
            if (Util.Sha256(raw) != file.Sha256) throw new DamagedException(file.Path + ": its sha256 differs after decompression");
            cachedBlob = file.Blob;
            cached = raw;
            return raw;
        }
        public void Forget() { cachedBlob = -1; cached = null; }
    }

    /// <summary>
    /// Makes Setup.exe from a staging folder (scripts\package.ps1's output), the hd_loading pack's folder and
    /// the folder of the music tool. Refuses anything that must not be in a release (section 2.1 of the design).
    /// </summary>
    public static class Packer
    {
        public sealed class Input
        {
            public string Package, HdLoading, SetupFiles, Out, Version = "0.9.0", Publisher = "Chili Con Carnage PC project";
            public string MusicTool = "setup/ChiliSoundExport.exe", MusicArgs = "\"{disc}\" \"{wav}\"";
            public string Stub;
            public int Jobs = 3;
        }

        // Never in a release: the game's files, a player's own data, private notes.
        static readonly string[] gameNames = { "boot.bin", "eboot.bin", "umd_data.bin", "icon0.png", "icon1.pmf", "pic0.png", "pic1.png", "snd0.at3", "param.sfo", "configpsp.txt" };
        static readonly string[] gameExtensions = { ".iso", ".cso", ".naz", ".at3", ".pmf", ".prx", ".sfo", ".texture_psp", ".wav" };
        static readonly string[] playerFolders = { "saves", "art", "textures", "texture_dump", "logs", "captures" };

        /// <summary>What the staging folder holds, mapped to payload paths and parts; problems listed instead of thrown.</summary>
        public static List<KeyValuePair<string, string>> Plan(Input input, List<string> problems, List<string> notes)
        {
            var plan = new List<KeyValuePair<string, string>>();       // payload path -> source file
            string root = Path.GetFullPath(input.Package).TrimEnd('\\');
            if (!Directory.Exists(root)) { problems.Add("No staging folder: " + root); return plan; }
            foreach (var file in Directory.GetFiles(root, "*", SearchOption.AllDirectories).OrderBy(x => x, StringComparer.OrdinalIgnoreCase))
            {
                string rel = file.Substring(root.Length + 1).Replace('\\', '/');
                string lower = rel.ToLowerInvariant(), name = Path.GetFileName(lower);
                string first = lower.Contains('/') ? lower.Substring(0, lower.IndexOf('/')) : "";
                if (Array.IndexOf(playerFolders, first) >= 0) { problems.Add("A player's folder is in the staging folder: " + rel); continue; }
                if (first == "mods" && lower != "mods/readme.md" && lower != "mods/textures.md") { problems.Add("A mod is in the staging folder (packs come only through -HdLoading): " + rel); continue; }
                if (Array.IndexOf(gameNames, name) >= 0 || Array.IndexOf(gameExtensions, Path.GetExtension(name)) >= 0) { problems.Add("A file of the game is in the staging folder: " + rel); continue; }
                if (name == "chilinative.gpucache" || name == "launcher_error.log" || name.EndsWith(".log")) { problems.Add("A file of a player's run is in the staging folder: " + rel); continue; }
                if (name.EndsWith(".previous.exe")) { problems.Add("An earlier program is in the staging folder: " + rel); continue; }
                if (lower == "what_changed.txt") { notes.Add("skipped WHAT_CHANGED.txt (private notes; CHANGELOG.md is the public one)"); continue; }
                if (name.EndsWith(".lnk")) { notes.Add("skipped " + rel + " (Setup makes the shortcuts itself)"); continue; }
                if (lower == "chilinative.ini")
                {
                    string text = File.ReadAllText(file);
                    if (LiveDiscPath(text)) { problems.Add("ChiliNative.ini has a live [Disc] Path line: the release's settings must be the defaults"); continue; }
                    plan.Add(new KeyValuePair<string, string>("template/ChiliNative.ini", file));
                    continue;
                }
                if (lower == "launcher.ini")
                {
                    string text = File.ReadAllText(file);
                    if (text.IndexOf("[GameProbe]", StringComparison.OrdinalIgnoreCase) >= 0) { problems.Add("launcher.ini has a [GameProbe] section: use the three-line file package.ps1 writes"); continue; }
                    plan.Add(new KeyValuePair<string, string>("template/launcher.ini", file));
                    continue;
                }
                plan.Add(new KeyValuePair<string, string>(rel, file));
            }
            // The fonts' licence travels with the program too (Setup carries the fonts; the text is its resource).
            if (!plan.Any(p => string.Equals(p.Key, "licenses/OFL-fonts.txt", StringComparison.OrdinalIgnoreCase)))
            {
                string ofl = Path.Combine(Env.Temp, "OFL-fonts.txt");
                using (var s = typeof(Packer).Assembly.GetManifestResourceStream("OFL.txt"))
                    if (s != null) { using (var o = File.Create(ofl)) s.CopyTo(o); plan.Add(new KeyValuePair<string, string>("licenses/OFL-fonts.txt", ofl)); }
            }
            foreach (var needed in new[] { "ChiliConCarnage.exe", "ChiliConCarnageLauncher.exe", "template/ChiliNative.ini", "template/launcher.ini" })
                if (!plan.Any(p => string.Equals(p.Key, needed, StringComparison.OrdinalIgnoreCase))) problems.Add("Missing in the staging folder: " + needed.Replace("template/", ""));
            if (!string.IsNullOrEmpty(input.HdLoading))
            {
                string pack = Path.GetFullPath(input.HdLoading).TrimEnd('\\');
                if (!Directory.Exists(Path.Combine(pack, "textures"))) problems.Add("No textures folder in " + pack);
                else
                {
                    foreach (var file in Directory.GetFiles(pack, "*", SearchOption.AllDirectories).OrderBy(x => x, StringComparer.OrdinalIgnoreCase))
                    {
                        string rel = file.Substring(pack.Length + 1).Replace('\\', '/');
                        bool picture = rel.StartsWith("textures/", StringComparison.OrdinalIgnoreCase) && rel.IndexOf('/', 9) < 0 &&
                            System.Text.RegularExpressions.Regex.IsMatch(Path.GetFileName(rel), "^[0-9a-fA-F]{16}\\.png$");
                        if (rel.Equals("README.md", StringComparison.OrdinalIgnoreCase) || picture) plan.Add(new KeyValuePair<string, string>("mods/hd_loading/" + rel, file));
                        else problems.Add("Not a file of a texture pack: " + Path.Combine(pack, rel));
                    }
                }
            }
            if (!string.IsNullOrEmpty(input.SetupFiles))
            {
                string dir = Path.GetFullPath(input.SetupFiles).TrimEnd('\\');
                if (!Directory.Exists(dir)) problems.Add("No folder " + dir);
                else
                    foreach (var file in Directory.GetFiles(dir, "*", SearchOption.TopDirectoryOnly).OrderBy(x => x, StringComparer.OrdinalIgnoreCase))
                    {
                        string name = Path.GetFileName(file), lower = name.ToLowerInvariant();
                        if (lower.EndsWith(".exe") || lower.EndsWith(".dll")) plan.Add(new KeyValuePair<string, string>("setup/" + name, file));
                        else notes.Add("skipped " + file + " (the music tool's folder: programs and DLLs only)");
                    }
                if (!string.IsNullOrEmpty(input.MusicTool) && !plan.Any(p => string.Equals(p.Key, input.MusicTool, StringComparison.OrdinalIgnoreCase)))
                    problems.Add("The music tool " + input.MusicTool + " is not among the files of " + dir);
            }
            return plan;
        }

        public static bool LiveDiscPath(string ini)
        {
            bool inDisc = false;
            foreach (var raw in ini.Split('\n'))
            {
                string line = raw.Trim();
                if (line.StartsWith("[")) { inDisc = line.Equals("[Disc]", StringComparison.OrdinalIgnoreCase); continue; }
                if (inDisc && line.StartsWith("Path", StringComparison.OrdinalIgnoreCase) && line.Contains("=")) return true;
            }
            return false;
        }

        static string PartOf(string path)
        {
            if (path.StartsWith("mods/hd_loading/", StringComparison.OrdinalIgnoreCase)) return "hd_loading";
            if (path.StartsWith("setup/", StringComparison.OrdinalIgnoreCase)) return "setup";
            if (path.StartsWith("template/", StringComparison.OrdinalIgnoreCase)) return "template";
            return "program";
        }

        /// <summary>Writes Setup.exe. Returns 0, or 2 with the problems printed.</summary>
        public static int Run(Input input, TextWriter output)
        {
            var problems = new List<string>();
            var notes = new List<string>();
            var plan = Plan(input, problems, notes);
            foreach (var n in notes) output.WriteLine("[pack] " + n);
            if (problems.Count > 0)
            {
                foreach (var p in problems) output.WriteLine("[pack] REFUSED: " + p);
                return 2;
            }
            var clock = System.Diagnostics.Stopwatch.StartNew();
            // Read and hash every file; the same bytes twice share one blob.
            var files = new List<PayloadFile>();
            var bySha = new Dictionary<string, int>();
            var blobSources = new List<string>();
            foreach (var entry in plan)
            {
                var info = new FileInfo(entry.Value);
                string sha = Util.Sha256File(entry.Value);
                var pf = new PayloadFile { Path = entry.Key, Part = PartOf(entry.Key), Raw = info.Length, Sha256 = sha };
                if (info.Length > 0)
                {
                    int blob;
                    if (!bySha.TryGetValue(sha, out blob)) { blob = blobSources.Count; bySha[sha] = blob; blobSources.Add(entry.Value); }
                    pf.Blob = blob;
                }
                files.Add(pf);
            }
            // Compress the blobs, a few at a time, the largest first.
            var packed = new byte[blobSources.Count][];
            var algorithms = new string[blobSources.Count];
            var raws = new long[blobSources.Count];
            var order = Enumerable.Range(0, blobSources.Count).OrderByDescending(i => new FileInfo(blobSources[i]).Length).ToList();
            int done = 0;
            Parallel.ForEach(order, new ParallelOptions { MaxDegreeOfParallelism = Math.Max(1, input.Jobs) }, i =>
            {
                var raw = File.ReadAllBytes(blobSources[i]);
                raws[i] = raw.Length;
                byte[] small = null;
                try { small = Lzms.Pack(raw); } catch { small = null; }
                if (small != null && small.Length < raw.Length) { packed[i] = small; algorithms[i] = "lzms"; }
                else { packed[i] = raw; algorithms[i] = "store"; }
                // The compressed bytes must give the file back.
                if (algorithms[i] == "lzms")
                {
                    var back = new byte[raw.Length];
                    Lzms.Unpack(packed[i], back);
                    if (!back.SequenceEqual(raw)) throw new InvalidDataException("LZMS did not give " + blobSources[i] + " back");
                }
                int n = Interlocked.Increment(ref done);
                lock (output) output.WriteLine(string.Format(CultureInfo.InvariantCulture, "[pack] {0,3}/{1}  {2,12:N0} -> {3,12:N0}  {4}", n, blobSources.Count, raw.Length, packed[i].Length, blobSources[i]));
            });
            // Write: the stub, the blobs, the manifest, the trailer.
            string stub = input.Stub ?? Env.SelfPath;
            long stubLength = Payload.StubLength(stub);
            string outFile = Path.GetFullPath(input.Out);
            Directory.CreateDirectory(Path.GetDirectoryName(outFile));
            string temp = outFile + ".part";
            var blobs = new List<object>();
            using (var o = new FileStream(temp, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 20))
            {
                using (var s = new FileStream(stub, FileMode.Open, FileAccess.Read, FileShare.Read))
                {
                    var buffer = new byte[1 << 20];
                    long left = stubLength;
                    while (left > 0) { int r = s.Read(buffer, 0, (int)Math.Min(buffer.Length, left)); if (r <= 0) throw new IOException("the stub is cut short"); o.Write(buffer, 0, r); left -= r; }
                }
                long start = o.Position;
                for (int i = 0; i < packed.Length; i++)
                {
                    var b = new Dictionary<string, object>
                    {
                        { "offset", o.Position - start }, { "length", (long)packed[i].Length }, { "raw", raws[i] },
                        { "algorithm", algorithms[i] }, { "sha256", Util.Sha256(packed[i]) },
                    };
                    o.Write(packed[i], 0, packed[i].Length);
                    blobs.Add(b);
                }
                var parts = new Dictionary<string, object>();
                foreach (var g in files.GroupBy(f => f.Part))
                    parts[g.Key] = new Dictionary<string, object> { { "files", (long)g.Count() }, { "rawBytes", g.Sum(f => f.Raw) } };
                var manifest = new Dictionary<string, object>
                {
                    { "format", 1L }, { "product", Env.Product }, { "version", input.Version }, { "publisher", input.Publisher },
                    { "built", DateTime.UtcNow.ToString("yyyy-MM-ddTHH:mm:ssZ", CultureInfo.InvariantCulture) },
                    { "parts", parts },
                };
                if (!string.IsNullOrEmpty(input.SetupFiles) && !string.IsNullOrEmpty(input.MusicTool))
                    manifest["music"] = new Dictionary<string, object> { { "tool", input.MusicTool }, { "args", input.MusicArgs } };
                manifest["files"] = files.Select(f => (object)new Dictionary<string, object>
                {
                    { "path", f.Path }, { "part", f.Part }, { "raw", f.Raw }, { "sha256", f.Sha256 }, { "blob", (long)f.Blob },
                }).ToList();
                manifest["blobs"] = blobs;
                var text = Encoding.UTF8.GetBytes(Json.Write(manifest));
                long at = o.Position;
                o.Write(text, 0, text.Length);
                byte[] hash;
                using (var sha = SHA256.Create()) hash = sha.ComputeHash(text);
                var trailer = new byte[Payload.TrailerSize];
                Encoding.ASCII.GetBytes(Payload.Magic).CopyTo(trailer, 0);
                BitConverter.GetBytes(start).CopyTo(trailer, 12);
                BitConverter.GetBytes(at).CopyTo(trailer, 20);
                BitConverter.GetBytes((long)text.Length).CopyTo(trailer, 28);
                hash.CopyTo(trailer, 36);
                o.Write(trailer, 0, trailer.Length);
            }
            if (File.Exists(outFile)) File.Delete(outFile);
            File.Move(temp, outFile);
            // Read it back the way Setup will.
            var check = Payload.Open(outFile);
            foreach (var f in check.Files) check.Read(f);
            check.Forget();
            long size = new FileInfo(outFile).Length;
            foreach (var g in files.GroupBy(f => f.Part))
                output.WriteLine(string.Format(CultureInfo.InvariantCulture, "[pack] part {0}: {1} files, {2:N0} bytes", g.Key, g.Count(), g.Sum(f => f.Raw)));
            output.WriteLine(string.Format(CultureInfo.InvariantCulture, "[pack] wrote {0} ({1:N0} bytes, stub {2:N0}) in {3:0.0} s; every file read back and checked", outFile, size, stubLength, clock.Elapsed.TotalSeconds));
            output.WriteLine("[pack] sha256 " + Util.Sha256File(outFile));
            return 0;
        }
    }
}
