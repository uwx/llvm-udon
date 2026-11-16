//===-- StubTypes.cs - Udon type strings to CLR types ---------------------===//
//
// The assembler resolves every `.data_start` type string through this chain.
// The editor's own resolver (VRC.Udon.VRCTypeResolverModules) needs VRCSDK3,
// VRC.Dynamics and Unity's runtime to initialize and throws NullReference on
// every name without them, and the SDK's SystemTypeResolver knows only
// SystemInt32/String/Single/Double/UInt32 -- it returns null for SystemInt64,
// SystemBoolean, SystemObject and every array name. So the harness supplies the
// complete map, and returns null for anything else so the failure names the
// string that was missing.
//
//===----------------------------------------------------------------------===//

using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using VRC.Udon.UAssembly.Interfaces;

namespace Udon.VM
{
    /// <summary>The subset of Unity's math types the backend emits.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct Vector2
    {
        public float x, y;
        public Vector2(float x, float y) { this.x = x; this.y = y; }
        public override string ToString() => $"({x}, {y})";
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Vector3
    {
        public float x, y, z;
        public Vector3(float x, float y, float z) { this.x = x; this.y = y; this.z = z; }
        public override string ToString() => $"({x}, {y}, {z})";
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Vector4
    {
        public float x, y, z, w;
        public Vector4(float x, float y, float z, float w) { this.x = x; this.y = y; this.z = z; this.w = w; }
        public override string ToString() => $"({x}, {y}, {z}, {w})";
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Quaternion
    {
        public float x, y, z, w;
        public Quaternion(float x, float y, float z, float w) { this.x = x; this.y = y; this.z = z; this.w = w; }
        public override string ToString() => $"({x}, {y}, {z}, {w})";
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Color
    {
        public float r, g, b, a;
        public Color(float r, float g, float b, float a) { this.r = r; this.g = g; this.b = b; this.a = a; }
        public override string ToString() => $"RGBA({r}, {g}, {b}, {a})";
    }

    /// <summary>
    /// Stand-ins for the Unity objects the writer can name. They carry no
    /// behaviour: the harness runs no scene, so a slot of one of these types
    /// only has to exist and round-trip.
    /// </summary>
    public class Transform { }
    public class GameObject { }
    public class UdonBehaviourHandle { }

    /// <summary>Maps the Udon type strings the backend emits to CLR types.</summary>
    public sealed class StubTypeResolver : IUAssemblyTypeResolver
    {
        private static readonly Dictionary<string, Type> Map = new Dictionary<string, Type>
        {
            // Scalars. SystemInt64 and SystemBoolean are here because the SDK's
            // resolver omits them.
            ["SystemInt32"] = typeof(int),
            ["SystemUInt32"] = typeof(uint),
            ["SystemInt64"] = typeof(long),
            ["SystemUInt64"] = typeof(ulong),
            ["SystemSingle"] = typeof(float),
            ["SystemDouble"] = typeof(double),
            ["SystemBoolean"] = typeof(bool),
            ["SystemString"] = typeof(string),
            ["SystemObject"] = typeof(object),
            ["SystemChar"] = typeof(char),
            ["SystemByte"] = typeof(byte),
            ["SystemSByte"] = typeof(sbyte),
            ["SystemInt16"] = typeof(short),
            ["SystemUInt16"] = typeof(ushort),

            // Arrays. The SDK resolver returns null for every one of these.
            ["SystemByteArray"] = typeof(byte[]),
            ["SystemInt32Array"] = typeof(int[]),
            ["SystemUInt32Array"] = typeof(uint[]),
            ["SystemInt64Array"] = typeof(long[]),
            ["SystemUInt64Array"] = typeof(ulong[]),
            ["SystemSingleArray"] = typeof(float[]),
            ["SystemDoubleArray"] = typeof(double[]),
            ["SystemStringArray"] = typeof(string[]),
            ["SystemObjectArray"] = typeof(object[]),
            ["SystemBooleanArray"] = typeof(bool[]),
            ["SystemCharArray"] = typeof(char[]),

            // Unity math and objects.
            ["UnityEngineVector2"] = typeof(Vector2),
            ["UnityEngineVector3"] = typeof(Vector3),
            ["UnityEngineVector4"] = typeof(Vector4),
            ["UnityEngineQuaternion"] = typeof(Quaternion),
            ["UnityEngineColor"] = typeof(Color),
            ["UnityEngineTransform"] = typeof(Transform),
            ["UnityEngineGameObject"] = typeof(GameObject),
            ["VRCUdonCommonInterfacesIUdonEventReceiver"] = typeof(UdonBehaviourHandle),
        };

        public Type GetTypeFromTypeString(string typeString)
        {
            if (typeString == null) return null;
            return Map.TryGetValue(typeString, out Type t) ? t : null;
        }

        /// <summary>The type strings this resolver knows, for diagnostics.</summary>
        public static IEnumerable<string> KnownTypes => Map.Keys;

        /// <summary>The CLR type for a Udon type string, or null.</summary>
        public static Type Lookup(string typeString) =>
            typeString != null && Map.TryGetValue(typeString, out Type t) ? t : null;
    }
}
