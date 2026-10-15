// JSON for the payload's manifest and install.manifest: a writer and a small reader. Objects are
// Dictionary<string, object>, arrays List<object>, numbers long or double, plus string, bool and null.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text;

namespace ChiliSetup
{
    public static class Json
    {
        public static string Write(object value)
        {
            var sb = new StringBuilder();
            Write(sb, value, 0);
            return sb.ToString();
        }

        static void Write(StringBuilder sb, object v, int depth)
        {
            if (v == null) { sb.Append("null"); return; }
            var s = v as string;
            if (s != null) { Str(sb, s); return; }
            if (v is bool) { sb.Append((bool)v ? "true" : "false"); return; }
            if (v is int || v is long || v is uint || v is ulong) { sb.Append(Convert.ToString(v, CultureInfo.InvariantCulture)); return; }
            if (v is double) { sb.Append(((double)v).ToString("R", CultureInfo.InvariantCulture)); return; }
            var d = v as IDictionary<string, object>;
            if (d != null)
            {
                sb.Append('{');
                bool first = true;
                foreach (var pair in d)
                {
                    if (!first) sb.Append(',');
                    first = false;
                    sb.Append('\n').Append(' ', (depth + 1) * 2);
                    Str(sb, pair.Key);
                    sb.Append(": ");
                    Write(sb, pair.Value, depth + 1);
                }
                if (!first) sb.Append('\n').Append(' ', depth * 2);
                sb.Append('}');
                return;
            }
            var list = v as System.Collections.IEnumerable;
            if (list != null)
            {
                sb.Append('[');
                bool first = true;
                // Arrays of objects one per line; arrays of plain values on one line.
                foreach (var item in list)
                {
                    if (!first) sb.Append(',');
                    if (item is IDictionary<string, object>) sb.Append('\n').Append(' ', (depth + 1) * 2);
                    else if (!first) sb.Append(' ');
                    first = false;
                    WriteCompact(sb, item, depth + 1);
                }
                sb.Append(']');
                return;
            }
            Str(sb, v.ToString());
        }

        // An object inside an array on one line (the file lists stay one line per file).
        static void WriteCompact(StringBuilder sb, object v, int depth)
        {
            var d = v as IDictionary<string, object>;
            if (d == null) { Write(sb, v, depth); return; }
            sb.Append('{');
            bool first = true;
            foreach (var pair in d)
            {
                if (!first) sb.Append(", ");
                first = false;
                Str(sb, pair.Key);
                sb.Append(": ");
                WriteCompact(sb, pair.Value, depth);
            }
            sb.Append('}');
        }

        static void Str(StringBuilder sb, string s)
        {
            sb.Append('"');
            foreach (char c in s)
            {
                switch (c)
                {
                    case '"': sb.Append("\\\""); break;
                    case '\\': sb.Append("\\\\"); break;
                    case '\n': sb.Append("\\n"); break;
                    case '\r': sb.Append("\\r"); break;
                    case '\t': sb.Append("\\t"); break;
                    default:
                        if (c < 0x20) sb.Append("\\u").Append(((int)c).ToString("x4", CultureInfo.InvariantCulture));
                        else sb.Append(c);
                        break;
                }
            }
            sb.Append('"');
        }

        public static object Read(string text)
        {
            int p = 0;
            var v = Value(text, ref p);
            Space(text, ref p);
            if (p != text.Length) throw new FormatException("text after the JSON value at " + p);
            return v;
        }

        static void Space(string t, ref int p) { while (p < t.Length && (t[p] == ' ' || t[p] == '\n' || t[p] == '\r' || t[p] == '\t')) p++; }

        static object Value(string t, ref int p)
        {
            Space(t, ref p);
            if (p >= t.Length) throw new FormatException("the JSON ends early");
            char c = t[p];
            if (c == '{')
            {
                p++;
                var d = new Dictionary<string, object>();
                Space(t, ref p);
                if (p < t.Length && t[p] == '}') { p++; return d; }
                while (true)
                {
                    Space(t, ref p);
                    string key = Text(t, ref p);
                    Space(t, ref p);
                    if (p >= t.Length || t[p] != ':') throw new FormatException("':' expected at " + p);
                    p++;
                    d[key] = Value(t, ref p);
                    Space(t, ref p);
                    if (p < t.Length && t[p] == ',') { p++; continue; }
                    if (p < t.Length && t[p] == '}') { p++; return d; }
                    throw new FormatException("',' or '}' expected at " + p);
                }
            }
            if (c == '[')
            {
                p++;
                var l = new List<object>();
                Space(t, ref p);
                if (p < t.Length && t[p] == ']') { p++; return l; }
                while (true)
                {
                    l.Add(Value(t, ref p));
                    Space(t, ref p);
                    if (p < t.Length && t[p] == ',') { p++; continue; }
                    if (p < t.Length && t[p] == ']') { p++; return l; }
                    throw new FormatException("',' or ']' expected at " + p);
                }
            }
            if (c == '"') return Text(t, ref p);
            if (t.Length - p >= 4 && string.CompareOrdinal(t, p, "true", 0, 4) == 0) { p += 4; return true; }
            if (t.Length - p >= 5 && string.CompareOrdinal(t, p, "false", 0, 5) == 0) { p += 5; return false; }
            if (t.Length - p >= 4 && string.CompareOrdinal(t, p, "null", 0, 4) == 0) { p += 4; return null; }
            int start = p;
            while (p < t.Length && "+-0123456789.eE".IndexOf(t[p]) >= 0) p++;
            string num = t.Substring(start, p - start);
            if (num.Length == 0) throw new FormatException("a value expected at " + start);
            long l2;
            if (num.IndexOfAny(new[] { '.', 'e', 'E' }) < 0 && long.TryParse(num, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out l2)) return l2;
            return double.Parse(num, NumberStyles.Float, CultureInfo.InvariantCulture);
        }

        static string Text(string t, ref int p)
        {
            if (p >= t.Length || t[p] != '"') throw new FormatException("a string expected at " + p);
            p++;
            var sb = new StringBuilder();
            while (p < t.Length)
            {
                char c = t[p++];
                if (c == '"') return sb.ToString();
                if (c != '\\') { sb.Append(c); continue; }
                if (p >= t.Length) break;
                char e = t[p++];
                switch (e)
                {
                    case 'n': sb.Append('\n'); break;
                    case 'r': sb.Append('\r'); break;
                    case 't': sb.Append('\t'); break;
                    case 'b': sb.Append('\b'); break;
                    case 'f': sb.Append('\f'); break;
                    case 'u':
                        if (p + 4 > t.Length) throw new FormatException("a short \\u escape");
                        sb.Append((char)int.Parse(t.Substring(p, 4), NumberStyles.HexNumber, CultureInfo.InvariantCulture));
                        p += 4;
                        break;
                    default: sb.Append(e); break;
                }
            }
            throw new FormatException("a string does not end");
        }

        // Reading helpers.
        public static Dictionary<string, object> Obj(object o, string key)
        {
            var d = o as Dictionary<string, object>;
            object v;
            return d != null && d.TryGetValue(key, out v) ? v as Dictionary<string, object> : null;
        }
        public static List<object> Arr(object o, string key)
        {
            var d = o as Dictionary<string, object>;
            object v;
            return d != null && d.TryGetValue(key, out v) ? v as List<object> : null;
        }
        public static string S(object o, string key, string fallback = null)
        {
            var d = o as Dictionary<string, object>;
            object v;
            return d != null && d.TryGetValue(key, out v) && v != null ? Convert.ToString(v, CultureInfo.InvariantCulture) : fallback;
        }
        public static long L(object o, string key, long fallback = 0)
        {
            var d = o as Dictionary<string, object>;
            object v;
            if (d == null || !d.TryGetValue(key, out v) || v == null) return fallback;
            if (v is long) return (long)v;
            if (v is double) return (long)(double)v;
            long r;
            return long.TryParse(Convert.ToString(v, CultureInfo.InvariantCulture), NumberStyles.Integer, CultureInfo.InvariantCulture, out r) ? r : fallback;
        }
        public static bool B(object o, string key, bool fallback = false)
        {
            var d = o as Dictionary<string, object>;
            object v;
            return d != null && d.TryGetValue(key, out v) && v is bool ? (bool)v : fallback;
        }
    }
}
