// The music: one track of the game's own, read from the player's disc once it is accepted (nothing of the
// game's audio is in Setup), played looped at the track's own loop points through waveOut, starting softly.
// Volume and mute are remembered. Any failure means silence and one line in the log.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;

namespace ChiliSetup
{
    /// <summary>16-bit PCM in memory with its loop: the end is exclusive; LoopEnd 0 means "no loop points".</summary>
    public sealed class Track
    {
        public short[] Samples;          // interleaved
        public int Channels = 2, Rate = 44100;
        public long LoopStart, LoopEnd;
        public long Frames { get { return Samples.Length / Channels; } }
    }

    public static class TrackFile
    {
        /// <summary>A 16-bit PCM WAV (RIFF/WAVE, "fmt " and "data" chunks).</summary>
        public static Track ReadWav(byte[] b)
        {
            if (b.Length < 44 || Encoding.ASCII.GetString(b, 0, 4) != "RIFF" || Encoding.ASCII.GetString(b, 8, 4) != "WAVE") throw new InvalidDataException("not a RIFF/WAVE file");
            int channels = 0, rate = 0, bits = 0, format = 0;
            int p = 12;
            while (p + 8 <= b.Length)
            {
                string id = Encoding.ASCII.GetString(b, p, 4);
                int size = BitConverter.ToInt32(b, p + 4);
                int body = p + 8;
                if (size < 0 || body + size > b.Length) size = b.Length - body;
                if (id == "fmt ")
                {
                    format = BitConverter.ToUInt16(b, body);
                    channels = BitConverter.ToUInt16(b, body + 2);
                    rate = BitConverter.ToInt32(b, body + 4);
                    bits = BitConverter.ToUInt16(b, body + 14);
                    if (format == 0xFFFE && size >= 40) format = BitConverter.ToUInt16(b, body + 24);
                }
                else if (id == "data")
                {
                    if (format != 1 || bits != 16 || channels < 1 || channels > 8 || rate < 8000) throw new InvalidDataException("not 16-bit PCM (format " + format + ", " + bits + " bits)");
                    int frames = size / (2 * channels);
                    var samples = new short[frames * channels];
                    Buffer.BlockCopy(b, body, samples, 0, frames * channels * 2);
                    return new Track { Samples = samples, Channels = channels, Rate = rate };
                }
                p = body + size + (size & 1);
            }
            throw new InvalidDataException("no data chunk");
        }

        public static byte[] WriteWav(Track t)
        {
            using (var m = new MemoryStream())
            using (var w = new BinaryWriter(m))
            {
                int data = t.Samples.Length * 2;
                w.Write(Encoding.ASCII.GetBytes("RIFF")); w.Write(36 + data); w.Write(Encoding.ASCII.GetBytes("WAVE"));
                w.Write(Encoding.ASCII.GetBytes("fmt ")); w.Write(16); w.Write((ushort)1); w.Write((ushort)t.Channels); w.Write(t.Rate);
                w.Write(t.Rate * t.Channels * 2); w.Write((ushort)(t.Channels * 2)); w.Write((ushort)16);
                w.Write(Encoding.ASCII.GetBytes("data")); w.Write(data);
                var bytes = new byte[data];
                Buffer.BlockCopy(t.Samples, 0, bytes, 0, data);
                w.Write(bytes);
                return m.ToArray();
            }
        }

        /// <summary>The music tool's last line: "ok channels=2 rate=44100 samples=1366238 loop=0,1366238" (or loop=none).</summary>
        public static bool ParseResult(string stdout, out long loopStart, out long loopEnd, out long samples)
        {
            loopStart = loopEnd = samples = 0;
            if (stdout == null) return false;
            Match found = null;
            foreach (var line in stdout.Split('\n'))
            {
                var m = Regex.Match(line.Trim(), @"^ok\b.*$");
                if (m.Success) found = m;
            }
            if (found == null) return false;
            var s = Regex.Match(found.Value, @"\bsamples=(\d+)");
            if (s.Success) samples = long.Parse(s.Groups[1].Value, CultureInfo.InvariantCulture);
            var l = Regex.Match(found.Value, @"\bloop=(\d+),(\d+)");
            if (l.Success)
            {
                loopStart = long.Parse(l.Groups[1].Value, CultureInfo.InvariantCulture);
                loopEnd = long.Parse(l.Groups[2].Value, CultureInfo.InvariantCulture);
            }
            return true;
        }
    }

    /// <summary>
    /// The loop, sample-exact: plays the track from its start; at the loop end it goes on at the loop start
    /// inside the same buffer. A track without loop points (or with unusable ones) is made to loop by an
    /// equal-power crossfade of its last 250 ms into its first: the first time through it plays as it is up to
    /// that point, then a body whose first 250 ms are the crossfade, round and round.
    /// </summary>
    public sealed class Looper
    {
        readonly int channels;
        readonly short[] intro, body;
        readonly long introEnd, bodyStart, bodyEnd;
        bool inBody;
        long pos;
        public int Wraps;

        public Looper(Track t)
        {
            channels = t.Channels;
            long frames = t.Frames;
            intro = t.Samples;
            if (t.LoopEnd > t.LoopStart && t.LoopEnd <= frames && t.LoopEnd - t.LoopStart >= t.Rate / 10)
            {
                body = t.Samples; introEnd = t.LoopEnd; bodyStart = t.LoopStart; bodyEnd = t.LoopEnd;
            }
            else
            {
                long fade = Math.Min(t.Rate / 4, frames / 4);
                long length = frames - fade;
                body = new short[length * channels];
                Array.Copy(t.Samples, body, body.Length);
                for (long i = 0; i < fade; i++)
                {
                    double x = (i + 0.5) / fade;
                    double gin = Math.Sin(x * Math.PI / 2), gout = Math.Cos(x * Math.PI / 2);
                    for (int c = 0; c < channels; c++)
                    {
                        double v = t.Samples[i * channels + c] * gin + t.Samples[(length + i) * channels + c] * gout;
                        body[i * channels + c] = (short)Math.Max(-32768, Math.Min(32767, Math.Round(v)));
                    }
                }
                introEnd = length; bodyStart = 0; bodyEnd = length;
            }
        }

        /// <summary>The next `frames` frames into `outBuffer` (interleaved), unscaled.</summary>
        public void Fill(short[] outBuffer, int frames)
        {
            int o = 0;
            while (frames > 0)
            {
                long end = inBody ? bodyEnd : introEnd;
                var src = inBody ? body : intro;
                long n = Math.Min(frames, end - pos);
                if (n <= 0)
                {
                    inBody = true;
                    pos = bodyStart;
                    Wraps++;
                    continue;
                }
                Array.Copy(src, pos * channels, outBuffer, o, n * channels);
                o += (int)(n * channels);
                pos += n;
                frames -= (int)n;
            }
        }
    }

    /// <summary>The fade and volume: gain = (volume/100)^2 x fade, the fade moving linearly towards its target.</summary>
    public sealed class Gain
    {
        public double Fade, Target, PerSecond = 1;
        public int Volume = 60;
        public bool Muted;
        readonly object gate = new object();

        public void FadeTo(double target, double seconds)
        {
            lock (gate) { Target = target; PerSecond = seconds <= 0 ? 1e9 : Math.Abs(target - Fade) / seconds; if (PerSecond == 0) PerSecond = 1e9; }
        }
        /// <summary>Scales a buffer in place, moving the fade by its length.</summary>
        public void Apply(short[] buffer, int frames, int channels, int rate)
        {
            double fade, target, step;
            int volume;
            lock (gate) { fade = Fade; target = Target; step = PerSecond / rate; volume = Muted ? 0 : Volume; }
            double v = volume / 100.0;
            v *= v;
            for (int i = 0; i < frames; i++)
            {
                if (fade < target) fade = Math.Min(target, fade + step);
                else if (fade > target) fade = Math.Max(target, fade - step);
                double g = v * fade;
                for (int c = 0; c < channels; c++)
                {
                    int k = i * channels + c;
                    double s = buffer[k] * g;
                    buffer[k] = (short)(s > 32767 ? 32767 : s < -32768 ? -32768 : Math.Round(s));
                }
            }
            lock (gate) Fade = fade;
        }
    }

    /// <summary>waveOut: 8 buffers of 50 ms fed by a thread of its own.</summary>
    public sealed class Player : IDisposable
    {
        [StructLayout(LayoutKind.Sequential)]
        struct WaveFormat { public ushort Tag, Channels; public int Rate, BytesPerSecond; public ushort Align, Bits, Extra; }
        [StructLayout(LayoutKind.Sequential)]
        struct WaveHeader { public IntPtr Data; public uint Length, Recorded; public IntPtr User; public uint Flags, Loops; public IntPtr Next, Reserved; }
        [DllImport("winmm.dll")] static extern int waveOutOpen(out IntPtr device, uint id, ref WaveFormat format, IntPtr callback, IntPtr instance, uint flags);
        [DllImport("winmm.dll")] static extern int waveOutPrepareHeader(IntPtr device, IntPtr header, uint size);
        [DllImport("winmm.dll")] static extern int waveOutUnprepareHeader(IntPtr device, IntPtr header, uint size);
        [DllImport("winmm.dll")] static extern int waveOutWrite(IntPtr device, IntPtr header, uint size);
        [DllImport("winmm.dll")] static extern int waveOutReset(IntPtr device);
        [DllImport("winmm.dll")] static extern int waveOutClose(IntPtr device);
        const uint WaveMapper = 0xFFFFFFFF, CallbackEvent = 0x50000, Done = 1;
        const int Buffers = 8;

        readonly Track track;
        readonly Looper looper;
        public readonly Gain Gain = new Gain();
        IntPtr device;
        readonly IntPtr[] headers = new IntPtr[Buffers], data = new IntPtr[Buffers];
        readonly AutoResetEvent wake = new AutoResetEvent(false);
        Thread feeder;
        volatile bool stop;
        int frames;
        readonly int headerSize = Marshal.SizeOf(typeof(WaveHeader));
        readonly int flagsAt = (int)Marshal.OffsetOf(typeof(WaveHeader), "Flags");
        public bool Failed;
        public int Wraps { get { return looper.Wraps; } }
        /// <summary>
        /// Tests: no device; the feeder runs as if one took its buffers, `NullSpeed` times faster than real time,
        /// so that a long track reaches its loop point within a test.
        /// </summary>
        public static bool NullAudio;
        public static int NullSpeed = 10;

        public Player(Track t) { track = t; looper = new Looper(t); }

        /// <summary>Opens the device and starts; false (and silence) when Windows has no device for it.</summary>
        public bool Start(int volume, bool muted, double fadeInSeconds)
        {
            Gain.Volume = volume; Gain.Muted = muted; Gain.Fade = 0;
            Gain.FadeTo(1, fadeInSeconds);
            frames = track.Rate / 20;
            if (NullAudio)
            {
                feeder = new Thread(FeedNothing) { IsBackground = true, Name = "music (no device)" };
                feeder.Start();
                return true;
            }
            var format = new WaveFormat { Tag = 1, Channels = (ushort)track.Channels, Rate = track.Rate, Bits = 16, Align = (ushort)(track.Channels * 2), BytesPerSecond = track.Rate * track.Channels * 2 };
            int r = waveOutOpen(out device, WaveMapper, ref format, wake.SafeWaitHandle.DangerousGetHandle(), IntPtr.Zero, CallbackEvent);
            if (r != 0) { Log.Line("music: waveOutOpen failed (" + r + "), no sound"); Failed = true; return false; }
            frames = track.Rate / 20;
            for (int i = 0; i < Buffers; i++)
            {
                data[i] = Marshal.AllocHGlobal(frames * track.Channels * 2);
                headers[i] = Marshal.AllocHGlobal(headerSize);
                var h = new WaveHeader { Data = data[i], Length = (uint)(frames * track.Channels * 2) };
                Marshal.StructureToPtr(h, headers[i], false);
                waveOutPrepareHeader(device, headers[i], (uint)headerSize);
                Marshal.WriteInt32(headers[i], flagsAt, Marshal.ReadInt32(headers[i], flagsAt) | (int)Done);
            }
            feeder = new Thread(Feed) { IsBackground = true, Name = "music", Priority = ThreadPriority.AboveNormal };
            feeder.Start();
            return true;
        }

        void FeedNothing()
        {
            var buffer = new short[frames * track.Channels];
            while (!stop)
            {
                for (int i = 0; i < NullSpeed && !stop; i++)
                {
                    looper.Fill(buffer, frames);
                    Gain.Apply(buffer, frames, track.Channels, track.Rate);
                }
                wake.WaitOne(50);
            }
        }

        void Feed()
        {
            var buffer = new short[frames * track.Channels];
            try
            {
                while (!stop)
                {
                    bool any = false;
                    for (int i = 0; i < Buffers && !stop; i++)
                    {
                        if ((Marshal.ReadInt32(headers[i], flagsAt) & Done) == 0) continue;
                        looper.Fill(buffer, frames);
                        Gain.Apply(buffer, frames, track.Channels, track.Rate);
                        Marshal.Copy(buffer, 0, data[i], buffer.Length);
                        Marshal.WriteInt32(headers[i], flagsAt, Marshal.ReadInt32(headers[i], flagsAt) & ~(int)Done);
                        int r = waveOutWrite(device, headers[i], (uint)headerSize);
                        if (r != 0) { Log.Line("music: waveOutWrite failed (" + r + "), stopped"); Failed = true; return; }
                        any = true;
                    }
                    if (!any) wake.WaitOne(100);
                }
            }
            catch (Exception e) { Failed = true; Log.Line("music: " + e.Message); }
        }

        public void Dispose()
        {
            stop = true;
            if (feeder != null) { wake.Set(); feeder.Join(1000); }
            if (device != IntPtr.Zero)
            {
                waveOutReset(device);
                for (int i = 0; i < Buffers; i++) if (headers[i] != IntPtr.Zero) waveOutUnprepareHeader(device, headers[i], (uint)headerSize);
                waveOutClose(device);
                device = IntPtr.Zero;
            }
            for (int i = 0; i < Buffers; i++)
            {
                if (headers[i] != IntPtr.Zero) Marshal.FreeHGlobal(headers[i]);
                if (data[i] != IntPtr.Zero) Marshal.FreeHGlobal(data[i]);
                headers[i] = data[i] = IntPtr.Zero;
            }
        }
    }

    /// <summary>
    /// The music of the window: export from the disc (the payload's music tool, run hidden, 15 s at most),
    /// then the player. Everything here may fail; failure is silence.
    /// </summary>
    public sealed class Music
    {
        public int Volume = 60;
        public bool Muted;
        Player player;
        string exportedFor;
        int generation;
        public bool Playing { get { return player != null && !player.Failed; } }
        public int Wraps { get { return player != null ? player.Wraps : 0; } }
        public Action Changed = delegate { };

        public Music()
        {
            Volume = Math.Max(0, Math.Min(100, Remembered.Get("Volume", 60)));
            Muted = Remembered.Get("Muted", 0) != 0;
        }

        public void SetVolume(int v)
        {
            v = Math.Max(0, Math.Min(100, v));
            if (v == Volume) return;
            Volume = v;
            if (player != null) player.Gain.Volume = v;
            Remembered.Set("Volume", v);
            Changed();
        }
        public void ToggleMute()
        {
            Muted = !Muted;
            if (player != null)
            {
                // Down over 0.3 s, then muted; up from silence over 0.3 s.
                if (Muted) player.Gain.FadeTo(0, 0.3);
                else { player.Gain.Muted = false; player.Gain.FadeTo(1, 0.3); }
            }
            Remembered.Set("Muted", Muted ? 1 : 0);
            Changed();
        }

        /// <summary>Starts the export for this disc on a worker thread, then plays. A new disc starts again.</summary>
        public void StartFor(Payload payload, string disc)
        {
            if (payload == null || string.IsNullOrEmpty(payload.MusicTool) || disc == null) return;
            if (exportedFor != null && string.Equals(exportedFor, disc, StringComparison.OrdinalIgnoreCase) && player != null) return;
            Stop(0.3);
            exportedFor = disc;
            int mine = ++generation;
            ThreadPool.QueueUserWorkItem(_ =>
            {
                var t = Export(payload, disc);
                if (t == null || mine != generation) return;
                var p = new Player(t);
                lock (this)
                {
                    if (mine != generation) return;
                    if (!p.Start(Volume, Muted, 2.5)) { p.Dispose(); return; }
                    if (Muted) p.Gain.FadeTo(0, 0);
                    player = p;
                }
                Log.Line("music: playing");
                Changed();
            });
        }

        /// <summary>Fades out over `seconds`, then closes the device (on a worker, so the window does not wait).</summary>
        public void Stop(double seconds)
        {
            Player p;
            lock (this) { p = player; player = null; generation++; exportedFor = null; }
            if (p == null) return;
            p.Gain.FadeTo(0, seconds);
            Log.Line("music: " + p.Wraps + " times through the loop point");
            ThreadPool.QueueUserWorkItem(_ => { Thread.Sleep((int)(seconds * 1000) + 120); p.Dispose(); });
            Changed();
        }
        /// <summary>Closing: fade, wait for it, close.</summary>
        public void StopNow(double seconds)
        {
            Player p;
            lock (this) { p = player; player = null; generation++; }
            if (p == null) return;
            p.Gain.FadeTo(0, seconds);
            Log.Line("music: " + p.Wraps + " times through the loop point");
            Thread.Sleep((int)(seconds * 1000) + 60);
            p.Dispose();
        }

        /// <summary>
        /// Runs the payload's music tool on the disc into a WAV in Setup's temp folder, reads it into memory and
        /// deletes it at once. Null on any failure (logged).
        /// </summary>
        public static Track Export(Payload payload, string disc)
        {
            var clock = Stopwatch.StartNew();
            string wav = Path.Combine(Env.Temp, "music_" + Guid.NewGuid().ToString("N").Substring(0, 8) + ".wav");
            try
            {
                string tool = ExtractSetupPart(payload);
                if (tool == null) return null;
                string args = (payload.MusicArgs ?? "\"{disc}\" \"{wav}\"").Replace("{disc}", disc.TrimEnd('\\')).Replace("{wav}", wav);
                var info = new ProcessStartInfo(tool, args)
                {
                    UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true,
                    WorkingDirectory = Path.GetDirectoryName(tool), WindowStyle = ProcessWindowStyle.Hidden,
                };
                info.EnvironmentVariables["CHILI_UNATTENDED"] = "1";
                string stdout, stderr;
                using (var p = Process.Start(info))
                {
                    var outTask = p.StandardOutput.ReadToEndAsync();
                    var errTask = p.StandardError.ReadToEndAsync();
                    if (!p.WaitForExit(15000)) { try { p.Kill(); } catch { } Log.Line("music: the export took more than 15 s and was stopped"); return null; }
                    stdout = outTask.Result; stderr = errTask.Result;
                    if (p.ExitCode != 0) { Log.Line("music: the export failed (exit " + p.ExitCode + "): " + (stderr ?? "").Trim()); return null; }
                }
                long loopStart, loopEnd, samples;
                if (!TrackFile.ParseResult(stdout, out loopStart, out loopEnd, out samples)) { Log.Line("music: the export said no 'ok' line: " + (stdout ?? "").Trim()); return null; }
                var t = TrackFile.ReadWav(File.ReadAllBytes(wav));
                t.LoopStart = loopStart; t.LoopEnd = loopEnd;
                if (t.Frames < t.Rate / 20) { Log.Line("music: the track is too short"); return null; }
                Log.Line(string.Format(CultureInfo.InvariantCulture, "music: exported in {0:0.00} s, {1} channels, {2} Hz, {3:N0} frames, loop {4}",
                    clock.Elapsed.TotalSeconds, t.Channels, t.Rate, t.Frames, loopEnd > loopStart ? loopStart + "-" + loopEnd : "none (crossfade)"));
                return t;
            }
            catch (Exception e) { Log.Line("music: " + e.Message); return null; }
            finally
            {
                // The WAV, and the loop file the tool writes beside it: nothing of the game's audio stays on disk.
                foreach (var f in new[] { wav, Path.ChangeExtension(wav, ".loop.txt"), wav + ".part" })
                    try { if (File.Exists(f)) File.Delete(f); } catch { }
            }
        }

        static readonly object extractGate = new object();
        static string extracted;
        /// <summary>The payload's setup/ files in Setup's temp folder (once); the music tool's path there.</summary>
        static string ExtractSetupPart(Payload payload)
        {
            lock (extractGate)
            {
                string dir = Path.Combine(Env.Temp, "setup");
                if (extracted == null)
                {
                    Directory.CreateDirectory(dir);
                    var p = Payload.Open(payload.Source);
                    foreach (var f in p.Part("setup"))
                    {
                        string target = Path.Combine(dir, f.Path.Substring("setup/".Length).Replace('/', '\\'));
                        Directory.CreateDirectory(Path.GetDirectoryName(target));
                        File.WriteAllBytes(target, p.Read(f));
                    }
                    p.Forget();
                    extracted = dir;
                }
                string tool = Path.Combine(dir, payload.MusicTool.Substring(payload.MusicTool.StartsWith("setup/") ? 6 : 0).Replace('/', '\\'));
                if (!File.Exists(tool)) { Log.Line("music: the tool " + payload.MusicTool + " is not in the payload"); return null; }
                return tool;
            }
        }
    }
}
