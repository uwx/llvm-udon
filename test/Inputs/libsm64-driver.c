// The driver `scripts/build-libsm64.sh` links against libsm64.
//
// It exercises the library the way a host does, and checks the observable
// results:
//
//   1. `sm64_global_init` loads Mario's textures and animations into the
//      out-texture buffer and allocates the display-list pool. With no ROM every
//      read lands in the zeroed part of `__mem`, so the buffer holds the
//      library's own writes rather than ROM data -- but it holds *something*,
//      which is what proves the loaders ran and the heap works.
//   2. `sm64_mario_create` reaches `init_mario`, which returns -1 exactly when
//      `find_floor` found no floor. With no level loaded there is no collision
//      data, so -1 is the correct answer and confirms the whole spawn path ran.
//   3. Ticking Mario without a level is not meaningful, so it is not attempted:
//      a null floor is a level-loading concern, not a compiler one.
//
// The return value packs two measurements of the texture buffer: the low two
// digits are the number of non-zero bytes modulo 100, and the rest is the byte
// sum modulo 1000. `ret _start = 50536` is the measured value -- 50536 is
// 505 * 100 + 36, so 505 for the sum and 36 written bytes out of 100 slots.

#include <stdint.h>

extern void sm64_global_init(const uint8_t *rom, uint8_t *outTexture);
extern int32_t sm64_mario_create(float x, float y, float z);

#define TEXTURE_BYTES (4u << 20)

int udon_event_start(void) {
  uint8_t *texture = (uint8_t *)malloc(TEXTURE_BYTES);
  if (!texture)
    return -1;

  sm64_global_init(0, texture);

  unsigned sum = 0;
  unsigned nonZero = 0;
  for (unsigned i = 0; i < TEXTURE_BYTES; i++) {
    sum += texture[i];
    nonZero += texture[i] != 0;
  }

  // Mario's spawn path runs, and reports "no floor" because no level is loaded.
  int32_t mario = sm64_mario_create(0.0f, 100.0f, 0.0f);
  if (mario != -1)
    return -2;

  // Both halves of the answer must be non-trivial, or the check proves nothing:
  // an untouched buffer would sum to zero and a skipped spawn path would return
  // zero instead of -1.
  if (sum == 0 || nonZero == 0)
    return -3;

  // Report both measurements in one number: the byte sum (bounded), and the
  // count of written bytes, which is what the texture loaders produced.
  return (int)(sum % 1000) * 100 + (int)(nonZero % 100);
}
