/* SPDX-License-Identifier: GPL-3.0-only */
/* Host test for GRIDLIGHT: a song played through the six modes into build/render-out (levels, timing of the
 * SCORE loop), every sound once, then a fuzz of keys, knobs and blocks, under UBSan.   tests/run.sh */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tn.h"

int32_t sine(uint32_t phase) {
  int32_t x = (int32_t)phase >> 16, ax = x < 0 ? -x : x, y, ay;
  y = (x * (32768 - ax)) >> 13;
  ay = y < 0 ? -y : y;
  y += ((((y * ay) >> 15) - y) * 7373) >> 15;
  return y > 32767 ? 32767 : y < -32767 ? -32767 : y;
}
static const uint32_t INC_C9[12] = {815363807u, 863847862u, 915214929u, 969636441u, 1027294024u, 1088380105u,
                                    1153098554u, 1221665363u, 1294309365u, 1371273005u, 1452813141u, 1539201906u};
uint32_t note_inc(int note) {
  if (note < 0) note = 0;
  if (note > 127) note = 127;
  return INC_C9[note % 12] >> (10 - note / 12);
}
static uint32_t rs = 0x1234567u;
uint32_t random32(void) {
  rs ^= rs << 13, rs ^= rs >> 17, rs ^= rs << 5;
  return rs;
}
static int notes_out;
void midi_send(uint8_t s, uint8_t a, uint8_t b) {
  (void)a, (void)b;
  if ((s & 0xF0) == 0x90) notes_out++;
}

#define MAXF (44100 * 40)
static int16_t buf[MAXF * 2];
static uint32_t pos;
static int fail;

static void render(uint32_t frames) {
  static int32_t blk[512];
  while (frames) {
    uint32_t n = frames > 256 ? 256 : frames;
    memset(blk, 0, sizeof blk);
    seq_render(blk, n);
    for (uint32_t i = 0; i < 2 * n; i++) {
      if (blk[i] > 32767 || blk[i] < -32767) fail = 1;
      if (pos * 2 + i < MAXF * 2) buf[pos * 2 + i] = (int16_t)blk[i];
    }
    pos += n, frames -= n;
  }
}
static void save(const char *dir, const char *name) {
  char path[600];
  snprintf(path, sizeof path, "%s/%s.wav", dir, name);
  FILE *f = fopen(path, "wb");
  uint32_t frames = pos < MAXF ? pos : MAXF, data = frames * 4, riff = 36 + data, rate = 44100, br = rate * 4, fmt = 16;
  uint16_t pcm = 1, ch = 2, al = 4, bits = 16;
  fwrite("RIFF", 1, 4, f), fwrite(&riff, 4, 1, f), fwrite("WAVEfmt ", 1, 8, f), fwrite(&fmt, 4, 1, f);
  fwrite(&pcm, 2, 1, f), fwrite(&ch, 2, 1, f), fwrite(&rate, 4, 1, f), fwrite(&br, 4, 1, f);
  fwrite(&al, 2, 1, f), fwrite(&bits, 2, 1, f), fwrite("data", 1, 4, f), fwrite(&data, 4, 1, f);
  fwrite(buf, 2, frames * 2, f);
  fclose(f);
  double sq = 0;
  int peak = 0, clip = 0;
  for (uint32_t i = 0; i < frames * 2; i++) {
    int v = abs(buf[i]);
    sq += (double)v * v, peak = v > peak ? v : peak, clip += v >= 32700;
  }
  printf("%-14s %5.1f s  peak %6d rms %6.0f clip %.3f%%\n", name, frames / 44100.0, peak, sqrt(sq / (frames * 2)), 100.0 * clip / (frames * 2));
  pos = 0;
}

int main(int argc, char **argv) {
  const char *dir = argc > 1 ? argv[1] : ".";
  for (int b = 0; b < BLOCKS; b++)
    for (int l = 0; l < LAYERS; l++) layer_reset(&BLK[b].layer[l], l);
  snd_init();
  SONG.reverb = 40, SONG.chorus = 20, SONG.tempo = 120;
  /* SCORE: a C major arpeggio on layer 1, drums on layer 6 */
  static const int AR[8] = {7, 9, 11, 14, 11, 9, 7, 4};
  for (int i = 0; i < 8; i++) seq_key(0, i * 2, AR[i], 1);
  for (int c = 0; c < 16; c += 4) seq_key(5, c, 0, 1);
  seq_key(5, 4, 1, 1), seq_key(5, 12, 1, 1);
  for (int c = 2; c < 16; c += 4) seq_key(5, c, 2, 1);
  /* RANDOM: four notes; BOUNCE: three balls */
  seq_key(7, 2, 12, 1), seq_key(7, 9, 14, 1), seq_key(7, 13, 9, 1), seq_key(7, 5, 10, 1);
  seq_key(13, 3, 4, 1), seq_key(13, 7, 6, 1), seq_key(13, 10, 3, 1);
  render(64);
  notes_out = 0;
  seq_play(1);
  clock_t t0 = clock();
  render(44100 * 8);
  double ms = (clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
  printf("song: %d notes in 8 s (%.0f ms CPU)  ", notes_out, ms);
  save(dir, "1-song");
  /* SCORE timing: at 120 BPM, sixteenths, the arpeggio's 8 notes + the 10 drum hits per bar, 4 bars */
  /* DRAW: record a few presses while playing, then hear them back */
  seq_rec = 1;
  for (int i = 0; i < 6; i++) seq_key(11, i * 2, 6 + i, 1), render(5512), seq_key(11, i * 2, 6 + i, 0);
  seq_rec = 0;
  render(44100 * 4);
  printf("draw: %d notes recorded  ", BLK[0].layer[11].n);
  if (BLK[0].layer[11].n != 6) fail = 1, printf("WRONG ");
  save(dir, "2-draw");
  /* PUSH: held three seconds (it stays), then pressed again (it stops); SOLO: a fast repeat */
  seq_play(0), render(4410);
  seq_key(14, 4, 9, 1), render(44100 * 3), seq_key(14, 4, 9, 0), render(44100);
  int stayed = BLK[0].layer[14].grid[4] >> 9 & 1u;
  seq_key(14, 4, 9, 1), seq_key(14, 4, 9, 0), render(44100);
  printf("push: %s  ", stayed && !(BLK[0].layer[14].grid[4] >> 9 & 1u) ? "ok" : "WRONG");
  if (!stayed) fail = 1;
  seq_key(15, 7, 12, 1), render(44100 * 2), seq_key(15, 7, 12, 0), render(22050);
  save(dir, "3-push-solo");
  /* every sound: a phrase */
  for (int s = 0; s < SOUNDS; s++) {
    BLK[0].layer[2].sound = (uint8_t)s;
    for (int i = 0; i < 4; i++) seq_key(2, 0, (s >= TONES ? i * 3 : 4 + i * 3), 1), render(6000);
    seq_clear(2), render(14000);
  }
  save(dir, "4-sounds");
  /* fuzz */
  srand(3);
  for (int b = 0; b < 30000; b++) {
    int r = rand() % 10;
    if (r < 4) seq_key(rand() % LAYERS, rand() % 17 - 1, rand() % 16, rand() & 1);
    else if (r == 4) seq_play(rand() & 1);
    else if (r == 5) seq_block(rand() % BLOCKS);
    else if (r == 6 && rand() % 20 == 0) seq_clear(rand() % LAYERS);
    else if (r == 7) {
      layer_t *L = &BLK[rand() % BLOCKS].layer[rand() % LAYERS];
      L->sound = (uint8_t)(rand() % SOUNDS), L->len = (uint8_t)(rand() % 16), L->oct = (int8_t)(rand() % 11 - 5);
      L->lstart = (uint8_t)(rand() % 16), L->lend = (uint8_t)(rand() % 16), L->speed = (uint8_t)(rand() % 4), L->pan = (int8_t)(rand() % 128 - 64);
    } else if (r == 8) {
      SONG.tempo = (uint8_t)(40 + rand() % 201), SONG.transpose = (int8_t)(rand() % 16 - 7), SONG.scale = (uint8_t)(rand() % SCALES);
      SONG.swing = (uint8_t)(rand() % 101), SONG.spin = (uint8_t)(rand() % 5), seq_rec = (uint8_t)(rand() & 1);
    } else seq_midi(rand() % 16, rand() % 128, rand() & 1);
    render(1 + rand() % 256);
    pos = 0;
  }
  printf("fuzz: 30000 blocks, %s\n", fail ? "FAILED" : "ok");
  return fail;
}
