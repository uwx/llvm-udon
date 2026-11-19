//===-- UdonHeapImage.cs - apply a heap image to an IUdonProgram ----------===//
//
// The writer emits constants the assembler's literal syntax cannot express as a
// JSON sidecar rather than as `.data_start` text: `SystemInt64`,
// `SystemBoolean`, `SystemSingle`/`SystemDouble` as raw bits, and arbitrary
// bytes for the globals image. This is the one place that knows the format, so
// the headless harness and the Unity importer cannot drift apart.
//
//===----------------------------------------------------------------------===//

using System;
using System.Collections.Generic;
using System.Globalization;
using VRC.Udon.Common.Interfaces;

namespace Udon.Heap
{
    /// <summary>One constant to write into the heap after assembly.</summary>
    public sealed class HeapSlot
    {
        /// <summary>Symbol name, as it appears in the `.uasm`.</summary>
        public string Symbol;

        /// <summary>Udon type string, e.g. `SystemInt64`.</summary>
        public string Type;

        /// <summary>The value kind: `i64`, `u64`, `f32bits`, `f64bits`, `bool`, `bytes`.</summary>
        public string Kind;

        /// <summary>Integer payload for `i64`/`u64`/`f32bits`/`f64bits`, as written in the JSON.</summary>
        public ulong Bits;

        /// <summary>Boolean payload for `bool`.</summary>
        public bool Bool;

        /// <summary>Byte payload for `bytes` (the globals image).</summary>
        public byte[] Bytes;

        /// <summary>
        /// True when `bytes` should be unpacked into an `int[]` rather than a
        /// `byte[]`. `__mem` and `__data_image` are word arrays, so their
        /// payload is a little-endian word stream; everything else is bytes.
        /// </summary>
        public bool BytesAreWords;
        /// <summary>`bytes` payload is an f32 stream, applied as `float[]`.</summary>
        public bool BytesAreFloats;
    }

    /// <summary>A parsed `&lt;name&gt;.udonheap.json`.</summary>
    public sealed class HeapImage
    {
        public uint HeapSlots;
        public readonly List<HeapSlot> Slots = new List<HeapSlot>();

        /// <summary>
        /// Parse the sidecar. Deliberately hand-rolled: the importer runs inside
        /// Unity, where a JSON dependency is a package to install, and the
        /// format is one object with a fixed shape.
        /// </summary>
        public static HeapImage Parse(string json)
        {
            var reader = new JsonReader(json);
            var image = new HeapImage();

            reader.Expect('{');
            bool first = true;
            while (!reader.TryConsume('}'))
            {
                if (!first) reader.Expect(',');
                first = false;
                string key = reader.ReadString();
                reader.Expect(':');

                if (key == "heapSlots")
                {
                    image.HeapSlots = (uint)reader.ReadNumber();
                }
                else if (key == "slots")
                {
                    reader.Expect('{');
                    bool firstSlot = true;
                    while (!reader.TryConsume('}'))
                    {
                        if (!firstSlot) reader.Expect(',');
                        firstSlot = false;
                        // Each entry is `"<symbol>": { "type": ..., "value": {...} }`.
                        string symbol = reader.ReadString();
                        reader.Expect(':');
                        image.Slots.Add(ParseSlot(reader, symbol));
                    }
                }
                else
                {
                    reader.SkipValue();
                }
            }
            return image;
        }

        private static HeapSlot ParseSlot(JsonReader reader, string symbol)
        {
            var slot = new HeapSlot { Symbol = symbol };
            reader.Expect('{');
            bool first = true;
            while (!reader.TryConsume('}'))
            {
                if (!first) reader.Expect(',');
                first = false;
                string key = reader.ReadString();
                reader.Expect(':');
                switch (key)
                {
                    case "type": slot.Type = reader.ReadString(); break;
                    case "value":
                        reader.Expect('{');
                        bool firstValue = true;
                        while (!reader.TryConsume('}'))
                        {
                            if (!firstValue) reader.Expect(',');
                            firstValue = false;
                            string vkey = reader.ReadString();
                            reader.Expect(':');
                            switch (vkey)
                            {
                                case "i64":
                                case "u64":
                                case "f32bits":
                                case "f64bits":
                                    slot.Kind = vkey;
                                    slot.Bits = reader.ReadUnsignedBits();
                                    break;
                                case "bool":
                                    slot.Kind = vkey;
                                    slot.Bool = reader.ReadBool();
                                    break;
                                case "bytes":
                                    slot.Kind = vkey;
                                    slot.Bytes = reader.ReadBase64();
                                    break;
                                case "floatArray":
                                    // A modifier on `bytes`: the payload is a
                                    // little-endian f32 stream and becomes a
                                    // `float[]`, which is what a
                                    // %SystemSingleArray slot declares.
                                    slot.BytesAreFloats = reader.ReadBool();
                                    break;
                                case "wordArray":
                                    // A modifier on `bytes`, not a payload: the
                                    // byte stream is a little-endian word stream
                                    // and must become an `int[]`, because
                                    // `__mem` and `__data_image` are word arrays.
                                    slot.BytesAreWords = reader.ReadBool();
                                    break;
                                default:
                                    reader.SkipValue();
                                    break;
                            }
                        }
                        break;
                    default:
                        reader.SkipValue();
                        break;
                }
            }
            return slot;
        }

        /// <summary>
        /// Write every slot into the program's heap. Symbols the assembler did
        /// not define are an error naming the symbol rather than a silent skip:
        /// a missing constant would run as a zero.
        /// </summary>
        public void Apply(IUdonProgram program, Action<string> warn = null)
        {
            IUdonHeap heap = program.Heap;
            IUdonSymbolTable symbols = program.SymbolTable;
            foreach (HeapSlot slot in Slots)
            {
                if (!symbols.HasAddressForSymbol(slot.Symbol))
                {
                    throw new InvalidOperationException(
                        "heap image names a symbol the program does not define: " + slot.Symbol);
                }
                uint address = symbols.GetAddressFromSymbol(slot.Symbol);
                switch (slot.Kind)
                {
                    case "i64":
                        heap.SetHeapVariable(address, unchecked((long)slot.Bits), typeof(long));
                        break;
                    case "u64":
                        heap.SetHeapVariable(address, slot.Bits, typeof(ulong));
                        break;
                    case "f32bits":
                        heap.SetHeapVariable(address,
                            BitConverter.Int32BitsToSingle(unchecked((int)slot.Bits)), typeof(float));
                        break;
                    case "f64bits":
                        heap.SetHeapVariable(address,
                            BitConverter.Int64BitsToDouble(unchecked((long)slot.Bits)), typeof(double));
                        break;
                    case "bool":
                        heap.SetHeapVariable(address, slot.Bool, typeof(bool));
                        break;
                    case "bytes":
                        if (slot.BytesAreFloats)
                        {
                            // %SystemSingleArray: one float per 4 bytes.
                            float[] floats = new float[slot.Bytes.Length / 4];
                            for (int i = 0; i < floats.Length; i++)
                                floats[i] = BitConverter.ToSingle(slot.Bytes, i * 4);
                            heap.SetHeapVariable(address, floats, typeof(float[]));
                        }
                        else if (slot.BytesAreWords)
                        {
                            // The payload is a little-endian word stream; a
                            // word array is what the slot declares, and a
                            // byte[] would fail the slot's type check.
                            int[] words = new int[(slot.Bytes.Length + 3) / 4];
                            for (int i = 0; i < slot.Bytes.Length; i++)
                                words[i / 4] |= slot.Bytes[i] << (8 * (i % 4));
                            heap.SetHeapVariable(address, words, typeof(int[]));
                        }
                        else
                        {
                            heap.SetHeapVariable(address, slot.Bytes, typeof(byte[]));
                        }
                        break;
                    default:
                        throw new InvalidOperationException(
                            "heap image slot " + slot.Symbol + " has unknown value kind '" +
                            slot.Kind + "'");
                }
            }
        }
    }

    /// <summary>
    /// The smallest JSON reader the image format needs. Accepts what the writer
    /// emits: objects, strings, unsigned integers, booleans, null and base64
    /// strings, with no arrays or floats to worry about.
    /// </summary>
    public sealed class JsonReader
    {
        private readonly string _s;
        private int _i;

        public JsonReader(string s)
        {
            _s = s ?? throw new ArgumentNullException(nameof(s));
        }

        private void SkipWhitespace()
        {
            while (_i < _s.Length && char.IsWhiteSpace(_s[_i])) _i++;
        }

        public bool TryConsume(char c)
        {
            SkipWhitespace();
            if (_i < _s.Length && _s[_i] == c) { _i++; return true; }
            return false;
        }

        public void Expect(char c)
        {
            SkipWhitespace();
            if (_i >= _s.Length || _s[_i] != c)
                throw new FormatException(
                    "heap image: expected '" + c + "' at offset " + _i +
                    ( _i < _s.Length ? ", found '" + _s[_i] + "'" : ", at end of input"));
            _i++;
        }

        public string ReadString()
        {
            SkipWhitespace();
            Expect('"');
            var sb = new System.Text.StringBuilder();
            while (true)
            {
                if (_i >= _s.Length) throw new FormatException("heap image: unterminated string");
                char c = _s[_i++];
                if (c == '"') break;
                if (c != '\\') { sb.Append(c); continue; }
                if (_i >= _s.Length) throw new FormatException("heap image: unterminated escape");
                char e = _s[_i++];
                switch (e)
                {
                    case '"': sb.Append('"'); break;
                    case '\\': sb.Append('\\'); break;
                    case '/': sb.Append('/'); break;
                    case 'b': sb.Append('\b'); break;
                    case 'f': sb.Append('\f'); break;
                    case 'n': sb.Append('\n'); break;
                    case 'r': sb.Append('\r'); break;
                    case 't': sb.Append('\t'); break;
                    case 'u':
                        if (_i + 4 > _s.Length) throw new FormatException("heap image: short \\u escape");
                        sb.Append((char)int.Parse(_s.Substring(_i, 4), NumberStyles.HexNumber,
                                                  CultureInfo.InvariantCulture));
                        _i += 4;
                        break;
                    default:
                        throw new FormatException("heap image: bad escape \\" + e);
                }
            }
            return sb.ToString();
        }

        /// <summary>An unsigned integer, as emitted for a bit pattern.</summary>
        public ulong ReadUnsignedBits()
        {
            SkipWhitespace();
            int start = _i;
            if (_i < _s.Length && _s[_i] == '-')
            {
                // A negative i64 constant is written as a signed decimal; the
                // bit pattern is what the slot needs.
                _i++;
                while (_i < _s.Length && char.IsDigit(_s[_i])) _i++;
                long signed = long.Parse(_s.Substring(start, _i - start), CultureInfo.InvariantCulture);
                return unchecked((ulong)signed);
            }
            while (_i < _s.Length && char.IsDigit(_s[_i])) _i++;
            if (_i == start) throw new FormatException("heap image: expected a number at offset " + start);
            return ulong.Parse(_s.Substring(start, _i - start), CultureInfo.InvariantCulture);
        }

        public long ReadNumber() => unchecked((long)ReadUnsignedBits());

        public bool ReadBool()
        {
            SkipWhitespace();
            if (string.CompareOrdinal(_s, _i, "true", 0, 4) == 0) { _i += 4; return true; }
            if (string.CompareOrdinal(_s, _i, "false", 0, 5) == 0) { _i += 5; return false; }
            throw new FormatException("heap image: expected true or false at offset " + _i);
        }

        public byte[] ReadBase64() => Convert.FromBase64String(ReadString());

        public void SkipValue()
        {
            SkipWhitespace();
            if (_i >= _s.Length) throw new FormatException("heap image: unexpected end of input");
            char c = _s[_i];
            if (c == '"') { ReadString(); return; }
            if (c == '{' || c == '[')
            {
                char close = c == '{' ? '}' : ']';
                _i++;
                int depth = 1;
                while (_i < _s.Length && depth > 0)
                {
                    char d = _s[_i++];
                    if (d == '"') { _i--; ReadString(); continue; }
                    if (d == c) depth++;
                    else if (d == close) depth--;
                }
                return;
            }
            int start = _i;
            while (_i < _s.Length && _s[_i] != ',' && _s[_i] != '}' && _s[_i] != ']') _i++;
            if (_i == start) throw new FormatException("heap image: empty value at offset " + start);
        }
    }
}
