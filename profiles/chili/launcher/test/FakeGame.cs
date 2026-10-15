// A stand-in for the game, to test the launcher's PLAY without the real program: it writes what it was
// started with to logs\ChiliConCarnage.log beside itself, waits, and exits. What it does comes from
// fakegame.txt beside it, one "name=value" per line: seconds (default 2), exit (default 0), message.
using System;
using System.Collections.Generic;
using System.IO;
using System.Threading;

static class FakeGame
{
    static int Main(string[] args)
    {
        string dir = AppDomain.CurrentDomain.BaseDirectory;
        var settings = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        string file = Path.Combine(dir, "fakegame.txt");
        if (File.Exists(file))
            foreach (var line in File.ReadAllLines(file))
            {
                int eq = line.IndexOf('=');
                if (eq > 0) settings[line.Substring(0, eq).Trim()] = line.Substring(eq + 1).Trim();
            }
        string value;
        double seconds = settings.TryGetValue("seconds", out value) ? double.Parse(value, System.Globalization.CultureInfo.InvariantCulture) : 2;
        int exit = settings.TryGetValue("exit", out value) ? int.Parse(value) : 0;
        string message = settings.TryGetValue("message", out value) ? value : "the stand-in game was told to fail";
        Directory.CreateDirectory(Path.Combine(dir, "logs"));
        string config = Environment.GetEnvironmentVariable("CHILI_CONFIG");
        string ini = string.IsNullOrEmpty(config) ? Path.Combine(dir, "ChiliNative.ini") : config;
        // A developer's start (arguments given) prints its log, as the real game does; a player's start writes it.
        Console.WriteLine("[start] stand-in game, arguments: " + string.Join(" | ", args));
        Console.Error.WriteLine("[stderr] stand-in game started");
        if (exit != 0) Console.Error.WriteLine("[stop] " + message);
        using (var log = new StreamWriter(Path.Combine(dir, "logs", "ChiliConCarnage.log"), false))
        {
            log.WriteLine("[start] stand-in game, folder " + dir);
            log.WriteLine("[start] arguments: " + string.Join(" | ", args));
            log.WriteLine("[start] CHILI_CONFIG=" + (config ?? "(not set)"));
            log.WriteLine("[start] CHILI_RENDER_BACKEND=" + (Environment.GetEnvironmentVariable("CHILI_RENDER_BACKEND") ?? "(not set)"));
            log.WriteLine("[start] settings file: " + ini + (File.Exists(ini) ? " (" + new FileInfo(ini).Length + " bytes)" : " (missing)"));
            if (File.Exists(ini))
                foreach (var line in File.ReadAllLines(ini))
                {
                    string t = line.Trim();
                    if (t.Length > 0 && t[0] != ';' && t[0] != '#' && (t.StartsWith("Fullscreen", StringComparison.OrdinalIgnoreCase) || t.StartsWith("FrameLock", StringComparison.OrdinalIgnoreCase) || t.StartsWith("Enabled=", StringComparison.OrdinalIgnoreCase) && t.Length > 13))
                        log.WriteLine("[settings] " + t);
                }
            log.Flush();
            Thread.Sleep((int)(seconds * 1000));
            if (exit != 0) log.WriteLine("[stop] " + message);
            else log.WriteLine("[stop] closed by the player");
        }
        return exit;
    }
}
