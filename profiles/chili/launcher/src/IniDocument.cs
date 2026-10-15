// ChiliNative.ini as the player wrote it: every line is kept byte for byte, and a change touches
// only the line of the key that changed.
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace ChiliLauncher
{
    public enum IniLineKind { Blank, Comment, Section, KeyValue, Other }

    public sealed class IniLine
    {
        public string Text;          // without its line end
        public string Eol;           // "\r\n", "\n", "\r" or "" (last line without one)
        public IniLineKind Kind;
        public string Section = "";  // the section the line is in; for a header, the section it opens
        public string Key;           // KeyValue: the key. Comment: the key of a switched-off "; Key=value" line, else null
        public string Value;         // value without the inline comment, trimmed
        public string RawValue;      // everything after '=', trimmed (paths keep ';' and '#')
        public int ValueStart;       // where the value starts in Text (KeyValue only)
        public int ValueLength;
    }

    public sealed class IniDocument
    {
        public readonly List<IniLine> Lines = new List<IniLine>();
        public bool HasBom;
        public bool Utf8 = true;
        public string NewLine = "\r\n";

        static readonly Encoding StrictUtf8 = new UTF8Encoding(false, true);
        static readonly Encoding Latin1 = Encoding.GetEncoding(28591);

        public static IniDocument Load(string path)
        {
            return File.Exists(path) ? Parse(File.ReadAllBytes(path)) : new IniDocument();
        }

        public static IniDocument Parse(byte[] bytes)
        {
            var doc = new IniDocument();
            int start = 0;
            if (bytes.Length >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) { doc.HasBom = true; start = 3; }
            string text;
            try { text = StrictUtf8.GetString(bytes, start, bytes.Length - start); }
            catch (DecoderFallbackException) { doc.Utf8 = false; text = Latin1.GetString(bytes, start, bytes.Length - start); }
            doc.ParseText(text);
            return doc;
        }

        public static IniDocument FromText(string text)
        {
            var doc = new IniDocument();
            doc.ParseText(text);
            return doc;
        }

        void ParseText(string text)
        {
            int crlf = 0, lf = 0;
            int pos = 0;
            string section = "";
            while (pos < text.Length)
            {
                int end = pos;
                while (end < text.Length && text[end] != '\n' && text[end] != '\r') end++;
                string eol = "";
                if (end < text.Length)
                {
                    if (text[end] == '\r' && end + 1 < text.Length && text[end + 1] == '\n') { eol = "\r\n"; crlf++; }
                    else if (text[end] == '\n') { eol = "\n"; lf++; }
                    else eol = "\r";
                }
                var line = ParseLine(text.Substring(pos, end - pos), section);
                line.Eol = eol;
                if (line.Kind == IniLineKind.Section) section = line.Section;
                Lines.Add(line);
                pos = end + eol.Length;
            }
            NewLine = lf > crlf ? "\n" : "\r\n";
            if (crlf == 0 && lf == 0 && Lines.Count > 0) NewLine = "\r\n";
        }

        static bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r'; }

        static bool IsKeyName(string s)
        {
            if (s.Length == 0) return false;
            foreach (char c in s)
                if (!(char.IsLetterOrDigit(c) || c == '_' || c == '.')) return false;
            return true;
        }

        static IniLine ParseLine(string text, string section)
        {
            var line = new IniLine { Text = text, Section = section };
            int first = 0;
            while (first < text.Length && IsSpace(text[first])) first++;
            if (first == text.Length) { line.Kind = IniLineKind.Blank; return line; }
            if (text[first] == ';' || text[first] == '#')
            {
                line.Kind = IniLineKind.Comment;
                int p = first;
                while (p < text.Length && (text[p] == ';' || text[p] == '#')) p++;
                while (p < text.Length && IsSpace(text[p])) p++;
                int eq = text.IndexOf('=', p);
                if (eq > p)
                {
                    string key = text.Substring(p, eq - p).Trim();
                    if (IsKeyName(key))
                    {
                        line.Key = key;
                        line.RawValue = text.Substring(eq + 1).Trim();
                        line.Value = StripComment(text.Substring(eq + 1)).Trim();
                    }
                }
                return line;
            }
            int comment = text.IndexOfAny(new[] { ';', '#' }, first);
            int bodyEnd = comment < 0 ? text.Length : comment;
            while (bodyEnd > first && IsSpace(text[bodyEnd - 1])) bodyEnd--;
            if (text[first] == '[')
            {
                line.Kind = IniLineKind.Section;
                line.Section = bodyEnd - first >= 2 && text[bodyEnd - 1] == ']' ? text.Substring(first + 1, bodyEnd - first - 2).Trim() : "";
                return line;
            }
            int equals = text.IndexOf('=', first);
            if (equals < 0 || equals >= bodyEnd) { line.Kind = IniLineKind.Other; return line; }
            line.Kind = IniLineKind.KeyValue;
            line.Key = text.Substring(first, equals - first).Trim();
            int vs = equals + 1;
            while (vs < bodyEnd && IsSpace(text[vs])) vs++;
            line.ValueStart = vs;
            line.ValueLength = bodyEnd - vs;
            line.Value = text.Substring(vs, bodyEnd - vs);
            line.RawValue = text.Substring(equals + 1).Trim();
            return line;
        }

        static string StripComment(string s)
        {
            int c = s.IndexOfAny(new[] { ';', '#' });
            return c < 0 ? s : s.Substring(0, c);
        }

        public byte[] ToBytes()
        {
            var sb = new StringBuilder();
            foreach (var line in Lines) { sb.Append(line.Text); sb.Append(line.Eol); }
            byte[] body = (Utf8 ? new UTF8Encoding(false) : Latin1).GetBytes(sb.ToString());
            if (!HasBom) return body;
            var all = new byte[body.Length + 3];
            all[0] = 0xEF; all[1] = 0xBB; all[2] = 0xBF;
            Buffer.BlockCopy(body, 0, all, 3, body.Length);
            return all;
        }

        static bool Same(string a, string b) { return string.Equals(a, b, StringComparison.OrdinalIgnoreCase); }

        IniLine FindLive(string section, string key)
        {
            IniLine found = null;   // the game takes the last line that names the key
            foreach (var line in Lines)
                if (line.Kind == IniLineKind.KeyValue && Same(line.Section, section) && Same(line.Key, key)) found = line;
            return found;
        }

        /// <summary>The value in force, or null when no live line names the key.</summary>
        public string Get(string section, string key)
        {
            var line = FindLive(section, key);
            return line == null ? null : line.Value;
        }

        /// <summary>Everything after '=' (for paths: ';' and '#' are part of it, quotes are removed).</summary>
        public string GetRaw(string section, string key)
        {
            var line = FindLive(section, key);
            if (line == null) return null;
            string v = line.RawValue;
            // One quoted value loses its quotes; several quoted words (a command line) keep theirs.
            if (v.Length >= 2 && v[0] == '"' && v[v.Length - 1] == '"' && v.IndexOf('"', 1) == v.Length - 1) v = v.Substring(1, v.Length - 2);
            return v;
        }

        public bool Has(string section, string key) { return FindLive(section, key) != null; }

        /// <summary>True when the file names the key at all, live or switched off with ';'.</summary>
        public bool Mentions(string section, string key)
        {
            foreach (var line in Lines)
                if ((line.Kind == IniLineKind.KeyValue || line.Kind == IniLineKind.Comment) && line.Key != null &&
                    Same(line.Section, section) && Same(line.Key, key)) return true;
            return false;
        }

        public bool HasSection(string section)
        {
            foreach (var line in Lines) if (line.Kind == IniLineKind.Section && Same(line.Section, section)) return true;
            return false;
        }

        public IEnumerable<IniLine> LiveLines()
        {
            foreach (var line in Lines) if (line.Kind == IniLineKind.KeyValue) yield return line;
        }

        string LeadingSpace(string text)
        {
            int i = 0;
            while (i < text.Length && IsSpace(text[i])) i++;
            return text.Substring(0, i);
        }

        /// <summary>
        /// Sets a key. A live line keeps its spelling, spacing and trailing comment; a switched-off
        /// "; Key=value" line becomes live; otherwise the line is added at the end of its section, and
        /// the section at the end of the file.
        /// </summary>
        public void Set(string section, string key, string value, bool raw = false)
        {
            var live = FindLive(section, key);
            if (live != null)
            {
                string text;
                if (raw)
                {
                    int eq = live.Text.IndexOf('=');
                    text = live.Text.Substring(0, eq + 1) + value;
                }
                else text = live.Text.Substring(0, live.ValueStart) + value + live.Text.Substring(live.ValueStart + live.ValueLength);
                Replace(live, text);
                return;
            }
            foreach (var line in Lines)
            {
                if (line.Kind == IniLineKind.Comment && line.Key != null && Same(line.Section, section) && Same(line.Key, key))
                {
                    Replace(line, LeadingSpace(line.Text) + line.Key + "=" + value);
                    return;
                }
            }
            int header = -1, last = -1;
            for (int i = 0; i < Lines.Count; i++)
            {
                var line = Lines[i];
                if (line.Kind == IniLineKind.Section)
                {
                    if (Same(line.Section, section)) { header = i; last = i; }
                    continue;
                }
                if (header >= 0 && Same(line.Section, section) && line.Kind != IniLineKind.Blank) last = i;
            }
            if (header < 0)
            {
                if (Lines.Count > 0)
                {
                    var tail = Lines[Lines.Count - 1];
                    if (tail.Eol.Length == 0) tail.Eol = NewLine;
                    if (tail.Kind != IniLineKind.Blank) Lines.Add(new IniLine { Text = "", Eol = NewLine, Kind = IniLineKind.Blank, Section = tail.Section });
                }
                var head = ParseLine("[" + section + "]", "");
                head.Eol = NewLine;
                Lines.Add(head);
                last = Lines.Count - 1;
            }
            if (Lines[last].Eol.Length == 0) Lines[last].Eol = NewLine;
            var added = ParseLine(key + "=" + value, section);
            added.Eol = NewLine;
            Lines.Insert(last + 1, added);
        }

        /// <summary>Switches every live line of the key off by putting "; " in front of it.</summary>
        public void Unset(string section, string key)
        {
            for (int i = 0; i < Lines.Count; i++)
            {
                var line = Lines[i];
                if (line.Kind == IniLineKind.KeyValue && Same(line.Section, section) && Same(line.Key, key))
                {
                    string lead = LeadingSpace(line.Text);
                    Replace(line, lead + "; " + line.Text.Substring(lead.Length));
                }
            }
        }

        void Replace(IniLine line, string text)
        {
            var parsed = ParseLine(text, line.Section);
            line.Text = parsed.Text;
            line.Kind = parsed.Kind;
            line.Key = parsed.Key;
            line.Value = parsed.Value;
            line.RawValue = parsed.RawValue;
            line.ValueStart = parsed.ValueStart;
            line.ValueLength = parsed.ValueLength;
        }
    }
}
