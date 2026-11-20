//=== gfx_adapter.zig - typed replacement for gfx_adapter.c -------------===//
//
// Same external surface as the C adapter: gSPMatrix, gSPDisplayList,
// gfx_adapter_bind_output_buffers. Internally it is value-semantics-first:
// the matrix and the transform run in typed structs, so the writer keeps them
// in %SystemSingle slots instead of the byte-addressed __mem array.
//
// The byte-level boundary is confined to exactly three places:
//   * reading the raw display-list words (they ARE a byte stream),
//   * copying Mtx (an i32[4][4] union) into a float Mat4 at gSPMatrix,
//   * writing the final float stream into the output buffers.
// Everything between is by value.

const Vec3 = extern struct { x: f32, y: f32, z: f32 };
const Mat4 = extern struct { m: [16]f32 };

// --- commands -----------------------------------------------------------
const GFXCMD_None = 0;
const GFXCMD_VertexData = 1;
const GFXCMD_Triangle = 2;
const GFXCMD_Light = 3;
const GFXCMD_Texture = 4;
const GFXCMD_SetTileSize = 5;
const GFXCMD_SetTextureImage = 6;
const GFXCMD_SubDisplayList = 7;
const GFXCMD_EndDisplayList = 8;

const NUM_USED_TEXTURES: usize = 11;

// libsm64.h SM64MarioGeometryBuffers layout, byte for byte.
const OutBuffers = extern struct {
    position: [*]f32,
    normal: [*]f32,
    color: [*]f32,
    uv: [*]f32,
    numTrianglesUsed: u16,
};

// Vtx_tn under GBI_FLOATS: three floats, flag, tc, then the signed-byte
// normal and alpha. The union Vtx is the same 8-byte footprint.
const Vtx = extern struct {
    ob: [3]f32,
    flag: u16,
    tc: [2]i16,
    n: [3]i8,
    a: u8,
};

var g_matrix: Mat4 = undefined;
var g_color: [3]f32 = undefined;
var g_scaleS: u16 = 0;
var g_scaleT: u16 = 0;
var g_uls: u16 = 0;
var g_ult: u16 = 0;
var g_textureOn: i32 = 0;
var g_textureIndex: i32 = 0;
var g_texWidth: f32 = 0;
var g_texHeight: f32 = 0;

var g_buffers: ?*OutBuffers = null;
var g_triPtr: [*]f32 = undefined;
var g_colorPtr: [*]f32 = undefined;
var g_normalPtr: [*]f32 = undefined;
var g_uvPtr: [*]f32 = undefined;
var g_triangles: u32 = 0;

const tex_widths = [11]f32{ 64, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32 };
const tex_heights = [11]f32{ 32, 32, 32, 32, 32, 32, 32, 32, 32, 64, 64 };

inline fn mtxf_mul_vec3f(mtx: Mat4, b: Vec3, w: f32) Vec3 {
    return .{
        .x = b.x * mtx.m[0] + b.y * mtx.m[4] + b.z * mtx.m[8] + w * mtx.m[12],
        .y = b.x * mtx.m[1] + b.y * mtx.m[5] + b.z * mtx.m[9] + w * mtx.m[13],
        .z = b.x * mtx.m[2] + b.y * mtx.m[6] + b.z * mtx.m[10] + w * mtx.m[14],
    };
}

/// Mirror of the decomp's `vec3f_normalize`: scale the vector to unit length.
/// The Zig adapter dropped this, so every normal stored un-normalised
/// (measured: the first normal-region mismatch was exactly there).
fn normalizeV(dst: [*]f32) void {
    const len2 = dst[0] * dst[0] + dst[1] * dst[1] + dst[2] * dst[2];
    // A zero-length normal divides by zero; the C version would produce NaN
    // through sqrtf, but the VM turns a division by zero extern into a halt.
    // Leave the vector unchanged in that case, matching the intent.
    if (len2 == 0.0)
        return;
    // The SDK only has SystemMath.__Sqrt__SystemDouble; an f32 sqrt asks for
    // an extern that resolves to no delegate and halts the VM. Compute in
    // f64 and narrow, which is what the C path does.
    const inv = 1.0 / @sqrt(@as(f64, len2));
    const invf: f32 = @floatCast(inv);
    dst[0] *= invf;
    dst[1] *= invf;
    dst[2] *= invf;
}

inline fn storeV(dst: [*]f32, v: Vec3) void {
    dst[0] = v.x;
    dst[1] = v.y;
    dst[2] = v.z;
}

/// Read the word at `ptr` and advance it by one element — the direct mirror of
/// the C adapter's `*ptr++`. A pointer cursor (not a loop-carried element index)
/// is essential: the writer mis-threads a loop-carried `i` phi across this
/// function's 8-arm switch and its recursive self-call, so a second `words[i]`
/// read in the same arm could land on the stale index and desync the walk
/// (measured: the recursion bases cycled with period 4 and the buffer overflowed
/// at 1024 triangles instead of terminating at 752).
inline fn nextWord(ptr: *[*]u32) u32 {
    const w = ptr.*;
    ptr.* = w + 1;
    return w[0];
}

// process_display_list: walk the raw words, gather three vertices, transform
// them, emit. The ONLY byte-level work here is reading `words` and the Vtx
// structs it points at; the transform itself is pure value math.
/// Recurse with nothing but the pointer live. A separate noinline function is
/// what limits the spilled set to one slot, matching the C adapter's cost.
///
/// The walk uses a POINTER cursor (`var ptr`), not a loop-carried element index.
/// The two are equivalent in C (`*ptr++` vs `words[i++]`), but the writer
/// mis-threads the element-index phi across this function's switch and
/// recursive self-call, while a single pointer value survives the emulated
/// stack frame correctly.
fn processDisplayList(words: [*]u32) void {
    var ptr: [*]u32 = words;
    var vdata: [*]Vtx = undefined;
    var haveVtx = false;

    while (true) {
        const cmd = nextWord(&ptr);
        switch (cmd) {
            GFXCMD_VertexData => {
                // The first operand IS the vertex buffer pointer; the next two
                // (count, first-index) are unused by this adapter.
                const v: usize = @intCast(nextWord(&ptr));
                _ = nextWord(&ptr); // n
                _ = nextWord(&ptr); // v0
                vdata = @ptrFromInt(v);
                haveVtx = true;
            },
            GFXCMD_Triangle => {
                const v00: usize = @intCast(nextWord(&ptr));
                const v01: usize = @intCast(nextWord(&ptr));
                const v02: usize = @intCast(nextWord(&ptr));
                _ = nextWord(&ptr); // flag0
                if (!haveVtx) unreachable;

                const p0 = Vec3{ .x = vdata[v00].ob[0], .y = vdata[v00].ob[1], .z = vdata[v00].ob[2] };
                const p1 = Vec3{ .x = vdata[v01].ob[0], .y = vdata[v01].ob[1], .z = vdata[v01].ob[2] };
                const p2 = Vec3{ .x = vdata[v02].ob[0], .y = vdata[v02].ob[1], .z = vdata[v02].ob[2] };

                const n0 = Vec3{ .x = @as(f32, @floatFromInt(vdata[v00].n[0])) / 128.0, .y = @as(f32, @floatFromInt(vdata[v00].n[1])) / 128.0, .z = @as(f32, @floatFromInt(vdata[v00].n[2])) / 128.0 };
                const n1 = Vec3{ .x = @as(f32, @floatFromInt(vdata[v01].n[0])) / 128.0, .y = @as(f32, @floatFromInt(vdata[v01].n[1])) / 128.0, .z = @as(f32, @floatFromInt(vdata[v01].n[2])) / 128.0 };
                const n2 = Vec3{ .x = @as(f32, @floatFromInt(vdata[v02].n[0])) / 128.0, .y = @as(f32, @floatFromInt(vdata[v02].n[1])) / 128.0, .z = @as(f32, @floatFromInt(vdata[v02].n[2])) / 128.0 };

                storeV(g_triPtr, mtxf_mul_vec3f(g_matrix, p0, 1.0));
                g_triPtr += 3;
                storeV(g_triPtr, mtxf_mul_vec3f(g_matrix, p1, 1.0));
                g_triPtr += 3;
                storeV(g_triPtr, mtxf_mul_vec3f(g_matrix, p2, 1.0));
                g_triPtr += 3;

                storeV(g_normalPtr, mtxf_mul_vec3f(g_matrix, n0, 0.0));
                normalizeV(g_normalPtr);
                g_normalPtr += 3;
                storeV(g_normalPtr, mtxf_mul_vec3f(g_matrix, n1, 0.0));
                normalizeV(g_normalPtr);
                g_normalPtr += 3;
                storeV(g_normalPtr, mtxf_mul_vec3f(g_matrix, n2, 0.0));
                normalizeV(g_normalPtr);
                g_normalPtr += 3;

                // Nine floats: the same RGB triplet repeated once per vertex.
                // Writing only three left the colour region one third full and
                // shifted every later triangle's data (measured: the first
                // mismatch was exactly at the colour region's start).
                var k: usize = 0;
                while (k < 9) : (k += 1) {
                    g_colorPtr[k] = g_color[k % 3];
                }
                g_colorPtr += 9;

                // The uv cursor is threaded through a LOCAL and stored back
                // once: three separate `g_uvPtr += 2` statements are dead
                // stores when LLVM folds the six writes into vector stores,
                // and the loop's phi-carried cursor then never advances, so
                // every triangle writes uv at the same index.
                var uv: [*]f32 = g_uvPtr;
                if (g_textureOn != 0) {
                    // convert_uv_to_atlas, twice for two triangles' worth.
                    const cu0 = (@as(f32, @floatFromInt(@as(i32, vdata[v00].tc[0]) * @as(i32, @intCast(g_scaleS)) >> 16)) - 8.0 * @as(f32, @floatFromInt(g_uls))) / 32.0 / g_texWidth;
                    const cv0 = (@as(f32, @floatFromInt(@as(i32, vdata[v00].tc[1]) * @as(i32, @intCast(g_scaleT)) >> 16)) - 8.0 * @as(f32, @floatFromInt(g_ult))) / 32.0 / g_texHeight;
                    uv[0] = cu0 * g_texWidth / 64.0 / @as(f32, @floatFromInt(NUM_USED_TEXTURES)) + @as(f32, @floatFromInt(g_textureIndex)) / @as(f32, @floatFromInt(NUM_USED_TEXTURES));
                    uv[1] = cv0 * g_texHeight / 64.0;
                    uv += 2;

                    const cu1 = (@as(f32, @floatFromInt(@as(i32, vdata[v01].tc[0]) * @as(i32, @intCast(g_scaleS)) >> 16)) - 8.0 * @as(f32, @floatFromInt(g_uls))) / 32.0 / g_texWidth;
                    const cv1 = (@as(f32, @floatFromInt(@as(i32, vdata[v01].tc[1]) * @as(i32, @intCast(g_scaleT)) >> 16)) - 8.0 * @as(f32, @floatFromInt(g_ult))) / 32.0 / g_texHeight;
                    uv[0] = cu1 * g_texWidth / 64.0 / @as(f32, @floatFromInt(NUM_USED_TEXTURES)) + @as(f32, @floatFromInt(g_textureIndex)) / @as(f32, @floatFromInt(NUM_USED_TEXTURES));
                    uv[1] = cv1 * g_texHeight / 64.0;
                    uv += 2;

                    const cu2 = (@as(f32, @floatFromInt(@as(i32, vdata[v02].tc[0]) * @as(i32, @intCast(g_scaleS)) >> 16)) - 8.0 * @as(f32, @floatFromInt(g_uls))) / 32.0 / g_texWidth;
                    const cv2 = (@as(f32, @floatFromInt(@as(i32, vdata[v02].tc[1]) * @as(i32, @intCast(g_scaleT)) >> 16)) - 8.0 * @as(f32, @floatFromInt(g_ult))) / 32.0 / g_texHeight;
                    uv[0] = cu2 * g_texWidth / 64.0 / @as(f32, @floatFromInt(NUM_USED_TEXTURES)) + @as(f32, @floatFromInt(g_textureIndex)) / @as(f32, @floatFromInt(NUM_USED_TEXTURES));
                    uv[1] = cv2 * g_texHeight / 64.0;
                    uv += 2;
                } else {
                    uv[0] = 1.0;
                    uv[1] = 1.0;
                    uv += 2;
                    uv[0] = 1.0;
                    uv[1] = 1.0;
                    uv += 2;
                    uv[0] = 1.0;
                    uv[1] = 1.0;
                    uv += 2;
                }
                g_uvPtr = uv;

                // The host reads this to know how many triangles to draw. The
                // C adapter recomputes it from the write cursor each triangle;
                // omitting it left the count at zero (measured: `report`
                // returned 2482000 instead of 2482752).
                // Count triangles directly. The C version derives this from the
                // write cursor, but `@intFromPtr` subtraction widens to i64 on
                // this target and emits a chain of Int64 externs for what is a
                // 32-bit value (measured: 24,821 extra Int64.__op_LogicalAnd).
                g_triangles += 1;
                if (g_buffers) |o|
                    o.numTrianglesUsed = @intCast(g_triangles);
            },
            GFXCMD_Light => {
                const l: usize = @intCast(nextWord(&ptr));
                const n = nextWord(&ptr);
                if (n == 1) {
                    const data: *const extern struct { col: [3]u8, pad: u8 } = @ptrFromInt(l);
                    g_color[0] = @as(f32, @floatFromInt(data.col[0])) / 255.0;
                    g_color[1] = @as(f32, @floatFromInt(data.col[1])) / 255.0;
                    g_color[2] = @as(f32, @floatFromInt(data.col[2])) / 255.0;
                }
            },
            GFXCMD_Texture => {
                g_scaleS = @intCast(nextWord(&ptr));
                g_scaleT = @intCast(nextWord(&ptr));
                g_textureOn = @intCast(nextWord(&ptr));
            },
            GFXCMD_SetTileSize => {
                g_uls = @intCast(nextWord(&ptr));
                g_ult = @intCast(nextWord(&ptr));
                _ = nextWord(&ptr); // lrs
                _ = nextWord(&ptr); // lrt
            },
            GFXCMD_SetTextureImage => {
                g_textureIndex = @intCast(nextWord(&ptr));
                g_texWidth = tex_widths[@intCast(g_textureIndex)];
                g_texHeight = tex_heights[@intCast(g_textureIndex)];
            },
            GFXCMD_SubDisplayList => {
                // Read the pointer, drop every other live value, then recurse:
                // the writer spills one caller-save slot per live value across
                // this call, so keeping the whole switch's state alive here
                // multiplies the stack cost of each nesting level (measured:
                // a 1024-deep list overflowed the caller-save stacks).
                const sub: usize = @intCast(nextWord(&ptr));
                processDisplayList(@ptrFromInt(sub));
            },
            GFXCMD_EndDisplayList => return,
            // An unknown command word: nextWord already advanced past it (the
            // command word was at `ptr` and it now points at the next word),
            // so the correct action is NOTHING. Advancing again desyncs the
            // walk (measured: the EndDisplayList word was stepped over, the
            // loop ran into adjacent memory, and the uv cursor overran the
            // geometry buffer).
            else => {},
        }
    }
}

// --- C ABI ---------------------------------------------------------------

export fn gSPMatrix(pkt: ?*anyopaque, m: [*]MtxRaw, flags: u8) void {
    _ = pkt;
    _ = flags;
    // Mtx is i32[4][4]; Mat4 is [16]f32. Copy the 16 words, each already a
    // float bit pattern under GBI_FLOATS.
    var j: usize = 0;
    while (j < 16) : (j += 1) {
        g_matrix.m[j] = m[0].m[j / 4][j % 4];
    }
}

const MtxRaw = extern struct { m: [4][4]f32 };

export fn gSPDisplayList(pkt: ?*anyopaque, dl: ?*anyopaque) void {
    _ = pkt;
    processDisplayList(@ptrCast(@alignCast(dl)));
}

export fn gfx_adapter_bind_output_buffers(out: ?*OutBuffers) void {
    g_buffers = out;
    const o = out.?;
    g_triPtr = o.position;
    g_colorPtr = o.color;
    g_normalPtr = o.normal;
    g_uvPtr = o.uv;
    o.numTrianglesUsed = 0;
    g_triangles = 0;
}