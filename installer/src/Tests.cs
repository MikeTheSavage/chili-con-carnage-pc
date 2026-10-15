// --test: what can be checked without a window and without the game. Made-up files only: a made-up payload,
// made-up discs, a made-up save; with --iso the player's disc is also checked (read only). Everything is
// written under a temp folder that is removed at the end; the registry keys used are the test ones
// (HKCU\...\Uninstall\ChiliConCarnagePC-Test, HKCU\Software\ChiliConCarnagePC\Test) and are removed too.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;
using Microsoft.Win32;

namespace ChiliSetup
{
    public static class Tests
    {
        static int passed, failed;
        static void Check(string what, bool ok, string detail = null)
        {
            if (ok) passed++; else failed++;
            Console.WriteLine((ok ? "PASS " : "FAIL ") + what + (ok || detail == null ? "" : "  (" + detail + ")"));
        }

        public static int Run(string iso)
        {
            string root = Path.Combine(Path.GetTempPath(), "ChiliSetupTest-" + Guid.NewGuid().ToString("N").Substring(0, 8));
            Directory.CreateDirectory(root);
            Log.Open(Path.Combine(root, "test.log"), "Setup tests");
            Env.Home = Path.Combine(root, "home");
            try
            {
                Lzms_();
                Json_();
                Args_();
                Ini_();
                Discs(root, iso);
                Folders(root);
                MusicLoop();
                Payloads(root);
                Texts();
            }
            catch (Exception e) { Check("no exception", false, e.ToString()); }
            finally
            {
                if (failed > 0)
                    foreach (var line in Log.ThisRun.Split('\n').Where(l => l.Contains("error"))) Console.WriteLine("  log: " + line.Trim());
                try { Registry.CurrentUser.DeleteSubKeyTree(Env.UninstallKeyRoot + "ChiliConCarnagePC-Test", false); } catch { }
                try { Registry.CurrentUser.DeleteSubKeyTree(@"Software\ChiliConCarnagePC\Test", false); } catch { }
                try { Directory.Delete(root, true); } catch { }
            }
            Console.WriteLine(passed + " passed, " + failed + " failed");
            return failed == 0 ? 0 : 1;
        }

        static void Lzms_()
        {
            var rng = new Random(7);
            var noise = new byte[300000];
            rng.NextBytes(noise);
            var text = Encoding.ASCII.GetBytes(string.Concat(Enumerable.Repeat("Chili Con Carnage PC, a setup for the player's own disc. ", 20000)));
            foreach (var data in new[] { noise, text, new byte[] { 1 } })
            {
                var packed = Lzms.Pack(data);
                var back = new byte[data.Length];
                Lzms.Unpack(packed, back);
                Check("LZMS gives back " + data.Length + " bytes (" + packed.Length + " packed)", back.SequenceEqual(data));
            }
            Check("LZMS makes text smaller", Lzms.Pack(text).Length < text.Length / 20);
        }

        static void Json_()
        {
            var o = new Dictionary<string, object> { { "a", "x\"y\\z\n" }, { "n", 123456789012L }, { "l", new List<object> { 1L, "two", true, null } }, { "o", new Dictionary<string, object> { { "k", false } } } };
            var back = Json.Read(Json.Write(o));
            Check("JSON round trip", Json.S(back, "a") == "x\"y\\z\n" && Json.L(back, "n") == 123456789012L && Json.Arr(back, "l").Count == 4 && Json.B(Json.Obj(back, "o"), "k", true) == false);
        }

        static void Args_()
        {
            Check("--silent without --iso is refused", Args.Parse(new[] { "--silent" }).Problem != null);
            Check("an unknown argument is refused", Args.Parse(new[] { "--whatever" }).Problem != null);
            Check("--iso without a value is refused", Args.Parse(new[] { "--iso" }).Problem != null);
            var a = Args.Parse(new[] { "--silent", "--iso", @"D:\x.iso", "--dir", @"D:\Games\CCC", "--no-hd-loading", "--no-desktop", "--no-start-menu", "--launch", "--log", @"D:\l.log" });
            Check("the silent arguments", a.Problem == null && a.Silent && a.Iso == @"D:\x.iso" && a.Dir == @"D:\Games\CCC" && a.NoHd && a.NoDesktop && a.NoStartMenu && a.Launch && a.Log == @"D:\l.log");
            Check("--pack needs --package and --out", Args.Parse(new[] { "--pack", "--package", "x" }).Problem != null);
            Check("--scale takes a number", Args.Parse(new[] { "--screenshot", "x", "--scale", "2" }).Scale == 2 && Args.Parse(new[] { "--scale", "nine" }).Problem != null);
        }

        const string DefaultIni = "[Render]\nScale=2\n\n[Disc]\n; The game disc: your own copy.\n; Remove the line to be asked again.\n; Path=C:\\Games\\Chili Con Carnage (USA).iso\n\n[Audio]\nBufferMs=80\n";

        static void Ini_()
        {
            string lf = IniEdit.SetDiscPath(DefaultIni, @"D:\My Games\Chili.iso");
            Check("[Disc] Path is written over the commented example", lf.Contains("\n; Remove the line to be asked again.\nPath=D:\\My Games\\Chili.iso\n") && !lf.Contains("; Path=") && lf.Contains("[Audio]\nBufferMs=80\n"), lf);
            Check("nothing else of the file changes", lf.Replace("Path=D:\\My Games\\Chili.iso", "; Path=C:\\Games\\Chili Con Carnage (USA).iso") == DefaultIni);
            string crlf = IniEdit.SetDiscPath(DefaultIni.Replace("\n", "\r\n"), @"E:\c.iso");
            Check("CRLF line ends are kept", crlf.Contains("\r\nPath=E:\\c.iso\r\n") && !Regex.IsMatch(crlf, "[^\r]\n"));
            string none = IniEdit.SetDiscPath("[Render]\r\nScale=2\r\n", @"E:\c.iso");
            Check("a file without [Disc] gets one", none.EndsWith("\r\n[Disc]\r\nPath=E:\\c.iso\r\n"), none);
            Check("a live Path is seen", Packer.LiveDiscPath(lf) && !Packer.LiveDiscPath(DefaultIni));
            Check("a disc inside the game's folder is written relative", IniEdit.DiscValue(@"C:\Games\CCC\disc\game.iso", @"C:\Games\CCC") == @"disc\game.iso" && IniEdit.DiscValue(@"D:\x.iso", @"C:\Games\CCC") == @"D:\x.iso");
        }

        static List<KeyValuePair<string, byte[]>> DiscFiles(string id, byte[] boot, bool complete)
        {
            var files = new List<KeyValuePair<string, byte[]>>
            {
                new KeyValuePair<string, byte[]>("UMD_DATA.BIN", Encoding.ASCII.GetBytes(id + "|0123456789ABCDEF|0001|G|")),
                new KeyValuePair<string, byte[]>("PSP_GAME/SYSDIR/BOOT.BIN", boot),
            };
            if (complete)
            {
                files.Add(new KeyValuePair<string, byte[]>("PSP_GAME/USRDIR/assets00.naz", new byte[] { 1, 2, 3 }));
                files.Add(new KeyValuePair<string, byte[]>("PSP_GAME/USRDIR/configpsp.txt", new byte[] { 4 }));
                files.Add(new KeyValuePair<string, byte[]>("PSP_GAME/USRDIR/MODULE/psmf.prx", new byte[] { 5 }));
            }
            return files;
        }

        static void Discs(string root, string iso)
        {
            string d = Path.Combine(root, "discs");
            Directory.CreateDirectory(d);
            // An .iso with the right id and a made-up BOOT.BIN: the id passes, the game program does not.
            File.WriteAllBytes(Path.Combine(d, "right_id.iso"), Disc.MakeIso(DiscFiles(Disc.Id, new byte[] { 1, 2, 3, 4 }, true)));
            var r = Disc.Check(Path.Combine(d, "right_id.iso"), false);
            Check("a made-up .iso with the right id and another BOOT.BIN: 'differs'", r.Verdict == DiscVerdict.OtherProgram && r.Message == Disc.TextOtherProgram, r.Verdict + " " + r.Message);
            File.WriteAllBytes(Path.Combine(d, "other.iso"), Disc.MakeIso(DiscFiles("ULES-00629", new byte[] { 1 }, true)));
            r = Disc.Check(Path.Combine(d, "other.iso"), false);
            Check("a made-up .iso of another game: its id is named", r.Verdict == DiscVerdict.WrongId && r.Message == "This is the disc ULES-00629, not Chili Con Carnage (USA, ULUS-10216).", r.Message);
            File.WriteAllBytes(Path.Combine(d, "odd.iso"), Disc.MakeIso(DiscFiles("\u0001\u0002", new byte[] { 1 }, true)));
            Check("an id that is not text: the general sentence", Disc.Check(Path.Combine(d, "odd.iso"), false).Message == Disc.TextWrong);
            var cso = new byte[40000]; Encoding.ASCII.GetBytes("CISO").CopyTo(cso, 0);
            File.WriteAllBytes(Path.Combine(d, "game.cso"), cso);
            Check("a CSO is refused with its own sentence", Disc.Check(Path.Combine(d, "game.cso"), false).Verdict == DiscVerdict.Cso);
            File.WriteAllBytes(Path.Combine(d, "empty.iso"), new byte[0]);
            Check("an empty file cannot be read as an .iso", Disc.Check(Path.Combine(d, "empty.iso"), false).Message == Disc.TextNotIso);
            File.WriteAllBytes(Path.Combine(d, "zeros.iso"), new byte[2048 * 40]);
            Check("a file without ISO 9660 cannot be read as an .iso", Disc.Check(Path.Combine(d, "zeros.iso"), false).Verdict == DiscVerdict.NotIso);
            var noUmd = DiscFiles(Disc.Id, new byte[] { 1 }, true); noUmd.RemoveAt(0);
            File.WriteAllBytes(Path.Combine(d, "no_umd.iso"), Disc.MakeIso(noUmd));
            Check("an .iso without UMD_DATA.BIN is not a PSP game disc", Disc.Check(Path.Combine(d, "no_umd.iso"), false).Verdict == DiscVerdict.NotPspFile);
            var nf = Disc.Check(Path.Combine(d, "missing.iso"), false);
            Check("a missing file: 'was not found'", nf.Verdict == DiscVerdict.NotFound && nf.Message == "missing.iso was not found.", nf.Message);
            // Folders, as the game reads them; PSP_GAME or SYSDIR itself names the same disc.
            string folder = Path.Combine(d, "unpacked");
            foreach (var f in DiscFiles(Disc.Id, new byte[0], true))
            {
                string p = Path.Combine(folder, f.Key.Replace('/', '\\'));
                Directory.CreateDirectory(Path.GetDirectoryName(p));
                File.WriteAllBytes(p, f.Value);
            }
            r = Disc.Check(folder, false);
            Check("a disc folder with an empty BOOT.BIN: 'differs'", r.Verdict == DiscVerdict.OtherProgram && r.IsFolder, r.Verdict.ToString());
            r = Disc.Check(Path.Combine(folder, "PSP_GAME", "SYSDIR"), false);
            Check("SYSDIR names the disc folder above it", r.Verdict == DiscVerdict.OtherProgram && string.Equals(r.Path, folder, StringComparison.OrdinalIgnoreCase), r.Path);
            Check("a folder without PSP_GAME is not a PSP game disc", Disc.Check(d, false).Verdict == DiscVerdict.NotPspFolder);
            // The verdict alone, with the size and hash of the game's own BOOT.BIN.
            var umd = Encoding.ASCII.GetBytes(Disc.Id + "|X|");
            var v = new DiscResult();
            Disc.Verdict(v, umd, Disc.BootSize, Disc.BootHash, x => true);
            Check("the right id, size and hash with every file: accepted", v.Ok);
            v = new DiscResult();
            Disc.Verdict(v, umd, Disc.BootSize, Disc.BootHash, x => !x.EndsWith("assets00.naz"));
            Check("assets00.naz missing: 'incomplete'", v.Verdict == DiscVerdict.Incomplete && v.Message == "This disc is incomplete: assets00.naz is missing.", v.Message);
            v = new DiscResult();
            Disc.Verdict(v, umd, Disc.BootSize, Disc.BootHash ^ 1, x => true);
            Check("one bit of BOOT.BIN's hash off: 'differs'", v.Verdict == DiscVerdict.OtherProgram);
            Check("FNV-1a 64 of 'a' is af63dc4c8601ec8c", Disc.Fnv1a64(new byte[] { 0x61 }, 1) == 0xAF63DC4C8601EC8CUL);
            if (!string.IsNullOrEmpty(iso))
            {
                var real = Disc.Check(iso, true);
                Check("the player's disc is accepted (" + iso + ")", real.Ok, real.Verdict + " " + real.Message);
                Check("its BOOT.BIN is 8,616,992 bytes with FNV-1a 64 a18a6d5249286c4a", real.BootSize == Disc.BootSize && real.BootHash == Disc.BootHash, real.BootSize + " " + real.BootHash.ToString("x16"));
                Check("its two pictures are read into memory", real.Icon0 != null && real.Pic1 != null && real.Icon0.Length > 0 && real.Pic1.Length > 0);
            }
            else Console.WriteLine("(no --iso: the player's disc is not checked)");
        }

        static void Folders(string root)
        {
            string f = Path.Combine(root, "folders");
            Directory.CreateDirectory(f);
            Check("a drive is refused", Folder.Check(Path.GetPathRoot(root), 1).Verdict == FolderVerdict.Drive);
            string full = Path.Combine(f, "full");
            Directory.CreateDirectory(full);
            File.WriteAllText(Path.Combine(full, "stray.txt"), "x");
            Check("a folder with a stray file is refused", Folder.Check(full, 1).Verdict == FolderVerdict.NotEmpty);
            string empty = Path.Combine(f, "empty");
            Directory.CreateDirectory(empty);
            Check("an empty folder is taken", Folder.Check(empty, 1).Verdict == FolderVerdict.Ok);
            string fresh = Path.Combine(f, "new", "deeper");
            var c = Folder.Check(fresh, 1);
            Check("a folder that does not exist yet is taken and not left behind by the probe", c.Verdict == FolderVerdict.Ok && !Directory.Exists(Path.Combine(f, "new")), c.Verdict.ToString());
            string earlier = Path.Combine(f, "earlier");
            Directory.CreateDirectory(earlier);
            File.WriteAllText(Path.Combine(earlier, "ChiliConCarnage.exe"), "x");
            File.WriteAllText(Path.Combine(earlier, "other.txt"), "x");
            Check("a folder with the game in it is an update", Folder.Check(earlier, 1).Verdict == FolderVerdict.Update);
            Check("a file is not a folder", Folder.Check(Path.Combine(full, "stray.txt"), 1).Verdict == FolderVerdict.NotWritable);
            Check("not enough free space", Folder.Check(empty, long.MaxValue / 4).Verdict == FolderVerdict.NoSpace);
        }

        static void MusicLoop()
        {
            // A tone of one second, looped over the whole of it: ten seconds equal ten copies, sample for sample.
            int rate = 8000;
            var tone = new short[rate * 2];
            for (int i = 0; i < rate; i++) { short s = (short)(8000 * Math.Sin(i * 2 * Math.PI * 440 / rate)); tone[2 * i] = s; tone[2 * i + 1] = (short)-s; }
            var t = new Track { Samples = tone, Channels = 2, Rate = rate, LoopStart = 0, LoopEnd = rate };
            var looper = new Looper(t);
            var out10 = new short[rate * 2 * 10];
            var buffer = new short[441 * 2];
            int at = 0;
            while (at < out10.Length)
            {
                int frames = Math.Min(441, (out10.Length - at) / 2);
                looper.Fill(buffer, frames);
                Array.Copy(buffer, 0, out10, at, frames * 2);
                at += frames * 2;
            }
            bool same = true;
            for (int i = 0; i < out10.Length; i++) if (out10[i] != tone[i % tone.Length]) { same = false; break; }
            Check("a 1 s tone looped 10 times equals 10 copies, with buffers that cross the loop point", same && looper.Wraps == 9, "wraps " + looper.Wraps);
            // Loop points inside the track: the intro once, then start..end.
            var ramp = new short[100];
            for (int i = 0; i < 100; i++) ramp[i] = (short)i;
            var lp = new Looper(new Track { Samples = ramp, Channels = 1, Rate = 100, LoopStart = 20, LoopEnd = 60 });
            var got = new short[150];
            lp.Fill(got, 150);
            bool ok = true;
            for (int i = 0; i < 150; i++)
            {
                int want = i < 60 ? i : 20 + (i - 60) % 40;
                if (got[i] != want) { ok = false; break; }
            }
            Check("loop points 20..60: 0..59, then 20..59 round and round", ok);
            // No loop points: the crossfade's body is shorter by 250 ms and joins without a jump.
            var flat = new short[4000];
            for (int i = 0; i < flat.Length; i++) flat[i] = 1000;
            var cf = new Looper(new Track { Samples = flat, Channels = 1, Rate = 1000 });
            var cfOut = new short[12000];
            cf.Fill(cfOut, 12000);
            Check("without loop points: an equal-power crossfade of 250 ms, no sample far off", cfOut.All(x => x >= 990 && x <= 1420) && cf.Wraps >= 3, "min " + cfOut.Min() + " max " + cfOut.Max() + " wraps " + cf.Wraps);
            // The gain: (volume/100)^2, the fade linear over its time.
            var g = new Gain { Volume = 50, Fade = 0 };
            g.FadeTo(1, 2.5);
            var ones = new short[1000];
            for (int i = 0; i < ones.Length; i++) ones[i] = 10000;
            g.Apply(ones, 1000, 1, 1000);
            Check("after 1 s of a 2.5 s fade-in at volume 50 the gain is 0.25 x 0.4", Math.Abs(ones[999] - 10000 * 0.25 * 0.4) <= 2 && Math.Abs(g.Fade - 0.4) < 0.002, ones[999] + " fade " + g.Fade);
            g.Muted = true;
            for (int i = 0; i < ones.Length; i++) ones[i] = 10000;
            g.Apply(ones, 1000, 1, 1000);
            Check("muted is silence", ones.All(x => x == 0));
            g.Muted = false; g.Volume = 100; g.Fade = 1; g.FadeTo(0, 0.3);
            for (int i = 0; i < ones.Length; i++) ones[i] = 10000;
            g.Apply(ones, 1000, 1, 1000);
            Check("a fade-out over 0.3 s reaches silence and stays there", ones[299] <= 40 && ones.Skip(300).All(x => x == 0) && ones[0] > 9900, ones[0] + " " + ones[299]);
            var duck = new Gain { Volume = 100, Fade = 1, Target = 1 };
            duck.DuckTo(Music.DuckLevel, 0.3);
            for (int i = 0; i < ones.Length; i++) ones[i] = 10000;
            duck.Apply(ones, 1000, 1, 1000);
            bool down = ones[0] > 9900 && Math.Abs(ones[149] - 7500) <= 40 && ones.Skip(300).All(x => x == 5000);
            duck.DuckTo(1, 0.3);
            for (int i = 0; i < ones.Length; i++) ones[i] = 10000;
            duck.Apply(ones, 1000, 1, 1000);
            Check("behind another window the music steps back to half over 0.3 s, and up again", down && ones.Skip(300).All(x => x == 10000), ones[0] + " " + ones[999]);
            long ls, le, n;
            Check("the music tool's line is read", TrackFile.ParseResult("decoding\nok channels=2 rate=44100 samples=1366238 loop=0,1366238\n", out ls, out le, out n) && ls == 0 && le == 1366238 && n == 1366238);
            Check("loop=none means no loop points", TrackFile.ParseResult("ok channels=2 rate=44100 samples=9 loop=none", out ls, out le, out n) && le == 0 && n == 9);
            Check("a failure line is not 'ok'", !TrackFile.ParseResult("error: SND0.AT3 is missing", out ls, out le, out n));
            var wav = TrackFile.ReadWav(TrackFile.WriteWav(t));
            Check("WAV round trip", wav.Channels == 2 && wav.Rate == rate && wav.Samples.SequenceEqual(tone));
            // The real device, at volume 0 (nothing is heard): a 0.1 s loop must come round while it plays.
            var shortTone = new Track { Samples = tone.Take(800 * 2).ToArray(), Channels = 2, Rate = rate, LoopStart = 0, LoopEnd = 800 };
            using (var player = new Player(shortTone))
            {
                if (!player.Start(0, false, 0)) Console.WriteLine("SKIP waveOut: no audio device could be opened");
                else
                {
                    System.Threading.Thread.Sleep(700);
                    Check("waveOut plays at volume 0, the loop comes round (" + player.Wraps + " times in 0.7 s)", !player.Failed && player.Wraps >= 3);
                }
            }
        }

        static void Write(string file, string text) { Directory.CreateDirectory(Path.GetDirectoryName(file)); File.WriteAllText(file, text); }
        static void Write(string file, byte[] bytes) { Directory.CreateDirectory(Path.GetDirectoryName(file)); File.WriteAllBytes(file, bytes); }

        static void Payloads(string root)
        {
            // A made-up staging folder of 30 files: two the same (one blob), one empty, the two settings files.
            string stage = Path.Combine(root, "stage");
            var rng = new Random(11);
            Func<int, byte[]> bytes = n => { var b = new byte[n]; rng.NextBytes(b); for (int i = 0; i < n; i += 3) b[i] = 0; return b; };
            Write(Path.Combine(stage, "ChiliConCarnage.exe"), bytes(200000));
            Write(Path.Combine(stage, "ChiliConCarnageLauncher.exe"), bytes(50000));
            var dll = bytes(30000);
            Write(Path.Combine(stage, "avcodec-61.dll"), dll);
            Write(Path.Combine(stage, "msvcp140.dll"), bytes(9000));
            Write(Path.Combine(stage, "README.txt"), "the old readme");
            Write(Path.Combine(stage, "OLD_NOTES.txt"), "renamed in the next version");
            Write(Path.Combine(stage, "empty.txt"), new byte[0]);
            Write(Path.Combine(stage, "ChiliNative.ini"), DefaultIni);
            Write(Path.Combine(stage, "launcher.ini"), "; Chili Con Carnage launcher.\r\n\r\n[Launcher]\r\nGame=ChiliConCarnage.exe\r\n");
            Write(Path.Combine(stage, "mods", "README.md"), "mods");
            Write(Path.Combine(stage, "mods", "TEXTURES.md"), "textures");
            Write(Path.Combine(stage, "Chili Con Carnage settings.lnk"), "made by package.ps1");
            Write(Path.Combine(stage, "WHAT_CHANGED.txt"), "notes");
            for (int i = 0; i < 12; i++) Write(Path.Combine(stage, "tools", "tool" + i + ".py"), "print(" + i + ")");
            string hd = Path.Combine(root, "hd_loading");
            Write(Path.Combine(hd, "README.md"), "the pack");
            for (int i = 0; i < 5; i++) Write(Path.Combine(hd, "textures", (0x1000000000000000L + i).ToString("x16") + ".png"), bytes(4000 + i));
            string setup = Path.Combine(root, "setupfiles");
            Write(Path.Combine(setup, "ChiliSoundExport.exe"), bytes(3000));
            Write(Path.Combine(setup, "avcodec-61.dll"), dll);
            string exe = Path.Combine(root, "TestSetup.exe");
            var input = new Packer.Input { Package = stage, HdLoading = hd, SetupFiles = setup, Out = exe, Version = "9.8.7", Jobs = 2 };
            int code = Packer.Run(input, TextWriter.Null);
            Check("a made-up payload is packed", code == 0 && File.Exists(exe));
            var p = Payload.Open(exe);
            Check("its manifest is read back", p != null && p.Version == "9.8.7" && p.MusicTool == "setup/ChiliSoundExport.exe");
            Check("the settings files are templates, the notes and the old shortcut are left out", p.Files.Any(f => f.Path == "template/ChiliNative.ini") && p.Files.Any(f => f.Path == "template/launcher.ini") &&
                !p.Files.Any(f => f.Path.EndsWith(".lnk") || f.Path == "WHAT_CHANGED.txt" || f.Path == "ChiliNative.ini"));
            Check("the same bytes share one blob", p.Files.First(f => f.Path == "avcodec-61.dll").Blob == p.Files.First(f => f.Path == "setup/avcodec-61.dll").Blob);
            Check("an empty file has no blob", p.Files.First(f => f.Path == "empty.txt").Blob == -1);
            bool allSame = true;
            foreach (var f in p.Files)
            {
                string source = f.Part == "hd_loading" ? Path.Combine(hd, f.Path.Substring(16).Replace('/', '\\')) : f.Part == "setup" ? Path.Combine(setup, f.Path.Substring(6)) :
                    f.Part == "template" ? Path.Combine(stage, f.Path.Substring(9)) : f.Path == "licenses/OFL-fonts.txt" ? Path.Combine(Env.Temp, "OFL-fonts.txt") : Path.Combine(stage, f.Path.Replace('/', '\\'));
                if (!p.Read(f).SequenceEqual(File.ReadAllBytes(source))) { allSame = false; Console.WriteLine("  differs: " + f.Path); }
            }
            Check("every file comes back byte for byte", allSame);
            // Damage: one byte of a blob changed.
            string bad = Path.Combine(root, "Damaged.exe");
            var whole = File.ReadAllBytes(exe);
            whole[p.Start + p.Blobs[0].Offset + 10] ^= 0x55;
            File.WriteAllBytes(bad, whole);
            bool caught = false;
            try { var q = Payload.Open(bad); foreach (var f in q.Files) q.Read(f); } catch (DamagedException) { caught = true; }
            Check("a changed byte is found", caught);
            whole = File.ReadAllBytes(exe);
            whole[whole.Length - 20] ^= 1;
            File.WriteAllBytes(bad, whole);
            caught = false;
            try { Payload.Open(bad); } catch (DamagedException) { caught = true; }
            Check("a changed trailer is found", caught);
            // What a release must not hold is refused.
            var problems = new List<string>();
            Write(Path.Combine(stage, "saves", "PSP", "x.bin"), "made up");
            Write(Path.Combine(stage, "Chili.iso"), "made up");
            Write(Path.Combine(stage, "mods", "hd_hud", "textures", "0000000000000001.png"), "made up");
            Packer.Plan(new Packer.Input { Package = stage }, problems, new List<string>());
            Check("a save folder, a disc image and another pack are refused", problems.Count == 3, string.Join(" / ", problems));
            Directory.Delete(Path.Combine(stage, "saves"), true); File.Delete(Path.Combine(stage, "Chili.iso")); Directory.Delete(Path.Combine(stage, "mods", "hd_hud"), true);
            problems.Clear();
            Write(Path.Combine(stage, "ChiliNative.ini"), IniEdit.SetDiscPath(DefaultIni, @"C:\mine.iso"));
            Write(Path.Combine(stage, "launcher.ini"), "[Launcher]\r\n[GameProbe]\r\nStamp=1\r\n");
            Packer.Plan(new Packer.Input { Package = stage }, problems, new List<string>());
            Check("a live [Disc] Path and a [GameProbe] section are refused", problems.Any(x => x.Contains("[Disc] Path")) && problems.Any(x => x.Contains("[GameProbe]")), string.Join(" / ", problems));

            Installs(root, p, exe);
        }

        static void Installs(string root, Payload p, string exe)
        {
            // Installs from the made-up payload into the test home. Shortcuts and the Apps entry are the test's own.
            string dir = Path.Combine(Env.LocalAppData, "Programs", Env.FolderName);
            string disc = Path.Combine(root, "discs", "right_id.iso");
            var r = new Installer(p, new InstallOptions { Dir = dir, Disc = disc }).Run();
            Check("install: done", r.Code == Exit.Done, r.Code + " " + r.Message);
            var record = InstallRecord.Load(dir);
            Check("install: every file is there with its sha256", record != null && record.Files.Where(f => f.Part != "setup-made").All(f => File.Exists(Path.Combine(dir, f.Path)) && Util.Sha256File(Path.Combine(dir, f.Path)) == f.Sha256));
            Check("install: the templates, the notes and the music tool are not installed", !Directory.Exists(Path.Combine(dir, "template")) && !Directory.Exists(Path.Combine(dir, "setup")) && !File.Exists(Path.Combine(dir, "WHAT_CHANGED.txt")));
            string ini = File.ReadAllText(Path.Combine(dir, "ChiliNative.ini"));
            Check("install: ChiliNative.ini has the disc and nothing else changed", ini == IniEdit.SetDiscPath(DefaultIni, disc), ini);
            Check("install: launcher.ini, Uninstall.exe, the settings shortcut, logs\\setup.log", File.Exists(Path.Combine(dir, "launcher.ini")) && File.Exists(Path.Combine(dir, "Uninstall.exe")) &&
                File.Exists(Path.Combine(dir, "Chili Con Carnage settings.lnk")) && File.Exists(Path.Combine(dir, "logs", "setup.log")));
            Check("install: Uninstall.exe is this program without the payload", new FileInfo(Path.Combine(dir, "Uninstall.exe")).Length == Payload.StubLength(exe) && Payload.Open(Path.Combine(dir, "Uninstall.exe")) == null);
            Check("install: the Start menu and desktop shortcuts", File.Exists(Path.Combine(Env.StartMenuFolder, "Chili Con Carnage.lnk")) && File.Exists(Path.Combine(Env.StartMenuFolder, "Chili Con Carnage settings.lnk")) && File.Exists(Path.Combine(Env.Desktop, "Chili Con Carnage.lnk")));
            using (var k = Registry.CurrentUser.OpenSubKey(Env.UninstallKeyRoot + "ChiliConCarnagePC-Test"))
                Check("install: the Apps entry (test name)", k != null && (string)k.GetValue("UninstallString") == "\"" + Path.Combine(dir, "Uninstall.exe") + "\"" && (string)k.GetValue("DisplayName") == "Chili Con Carnage PC (test)");
            Check("install: the pack is there", Directory.GetFiles(Path.Combine(dir, "mods", "hd_loading", "textures")).Length == 5);
            Check("install: no stage folder left", !Directory.Exists(Path.Combine(dir, ".setup-stage")));

            // The player's own files, then an update without the pack and with a renamed document.
            Write(Path.Combine(dir, "saves", "PSP", "SAVEDATA", "ULUS10216DATA00", "made_up.txt"), "a made-up save");
            Write(Path.Combine(dir, "saves", "PSP", "SAVEDATA", "ULUS10216DATA00", "PARAM.SFO"), new byte[16]);
            Write(Path.Combine(dir, "art", "logo.png"), new byte[] { 0x89, 0x50, 0x4E, 0x47 });
            Write(Path.Combine(dir, "mods", "my_pack", "textures", "0000000000000001.png"), new byte[] { 1 });
            File.WriteAllText(Path.Combine(dir, "ChiliNative.ini"), ini.Replace("BufferMs=80", "BufferMs=120"));
            var mine = new[] { @"saves\PSP\SAVEDATA\ULUS10216DATA00\made_up.txt", @"saves\PSP\SAVEDATA\ULUS10216DATA00\PARAM.SFO", @"art\logo.png", @"mods\my_pack\textures\0000000000000001.png", "ChiliNative.ini" };
            var before = mine.Select(f => Util.Sha256File(Path.Combine(dir, f))).ToList();
            // The next version: OLD_NOTES.txt is gone from it.
            var next = Payload.Open(exe);
            next.Files.RemoveAll(f => f.Path == "OLD_NOTES.txt");
            r = new Installer(next, new InstallOptions { Dir = dir, Disc = disc, HdLoading = false, Desktop = false }).Run();
            Check("update: done", r.Code == Exit.Done && r.Update, r.Code + " " + r.Message);
            Check("update: the save, the art, the player's pack and the changed settings are byte for byte the same", mine.Select(f => Util.Sha256File(Path.Combine(dir, f))).SequenceEqual(before));
            Check("update: the renamed document is removed", !File.Exists(Path.Combine(dir, "OLD_NOTES.txt")));
            Check("update: the pack, not chosen this time, is left as it was", Directory.GetFiles(Path.Combine(dir, "mods", "hd_loading", "textures")).Length == 5);
            Check("update: the desktop shortcut not wanted this time is removed", !File.Exists(Path.Combine(Env.Desktop, "Chili Con Carnage.lnk")));
            Check("update: the desktop folder itself stays, even empty", Directory.Exists(Env.Desktop));
            record = InstallRecord.Load(dir);
            Check("update: the record still names the pack's files, so that the uninstaller knows them", record.Files.Count(f => f.Part == "hd_loading") == 6 && !record.HdLoading);

            // A full disk while the stage is written: the earlier installation is untouched.
            var hashes = Directory.GetFiles(dir, "*", SearchOption.AllDirectories).OrderBy(x => x).Select(x => x + Util.Sha256File(x)).ToList();
            var failing = new Installer(p, new InstallOptions { Dir = dir, Disc = disc }) { FailAfterStage = "made-up failure" };
            r = failing.Run();
            Check("a failure before the commit leaves the earlier installation as it was", r.Code != Exit.Done && Directory.GetFiles(dir, "*", SearchOption.AllDirectories).OrderBy(x => x).Select(x => x + Util.Sha256File(x)).SequenceEqual(hashes), r.Code + " " + r.Message);
            var cancelled = new Installer(p, new InstallOptions { Dir = Path.Combine(root, "cancelled", Env.FolderName), Disc = disc }) { Cancel = true }.Run();
            Check("a cancel of a fresh install leaves no folder", cancelled.Code == Exit.Cancelled && !Directory.Exists(Path.Combine(root, "cancelled", Env.FolderName)));

            // Uninstall, keeping the player's files.
            var u = Uninstaller.Run(dir, false);
            var left = Directory.GetFileSystemEntries(dir, "*", SearchOption.AllDirectories).Select(x => x.Substring(dir.Length + 1)).OrderBy(x => x).ToList();
            var expected = new[] { "art", @"art\logo.png", "ChiliNative.ini", "launcher.ini", "logs", @"logs\setup.log", "mods", @"mods\my_pack", @"mods\my_pack\textures", @"mods\my_pack\textures\0000000000000001.png",
                "saves", @"saves\PSP", @"saves\PSP\SAVEDATA", @"saves\PSP\SAVEDATA\ULUS10216DATA00", @"saves\PSP\SAVEDATA\ULUS10216DATA00\made_up.txt", @"saves\PSP\SAVEDATA\ULUS10216DATA00\PARAM.SFO" };
            Check("uninstall: only the player's files are left", u.Code == Exit.Done && left.SequenceEqual(expected.OrderBy(x => x), StringComparer.OrdinalIgnoreCase), string.Join(", ", left));
            Check("uninstall: the save is byte for byte the same", Util.Sha256File(Path.Combine(dir, mine[0])) == before[0]);
            Check("uninstall: the shortcuts and the Apps entry are gone", !Directory.Exists(Env.StartMenuFolder) && Registry.CurrentUser.OpenSubKey(Env.UninstallKeyRoot + "ChiliConCarnagePC-Test") == null);

            // Install again, then remove everything; a disc image in the folder stays.
            r = new Installer(p, new InstallOptions { Dir = dir, Disc = disc }).Run();
            Write(Path.Combine(dir, "my copy.iso"), new byte[] { 1 });
            u = Uninstaller.Run(dir, true);
            left = Directory.GetFileSystemEntries(dir, "*", SearchOption.AllDirectories).Select(x => x.Substring(dir.Length + 1)).OrderBy(x => x).ToList();
            Check("uninstall with saves: everything but a disc image goes", r.Code == Exit.Done && u.Code == Exit.Done && left.SequenceEqual(new[] { "my copy.iso" }, StringComparer.OrdinalIgnoreCase), string.Join(", ", left));
        }

        // Every text of every page: one line each, no word of the forbidden list, no explanation in brackets.
        static void Texts()
        {
            Faces.Load();
            var all = new List<string>();
            SetupView.Overflow = new List<string>();
            foreach (var s in Shots.All(null)) Shots.Render(s, 1, all);
            var cut = SetupView.Overflow.Distinct().ToList();
            SetupView.Overflow = null;
            var bad = all.Where(t => t.Contains("\n") || Regex.IsMatch(t, @"\b(emulator|emulation|port|ported|wizard|click here|simply|please)\b", RegexOptions.IgnoreCase) ||
                (t.Contains("(") && !t.Contains("(USA"))).Distinct().ToList();
            Check("every text is one line, with no forbidden word and no bracketed explanation (" + all.Distinct().Count() + " texts)", bad.Count == 0, string.Join(" | ", bad));
            Check("every instruction and verdict line fits its width, nothing is cut short", cut.Count == 0, string.Join(" | ", cut));
            Check("the refusals a player can meet fit too", new[] { Disc.TextOtherProgram, Disc.TextCso, Disc.TextNotIso, Disc.TextNotPspFile, Disc.TextNotPspFolder, Disc.TextWrong, "This is the disc ULES-00629, not Chili Con Carnage (USA, ULUS-10216).",
                Folder.TextNotEmpty, Folder.TextNotWritable, Folder.TextNoSpace, Folder.TextDrive, Folder.TextUpdate }.All(x => ChiliLauncher.Tx.Width(x, Faces.Label, 16) <= 674 - 28));
            Check("the uninstaller's switch is off by default", !new SetupModel().RemoveAll);
            Check("the HD loading pictures are on by default", new SetupModel().HdLoading && new InstallOptions().HdLoading);
        }
    }
}
