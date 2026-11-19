// The Mario playground: the bridge between libsm64 and its Unity host.
//
// This is a real game loop in miniature, not a checksum:
//
//   `_start`      loads a floor and walls, spawns Mario, runs one tick so the
//                 geometry buffer and state are populated before Unity reads
//                 them.
//   `_tick`       one 30 Hz tick. Reads `ud_input` (the host-written input),
//                 calls `sm64_mario_tick`, then copies the results into the
//                 exported bridge globals.
//
// The bridge globals are the ABI. The writer exports an offset slot for each
// `udon_export_*` global and, with `-udon-export-memory`, exports `__mem`
// itself, so Unity reads and writes every value through `__mem` words at
// those offsets. Layout (all little-endian):
//
//   ud_input[8]     float4: stickX, stickY, buttonA, buttonB   (host -> C)
//   ud_state[16]    float4: x, y, z, faceAngle                  (C -> host)
//   ud_geo_count[1] uint32: triangles used                       (C -> host)
//   ud_geo[9216]    9 floats * 1024 triangles: positions(3v*3),
//                   normals(3v*3), colors(3v*3), uv(2*3)         (C -> host)
//
// Sizes: the state the host needs is five floats, padded to a four-float
// vector for word alignment. The geometry buffer is SM64_GEO_MAX_TRIANGLES
// (1024) triangles; the reference host allocates exactly 9 floats per
// triangle for position, normal and color and 6 for uv, i.e. 33 floats, but
// position/normal/color are each 9 floats per triangle *because each vertex
// is 3 floats*: 3 vertices * 3 floats = 9. So the whole triangle is
// 9 + 9 + 9 + 6 = 33 floats.

#include <stdint.h>
#include <string.h>

extern void sm64_global_init(const uint8_t *rom, uint8_t *outTexture);
extern void memory_init(void);
extern void load_mario_anims_from_rom(const uint8_t *rom);
extern void sm64_static_surfaces_load(const struct SM64Surface *surfaces,
                                      uint32_t numSurfaces);
extern int32_t sm64_mario_create(float x, float y, float z);
extern void sm64_mario_tick(int32_t marioId, const void *inputs, void *state,
                            void *buffers);

// Byte-compatible copies of the public structs from libsm64.h. Kept local so
// this file stays self-contained; the compiler checks the ABI, not the names,
// so the only requirement is that the layout matches.
struct SM64Surface {
  int16_t type;
  int16_t force;
  uint16_t terrain;
  int32_t vertices[3][3];
};

struct SM64MarioInputs {
  float camLookX, camLookZ;
  float stickX, stickY;
  uint8_t buttonA, buttonB, buttonZ;
};

struct SM64MarioState {
  float position[3];
  float velocity[3];
  float faceAngle;
  float forwardVelocity;
  int16_t health;
  uint32_t action;
  int32_t animID;
  int16_t animFrame;
  uint32_t flags;
  uint32_t particleFlags;
  int16_t invincTimer;
};

struct SM64MarioGeometryBuffers {
  float *position;
  float *normal;
  float *color;
  float *uv;
  uint16_t numTrianglesUsed;
};

#define SURFACE_NOT_SLIPPERY 0x0015
#define TERRAIN_GRASS 0

// --- static playground: a floor and two low walls -------------------------
// Vertices are SM64 units (1 unit = 1/65536 of the game's scale). The floor
// is a 2x2 quad of 400x400 triangles at y=0, big enough that Mario can run a
// while without falling off, and the walls are 200-unit slabs that give the
// camera something to look at and the movement something to run into.

static const struct SM64Surface kSurfaces[6] = {
    {SURFACE_NOT_SLIPPERY, 0, TERRAIN_GRASS,
      {{-200, 0, -200}, {200, 0, 200}, {200, 0, -200}}},
    {SURFACE_NOT_SLIPPERY, 0, TERRAIN_GRASS,
      {{-200, 0, -200}, {-200, 0, 200}, {200, 0, 200}}},
    {SURFACE_NOT_SLIPPERY, 0, TERRAIN_GRASS,
      {{-200, 0, 200}, {-200, 200, 200}, {-200, 200, -200}}},
    {SURFACE_NOT_SLIPPERY, 0, TERRAIN_GRASS,
      {{-200, 0, 200}, {-200, 200, -200}, {-200, 0, -200}}},
    {SURFACE_NOT_SLIPPERY, 0, TERRAIN_GRASS,
      {{200, 0, 200}, {200, 200, -200}, {200, 200, 200}}},
    {SURFACE_NOT_SLIPPERY, 0, TERRAIN_GRASS,
      {{200, 0, 200}, {200, 0, -200}, {200, 200, -200}}},
};

// --- bridge globals -------------------------------------------------------

// The host writes this each frame; `_tick` reads it.
float udon_export_ud_input[8];

// The host copies the whole ROM into `__mem` at the byte offset that the
// exported `__bridge_ud_rom` slot holds, before `_start`. The driver reads the
// ROM through this array's own address, so no pointer indirection is needed:
// `udon_export_ud_rom` is the window itself. 8 MiB is the size libsm64's
// offsets assume; the host zero-fills beyond the end of the file.
uint32_t udon_export_ud_rom[2 * 1024 * 1024];
#define UD_ROM_SIZE (8u * 1024 * 1024)

// `_tick` writes these; the host reads them.
float udon_export_ud_state[16];
uint32_t udon_export_ud_geo_count;
float udon_export_ud_geo[1024 * 33];

// -1 until `boot` creates Mario. A zero-initialised id would make `tick` drive
// a Mario instance that does not exist.
static int32_t s_marioId = -1;

static void tick_once(void)
{
    struct SM64MarioInputs inputs;
    struct SM64MarioState state;
    struct SM64MarioGeometryBuffers buffers;

    inputs.camLookX = 0.0f;
    inputs.camLookZ = 1.0f;
    inputs.stickX = udon_export_ud_input[0];
    inputs.stickY = udon_export_ud_input[1];
    inputs.buttonA = (uint8_t)udon_export_ud_input[2];
    inputs.buttonB = (uint8_t)udon_export_ud_input[3];
    inputs.buttonZ = (uint8_t)udon_export_ud_input[4];

    buffers.position = udon_export_ud_geo;
    buffers.normal = udon_export_ud_geo + 1024 * 9;
    buffers.color = udon_export_ud_geo + 1024 * 18;
    buffers.uv = udon_export_ud_geo + 1024 * 27;
    buffers.numTrianglesUsed = 0;

    sm64_mario_tick(s_marioId, &inputs, &state, &buffers);

    udon_export_ud_state[0] = state.position[0];
    udon_export_ud_state[1] = state.position[1];
    udon_export_ud_state[2] = state.position[2];
    udon_export_ud_state[3] = state.faceAngle;
    udon_export_ud_state[4] = state.forwardVelocity;
    udon_export_ud_state[5] = state.health;
    udon_export_ud_state[6] = state.action;
    udon_export_ud_state[7] = state.flags;
    // Velocity splits "physics computed the wrong speed" from "position did
    // not integrate": vel[0] is the sine-driven component, vel[2] the cosine
    // one, so a broken `sins` shows up here first.
    udon_export_ud_state[8] = state.velocity[0];
    udon_export_ud_state[9] = state.velocity[1];
    udon_export_ud_state[10] = state.velocity[2];
    udon_export_ud_state[11] = state.animID;
    udon_export_ud_state[12] = state.animFrame;
    udon_export_ud_geo_count = buffers.numTrianglesUsed;
}

// --- entries --------------------------------------------------------------

// The boot handshake is three steps, and each entry below is one of them.
//
// The writer inlines its init block (allocate `__mem`, copy the data image,
// build the stacks) into the prologue of *every* entry point, guarded by
// `__inited`. The ROM window lives inside `__mem`, so the host cannot copy the
// ROM in until some entry has run and created it -- and that entry must not
// itself read the ROM.
//
//   `prepare`  run the init block and touch nothing else.
//   (host)     copy the ROM into the `ud_rom` window.
//   `boot`     initialise libsm64 from the window and spawn Mario.
//
// `_start` is a no-op too: Udon may call it on its own schedule, and it must
// not race the handshake. It is exported only so a behaviour whose `_start`
// fires before the host's first frame still creates `__mem`.
int udon_event_start(void)
{
    return 0;
}

int udon_event_prepare(void)
{
    return 0;
}

int udon_event_boot(void)
{
    if (s_marioId >= 0)
        return 0;  // already booted

    uint8_t *texture = (uint8_t *)malloc(4u << 20);
    if (!texture)
        return -1;

    // The ROM arrives through the bridge: the host copies it into `__mem`
    // after `_start` created the memory array and before sending `boot`.
    // libsm64 reads it like any ROM, so textures and animations come from real
    // data. Without one (the window is still all zeros) the atlas stays zeroed
    // and a minimal animation table keeps the animated-part geometry from
    // indexing `values[-1]`.
    uint8_t *rom = (uint8_t *)udon_export_ud_rom;
    // A ROM that is still all zeros means the host supplied none: the file's
    // first bytes are always a header, never zero.
    if (rom[0] != 0 || rom[1] != 0 || rom[2] != 0 || rom[3] != 0) {
        sm64_global_init(rom, texture);
    } else {
        memset(texture, 0, 4u << 20);
        static uint8_t fakeRom[0x004EC000 + 64];
        uint8_t *anim = fakeRom + 0x004EC000;
        anim[0] = 0; anim[1] = 0; anim[2] = 0; anim[3] = 1;
        anim[8] = 0;  anim[9] = 0;  anim[10] = 0; anim[11] = 0x20;
        uint8_t *a = anim + 0x20;
        a[0] = 0; a[1] = 0;
        a[2] = 0; a[3] = 1;
        a[4] = 0; a[5] = 0;
        a[6] = 0; a[7] = 0;
        a[8] = 0; a[9] = 1;
        a[10] = 0; a[11] = 0;
        a[12] = 0; a[13] = 0; a[14] = 0; a[15] = 20;
        a[16] = 0; a[17] = 0; a[18] = 0; a[19] = 16;
        a[20] = 0; a[21] = 0; a[22] = 0; a[23] = 22;
        a[24] = 0; a[25] = 1; a[26] = 0; a[27] = 0;
        a[28] = 0; a[29] = 0;
        load_mario_anims_from_rom(fakeRom);
        memory_init();
    }
    sm64_static_surfaces_load(kSurfaces, 6);

    s_marioId = sm64_mario_create(0, 300, 0);
    if (s_marioId < 0)
        return -2;

    memset(udon_export_ud_input, 0, sizeof(udon_export_ud_input));
    tick_once();
    return 0;
}

int udon_event_tick(void)
{
    if (s_marioId < 0)
        return -1;
    tick_once();
    return 0;
}

/* Test entry: run several ticks with forward input and report the final
 * position and triangle count. Requires `boot` to have run first. */
int udon_event_report(void)
{
    if (s_marioId < 0)
        return -1;
    udon_export_ud_input[0] = 1.0f;   /* stickX: forward */
    udon_export_ud_input[1] = 0.0f;
    udon_export_ud_input[2] = 1.0f;   /* buttonA held */
    for (int i = 0; i < 30; i++)
        tick_once();
    int x = (int)udon_export_ud_state[0];
    int y = (int)udon_export_ud_state[1];
    int z = (int)udon_export_ud_state[2];
    int t = (int)udon_export_ud_geo_count;
    return (t % 1000) + ((x % 100) * 1000) + ((z % 100) * 100000);
}