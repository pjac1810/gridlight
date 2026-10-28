/* SPDX-License-Identifier: GPL-3.0-only */
/* GRIDLIGHT's clock and its six layer modes, all in audio(): loop() only queues what the hands do (seq_key ...),
 * so the song is only ever changed on the audio side and the sound never races the panel.
 *
 *   SCORE   (layers 1-7)   columns are time, rows are pitch; the loop runs from its start to its end column
 *   RANDOM  (8-11)         a light travels from note to note in the order they were put in; the farther, the later
 *   DRAW    (12-13)        presses recorded in time (REC) and played back with the loop
 *   BOUNCE  (14)           a ball drops from the height set in each column and sounds when it hits the floor
 *   PUSH    (15)           a held note grows brighter; held longer than a second, it stays (press it to stop it)
 *   SOLO    (16)           a held note repeats: the higher the row, the faster (the bottom row: once) */
#include "fm1.h"
#include "tn.h"

block_t BLK[BLOCKS] POOL;
song_t SONG = {120, 0, 0, 0, 40, 20, 0, 0, 0};

volatile uint8_t seq_playing, seq_rec, seq_cur_block, seq_view_layer;
volatile uint8_t seq_pos[LAYERS], seq_ball[N], seq_rnd_x, seq_rnd_y, seq_push_lvl;
volatile uint16_t seq_push_cell;
volatile uint8_t seq_flash[LAYERS][N * N];
volatile uint16_t seq_dirty;

const char *const MODE_NAME[6] = {"SCORE", "RANDOM", "DRAW", "BOUNCE", "PUSH", "SOLO"};
int layer_mode(int l) { return l < 7 ? MD_SCORE : l < 11 ? MD_RANDOM : l < 13 ? MD_DRAW : l == 13 ? MD_BOUNCE : l == 14 ? MD_PUSH : MD_SOLO; }

/* ---- scales */
const char *const SCALE_NAME[SCALES] = {"MAJOR", "MINOR", "HARMONIC", "DORIAN", "PENTATONIC", "MINOR PENTA", "BLUES", "JAPANESE", "CHROMATIC"};
static const uint8_t SCL[SCALES][12] = {{0, 2, 4, 5, 7, 9, 11}, {0, 2, 3, 5, 7, 8, 10}, {0, 2, 3, 5, 7, 8, 11}, {0, 2, 3, 5, 7, 9, 10},
                                        {0, 2, 4, 7, 9},        {0, 3, 5, 7, 10},       {0, 3, 5, 6, 7, 10},   {0, 1, 5, 7, 8},
                                        {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}};
static const uint8_t SCL_N[SCALES] = {7, 7, 7, 7, 5, 5, 6, 5, 12};

static int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

int row_note(const layer_t *L, int r) {
  int s = clampi(SONG.scale, 0, SCALES - 1), n = SCL_N[s];
  r = clampi(r, 0, 15);
  return clampi(48 + L->oct * 12 + SONG.transpose + (r / n) * 12 + SCL[s][r % n], 0, 120);
}

/* the layers' sounds at first: 1-7 piano, marimba, celesta, harp, bass, drums, pluck; 8-11 bells; 12-13 keys;
 * 14 steel drum; 15 pad; 16 sine lead */
static const uint8_t DEF_SOUND[LAYERS] = {0, 4, 2, 10, 22, TONES + 1, 9, 6, 3, 5, 7, 1, 15, 8, 16, 14};
void layer_reset(layer_t *L, int l) {
  *L = (layer_t){0};
  L->sound = DEF_SOUND[l & 15], L->len = (l == 14 || l == 15) ? 9 : 5, L->oct = l == 4 ? -1 : (l >= 7 && l <= 10) ? 1 : 0;
  L->vol = 100, L->lstart = 0, L->lend = 15, L->speed = 2, L->pan = 0;
}

/* ---- time: 96 ticks a beat */
static const uint16_t STEP_T[4] = {96, 48, 24, 12};
static const uint16_t GATE_MS[16] = {20, 40, 60, 80, 120, 160, 240, 320, 480, 640, 960, 1280, 1920, 2560, 3840, 5000};
static const uint8_t SOLO_T[16] = {0, 96, 72, 48, 36, 32, 24, 18, 16, 12, 9, 8, 6, 4, 3, 2}; /* repeat every n ticks */
static uint32_t tick_acc, ticks;

typedef struct {
  uint16_t tk;           /* ticks since the step (SCORE), or into the loop (DRAW) */
  uint8_t ri;            /* RANDOM: the note last reached */
  uint16_t rprog, rlen;  /* and how far towards the next, of how far */
  uint16_t bph[N];       /* BOUNCE: each ball's phase, ticks */
  uint16_t solo, push;   /* SOLO: columns held; PUSH: the note held, + 1 (row * 16 + column) */
  uint8_t solo_rate[N];
  uint32_t push_t;       /* PUSH: ticks held */
} run_t;
static run_t RUN[LAYERS];

/* ---- notes, and when they end */
#define OFFS 64
static struct {
  uint8_t layer, note;
  int32_t left; /* frames */
} off[OFFS];

static void sound(int l, int x, int y, int gate_frames) {
  layer_t *L = &BLK[seq_cur_block].layer[l];
  int md = layer_mode(l), idx = (md == MD_BOUNCE || md == MD_SOLO) ? x : y;
  int drum = L->sound >= TONES, note = drum ? idx : row_note(L, idx);
  seq_flash[l][(y & 15) * N + (x & 15)] = 6;
  if (SONG.mute >> l & 1u) return;
  snd_note_on(l, L->sound, note, 100, L->pan, L->vol);
  midi_send((uint8_t)(0x90 | l), (uint8_t)(drum ? 36 + idx : note), 100);
  if (gate_frames <= 0) return; /* held: its own note-off */
  for (int i = 0; i < OFFS; i++)
    if (off[i].left <= 0) {
      off[i].layer = (uint8_t)l, off[i].note = (uint8_t)(drum ? 36 + idx : note), off[i].left = gate_frames;
      if (drum) off[i].note |= 0x80u; /* only MIDI to end */
      return;
    }
}
static int gate(const layer_t *L) { return GATE_MS[L->len & 15] * 441 / 10; }
static void release(int l, int x, int y) {
  layer_t *L = &BLK[seq_cur_block].layer[l];
  int note = L->sound >= TONES ? 36 + y : row_note(L, y);
  (void)x;
  if (L->sound < TONES) snd_note_off(l, note);
  midi_send((uint8_t)(0x80 | l), (uint8_t)note, 0);
}

/* ---- the queue from loop() */
enum { EV_KEY, EV_PLAY, EV_BLOCK, EV_CLEAR, EV_NOTE, EV_COPY };
typedef struct {
  uint8_t type, layer, on;
  int8_t x, y;
} ev_t;
#define QN 64
static ev_t q[QN];
static volatile uint32_t q_wr, q_rd;
static void push_ev(ev_t e) {
  if (q_wr - q_rd >= QN) return;
  q[q_wr % QN] = e;
  RING_PUBLISH();
  q_wr = q_wr + 1;
}
void seq_key(int layer, int x, int y, int on) { push_ev((ev_t){EV_KEY, (uint8_t)layer, (uint8_t)on, (int8_t)x, (int8_t)y}); }
void seq_play(int on) { push_ev((ev_t){EV_PLAY, 0, (uint8_t)on, 0, 0}); }
void seq_block(int b) { push_ev((ev_t){EV_BLOCK, (uint8_t)b, 0, 0, 0}); }
void seq_clear(int layer) { push_ev((ev_t){EV_CLEAR, (uint8_t)layer, 0, 0, 0}); }
void seq_midi(int layer, int note, int on) { push_ev((ev_t){EV_NOTE, (uint8_t)layer, (uint8_t)on, (int8_t)(note & 0x7F), 0}); }
void seq_copy(int to) { push_ev((ev_t){EV_COPY, (uint8_t)to, 0, 0, 0}); }

static int next_block = -1;

static void restart(void) {
  for (int l = 0; l < LAYERS; l++) {
    layer_t *L = &BLK[seq_cur_block].layer[l];
    run_t *R = &RUN[l];
    R->tk = 0, R->ri = 0, R->rprog = 0, R->rlen = 0;
    seq_pos[l] = (uint8_t)(L->lstart <= L->lend ? L->lstart : L->lend);
    for (int c = 0; c < N; c++) R->bph[c] = 0;
  }
}

static void all_off(void) {
  for (int l = 0; l < LAYERS; l++) snd_layer_off(l);
  for (int i = 0; i < OFFS; i++)
    if (off[i].left > 0) {
      off[i].left = 0;
      midi_send((uint8_t)(0x80 | off[i].layer), off[i].note & 0x7Fu, 0);
    }
}

static int contains(const layer_t *L, int cell) {
  for (int i = 0; i < L->n; i++)
    if (L->cell[i] == cell) return i;
  return -1;
}

static void key(int l, int x, int y, int on) {
  layer_t *L = &BLK[seq_cur_block].layer[l];
  run_t *R = &RUN[l];
  x = clampi(x < 0 ? seq_pos[l] : x, 0, 15), y = clampi(y, 0, 15);
  int cell = y * N + x;
  switch (layer_mode(l)) {
    case MD_SCORE:
      if (!on) break;
      L->grid[x] ^= (uint16_t)(1u << y);
      if (L->grid[x] >> y & 1u) sound(l, x, y, gate(L));
      break;
    case MD_RANDOM: {
      if (!on) break;
      int i = contains(L, cell);
      if (i >= 0) { /* off again */
        for (int j = i; j + 1 < L->n; j++) L->cell[j] = L->cell[j + 1];
        L->n--;
        if (R->ri >= L->n) R->ri = 0;
      } else if (L->n < MAXPT) {
        L->cell[L->n++] = (uint8_t)cell;
        sound(l, x, y, gate(L));
      }
      break;
    }
    case MD_DRAW:
      if (!on) break;
      sound(l, x, y, gate(L));
      if (seq_rec && seq_playing && L->n < MAXPT) L->cell[L->n] = (uint8_t)cell, L->tick[L->n] = R->tk, L->n++;
      break;
    case MD_BOUNCE:
      if (!on) break;
      L->grid[x] = (uint16_t)y, R->bph[x] = 0; /* the ball starts at the top; the bottom row: no ball */
      break;
    case MD_PUSH:
      if (on) {
        if (L->grid[x] >> y & 1u) { /* a note that stayed: stop it */
          L->grid[x] &= (uint16_t)~(1u << y);
          release(l, x, y);
          break;
        }
        if (R->push) release(l, (R->push - 1) % N, (R->push - 1) / N);
        R->push = (uint16_t)(cell + 1), R->push_t = 0;
        sound(l, x, y, 0);
      } else if (R->push == cell + 1) {
        if (R->push_t > 192) L->grid[x] |= (uint16_t)(1u << y); /* held two beats: it stays */
        else release(l, x, y);
        R->push = 0;
      }
      break;
    default: /* SOLO */
      if (on) {
        sound(l, x, y, gate(L));
        if (y > 0) R->solo |= (uint16_t)(1u << x), R->solo_rate[x] = SOLO_T[y];
      } else {
        R->solo &= (uint16_t)~(1u << x);
      }
      break;
  }
  seq_dirty++;
}

static void do_events(void) {
  while (q_rd != q_wr) {
    ev_t e = q[q_rd % QN];
    q_rd = q_rd + 1;
    switch (e.type) {
      case EV_KEY: key(e.layer & 15, e.x, e.y, e.on); break;
      case EV_PLAY:
        if (e.on && !seq_playing) restart();
        if (!e.on) all_off();
        seq_playing = e.on;
        break;
      case EV_BLOCK:
        if (seq_playing) next_block = e.layer & 15;
        else all_off(), seq_cur_block = e.layer & 15, restart();
        break;
      case EV_NOTE: { /* MIDI in, channel = layer: its sound */
        layer_t *L = &BLK[seq_cur_block].layer[e.layer & 15];
        if (e.on) snd_note_on(e.layer & 15, L->sound, L->sound >= TONES ? (e.x - 36) & 15 : e.x, 100, L->pan, L->vol);
        else if (L->sound < TONES) snd_note_off(e.layer & 15, e.x);
        break;
      }
      case EV_COPY:
        BLK[e.layer & 15] = BLK[seq_cur_block];
        seq_dirty++;
        break;
      default: {
        layer_t *L = &BLK[seq_cur_block].layer[e.layer & 15];
        for (int c = 0; c < N; c++) L->grid[c] = 0;
        L->n = 0;
        snd_layer_off(e.layer & 15);
        seq_dirty++;
        break;
      }
    }
  }
}

/* ---- one tick (1/96 beat) of every layer */
static void tick(void) {
  block_t *B = &BLK[seq_cur_block];
  ticks++;
  if (next_block >= 0 && ticks % 96 == 0) { /* a block change waits for the beat */
    all_off();
    seq_cur_block = (uint8_t)next_block, next_block = -1;
    restart();
    B = &BLK[seq_cur_block];
  }
  for (int l = 0; l < LAYERS; l++) {
    layer_t *L = &B->layer[l];
    run_t *R = &RUN[l];
    int md = layer_mode(l), st = STEP_T[L->speed & 3];
    if (md == MD_SOLO) { /* live, playing or not: quantised to the clock */
      for (int c = 0; c < N; c++)
        if ((R->solo >> c & 1u) && R->solo_rate[c] && ticks % R->solo_rate[c] == 0) {
          int y = 0;
          for (int r = 1; r < 16; r++)
            if (SOLO_T[r] == R->solo_rate[c]) y = r;
          sound(l, c, y, gate(L));
        }
      continue;
    }
    if (md == MD_PUSH) {
      if (R->push) R->push_t++;
      int lvl = R->push ? clampi((int32_t)R->push_t / 3, 0, 127) : 0;
      int latched = 0;
      for (int c = 0; c < N; c++) latched |= L->grid[c] != 0;
      if (!R->push && latched) lvl = 127;
      snd_push(l, lvl);
      if (l == seq_view_layer || R->push) seq_push_lvl = (uint8_t)lvl;
      seq_push_cell = R->push;
      continue;
    }
    if (!seq_playing) continue;
    int lo = L->lstart <= L->lend ? L->lstart : L->lend, hi = L->lstart <= L->lend ? L->lend : L->lstart;
    switch (md) {
      case MD_SCORE: {
        int sw = (st * clampi(SONG.swing, 0, 100)) / 300, need = (seq_pos[l] - lo) % 2 ? st - sw : st + sw;
        if (R->tk == 0) {
          uint16_t col = L->grid[seq_pos[l] & 15];
          for (int y = 0; y < 16; y++)
            if (col >> y & 1u) sound(l, seq_pos[l], y, gate(L));
        }
        if (++R->tk >= need) {
          R->tk = 0;
          seq_pos[l] = (uint8_t)(seq_pos[l] >= hi || seq_pos[l] < lo ? lo : seq_pos[l] + 1);
        }
        break;
      }
      case MD_DRAW: {
        for (int i = 0; i < L->n; i++)
          if (L->tick[i] == R->tk) sound(l, L->cell[i] % N, L->cell[i] / N, gate(L));
        if (++R->tk >= 16 * st) R->tk = 0;
        seq_pos[l] = (uint8_t)(R->tk / st);
        break;
      }
      case MD_RANDOM: {
        if (!L->n) break;
        if (R->ri >= L->n) R->ri = 0;
        int a = L->cell[R->ri], b = L->cell[(R->ri + 1) % L->n];
        if (!R->rlen) {
          int dx = a % N - b % N, dy = a / N - b / N;
          dx = dx < 0 ? -dx : dx, dy = dy < 0 ? -dy : dy;
          R->rlen = (uint16_t)((dx > dy ? dx : dy) * st), R->rprog = 0;
          if (!R->rlen) R->rlen = (uint16_t)st;
        }
        if (++R->rprog >= R->rlen) {
          R->ri = (uint8_t)((R->ri + 1) % L->n), R->rlen = 0;
          sound(l, b % N, b / N, gate(L));
        }
        if (l == seq_view_layer && R->rlen) {
          seq_rnd_x = (uint8_t)((a % N) * 16 + ((b % N) - (a % N)) * 16 * R->rprog / R->rlen);
          seq_rnd_y = (uint8_t)((a / N) * 16 + ((b / N) - (a / N)) * 16 * R->rprog / R->rlen);
        }
        /* SPIN: the shape turns a quarter now and then, and its notes with it */
        if (SONG.spin && ticks % (uint32_t)(16 * st >> (SONG.spin - 1)) == 0)
          for (int i = 0; i < L->n; i++) {
            int x = L->cell[i] % N, y = L->cell[i] / N;
            L->cell[i] = (uint8_t)(x * N + (15 - y));
          }
        break;
      }
      default: { /* BOUNCE: per column, a ball from its height down to the floor and back */
        for (int c = 0; c < N; c++) {
          int h = L->grid[c] & 15;
          if (!h) {
            if (l == 13) seq_ball[c] = 255;
            continue;
          }
          int per = 2 * h * st;
          R->bph[c] = (uint16_t)((R->bph[c] + 1) % per);
          int t = R->bph[c] / st, row = t <= h ? h - t : t - h;
          if (l == 13) seq_ball[c] = (uint8_t)row;
          if (R->bph[c] == h * st) sound(l, c, 0, gate(L));
        }
        break;
      }
    }
  }
}

void seq_render(int32_t *out, uint32_t frames) {
  do_events();
  /* the note-offs due in this block */
  for (int i = 0; i < OFFS; i++)
    if (off[i].left > 0 && (off[i].left -= (int32_t)frames) <= 0) {
      int note = off[i].note & 0x7F;
      if (!(off[i].note & 0x80u)) snd_note_off(off[i].layer, note);
      midi_send((uint8_t)(0x80 | off[i].layer), (uint8_t)note, 0);
    }
  /* the clock, a few samples at a time */
  uint32_t inc = (uint32_t)clampi(SONG.tempo, 40, 240) * 96u * 65536u / 2646000u;
  for (uint32_t i = 0; i < frames; i += 16) {
    tick_acc += inc * 16u;
    while (tick_acc >= 65536u) tick_acc -= 65536u, tick();
  }
  snd_render(out, frames);
}
