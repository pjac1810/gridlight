/* SPDX-License-Identifier: GPL-3.0-only */
/* GRIDLIGHT: a 16 x 16 light-grid sequencer for the M-VAVE FM-1, after the Yamaha TENORI-ON (README.md has the map).
 *
 * The FM-1 has no grid to touch: one line of the grid is lit at the edge, and its 16 cells are the 16 white
 * keys. SCORE, RANDOM, DRAW, PUSH: a column (the keys are its rows, low to high); BOUNCE, SOLO: a row (the keys are
 * its columns, left to right).
 *   SELECT, OCT- OCT+   move the line            PRESETS  the layer 1..16 (its mode comes with it)
 *   ALGORITHM           the block 1..16          PLAY  play / stop      REC  record (SCORE live, DRAW)
 *   HOME  LAYER: SOUND, LENGTH, OCTAVE, VOLUME   ENV  LOOP: START, END, SPEED, PAN
 *   LFO   SONG: TEMPO, TRANSPOSE, SCALE, SWING   FX   EFFECTS: REVERB, CHORUS, SPIN, LIGHT
 *   SEL   clear the layer (held 1 s: the block)  GLO  mute the layer   SEQ  every layer's notes at once
 *   EDIT  copy the block into the next one       ARP  white / amber lights   SAVE  keep the song
 *   MIDI  in: channel n plays layer n's sound; out: every layer's notes on its channel */
#include "fm1.h"
#include "tn.h"

static int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }
static char *put(char *d, const char *s) {
  while (*s) *d++ = *s++;
  *d = 0;
  return d;
}
static char *put_int(char *d, int32_t v) {
  char b[12];
  format_int(b, v);
  return put(d, b);
}
static char *put_2(char *d, int v) { /* 01..16 */
  *d++ = (char)('0' + v / 10 % 10), *d++ = (char)('0' + v % 10), *d = 0;
  return d;
}

#define C_BG RGB(10, 10, 12)
#define C_BAR RGB(30, 30, 34)
#define C_TEXT RGB(230, 230, 235)
#define C_DIM RGB(120, 120, 128)
#define C_MARK RGB(255, 150, 40)
#define C_REC RGB(240, 60, 50)

/* ---------------------------------------------------------------- the song in flash */

#define STORE_MAGIC 0x474C5431u /* "GLT1" */
static uint8_t store[SETTINGS_MAX];
static uint32_t save_len;

static int layer_default(const layer_t *L, int l) {
  layer_t d;
  layer_reset(&d, l);
  if (L->sound != d.sound || L->len != d.len || L->oct != d.oct || L->vol != d.vol || L->lstart != d.lstart || L->lend != d.lend ||
      L->speed != d.speed || L->pan != d.pan || L->n)
    return 0;
  for (int c = 0; c < N; c++)
    if (L->grid[c]) return 0;
  return 1;
}

/* magic, the song's settings, then each layer that is not as it started: block << 4 | layer, its 8 settings, a
 * flag, its grid (flag 1), its notes (n, cells, and DRAW's ticks); 255 at the end. 0: it did not fit */
static int song_save(void) {
  uint32_t k = 0;
  uint32_t m = STORE_MAGIC;
  for (int i = 0; i < 4; i++) store[k++] = (uint8_t)(m >> (8 * i));
  const uint8_t *sp = (const uint8_t *)&SONG;
  for (uint32_t i = 0; i < sizeof SONG; i++) store[k++] = sp[i];
  for (int b = 0; b < BLOCKS; b++)
    for (int l = 0; l < LAYERS; l++) {
      const layer_t *L = &BLK[b].layer[l];
      if (layer_default(L, l)) continue;
      int grid = 0;
      for (int c = 0; c < N; c++) grid |= L->grid[c] != 0;
      uint32_t need = 1 + 8 + 1 + (grid ? 32u : 0u) + 1 + L->n * (layer_mode(l) == MD_DRAW ? 3u : 1u) + 1;
      if (k + need > SETTINGS_MAX) return 0;
      store[k++] = (uint8_t)(b << 4 | l);
      store[k++] = L->sound, store[k++] = L->len, store[k++] = (uint8_t)L->oct, store[k++] = L->vol;
      store[k++] = L->lstart, store[k++] = L->lend, store[k++] = L->speed, store[k++] = (uint8_t)L->pan;
      store[k++] = (uint8_t)grid;
      if (grid)
        for (int c = 0; c < N; c++) store[k++] = (uint8_t)L->grid[c], store[k++] = (uint8_t)(L->grid[c] >> 8);
      store[k++] = L->n;
      for (int i = 0; i < L->n; i++) store[k++] = L->cell[i];
      if (layer_mode(l) == MD_DRAW)
        for (int i = 0; i < L->n; i++) store[k++] = (uint8_t)L->tick[i], store[k++] = (uint8_t)(L->tick[i] >> 8);
    }
  store[k++] = 255;
  save_len = k;
  return settings_save(store, k) == 0;
}

static int song_load(void) {
  if (settings_load(store, SETTINGS_MAX)) return 0;
  uint32_t m = (uint32_t)store[0] | (uint32_t)store[1] << 8 | (uint32_t)store[2] << 16 | (uint32_t)store[3] << 24;
  if (m != STORE_MAGIC) return 0;
  uint32_t k = 4;
  uint8_t *sp = (uint8_t *)&SONG;
  for (uint32_t i = 0; i < sizeof SONG; i++) sp[i] = store[k++];
  SONG.tempo = (uint8_t)clampi(SONG.tempo, 40, 240), SONG.scale %= SCALES, SONG.spin %= 5, SONG.theme &= 1;
  for (int b = 0; b < BLOCKS; b++)
    for (int l = 0; l < LAYERS; l++) layer_reset(&BLK[b].layer[l], l);
  while (k < SETTINGS_MAX - 12 && store[k] != 255) {
    int b = store[k] >> 4, l = store[k] & 15;
    layer_t *L = &BLK[b].layer[l];
    k++;
    L->sound = (uint8_t)(store[k++] % SOUNDS), L->len = store[k++] & 15, L->oct = (int8_t)clampi((int8_t)store[k++], -5, 5);
    L->vol = store[k++] & 127, L->lstart = store[k++] & 15, L->lend = store[k++] & 15, L->speed = store[k++] & 3;
    L->pan = (int8_t)clampi((int8_t)store[k++], -64, 63);
    if (store[k++] && k + 32 < SETTINGS_MAX)
      for (int c = 0; c < N; c++) L->grid[c] = (uint16_t)(store[k] | store[k + 1] << 8), k += 2;
    L->n = (uint8_t)clampi(store[k++], 0, MAXPT);
    for (int i = 0; i < L->n && k < SETTINGS_MAX; i++) L->cell[i] = store[k++];
    if (layer_mode(l) == MD_DRAW)
      for (int i = 0; i < L->n && k + 1 < SETTINGS_MAX; i++) L->tick[i] = (uint16_t)(store[k] | store[k + 1] << 8), k += 2;
  }
  return 1;
}

/* a first song: block 1 has a little piece in it */
static void demo(void) {
  for (int b = 0; b < BLOCKS; b++)
    for (int l = 0; l < LAYERS; l++) layer_reset(&BLK[b].layer[l], l);
  layer_t *P = &BLK[0].layer[0], *M = &BLK[0].layer[1], *B = &BLK[0].layer[4], *D = &BLK[0].layer[5];
  static const uint8_t PIANO[][2] = {{0, 11}, {3, 9}, {6, 7}, {8, 12}, {11, 11}, {14, 9}};
  static const uint8_t MARIMBA[][2] = {{2, 14}, {7, 12}, {10, 14}, {13, 11}};
  static const uint8_t BASS[][2] = {{0, 0}, {3, 0}, {4, 5}, {7, 5}, {8, 3}, {11, 3}, {12, 4}, {14, 4}};
  for (unsigned i = 0; i < sizeof PIANO / 2; i++) P->grid[PIANO[i][0]] |= (uint16_t)(1u << PIANO[i][1]);
  for (unsigned i = 0; i < sizeof MARIMBA / 2; i++) M->grid[MARIMBA[i][0]] |= (uint16_t)(1u << MARIMBA[i][1]);
  for (unsigned i = 0; i < sizeof BASS / 2; i++) B->grid[BASS[i][0]] |= (uint16_t)(1u << BASS[i][1]);
  B->speed = 1;
  for (int c = 0; c < N; c += 4) D->grid[c] |= 1u;      /* kick */
  D->grid[4] |= 2u, D->grid[12] |= 2u;                   /* snare */
  for (int c = 2; c < N; c += 4) D->grid[c] |= 4u;      /* closed hat */
  D->grid[14] |= 8u, D->vol = 90;                        /* open hat */
  layer_t *R = &BLK[0].layer[7];
  static const uint8_t RND[] = {12 * 16 + 2, 14 * 16 + 9, 9 * 16 + 13, 10 * 16 + 5};
  for (unsigned i = 0; i < sizeof RND; i++) R->cell[R->n++] = RND[i];
  R->vol = 70;
  layer_t *O = &BLK[0].layer[13];
  O->grid[3] = 4, O->grid[7] = 6, O->grid[10] = 3, O->vol = 80;
  SONG = (song_t){112, 0, 0, 10, 46, 24, 0, 0, 0};
}

/* ---------------------------------------------------------------- the panel */

static uint8_t layer, cx = 0, cy = 8, page, show_all, held_cell[KEY_COUNT];
static uint32_t msg_until, sel_down, saved_t;
static uint8_t sel_done;
static char msg[32];

static const char *const PAGE_NAME[4] = {"LAYER", "LOOP", "SONG", "EFFECTS"};
static const char *const KNOB_NAME[4][4] = {
    {"SOUND", "LENGTH", "OCTAVE", "VOLUME"}, {"START", "END", "SPEED", "PAN"}, {"TEMPO", "TRANSP", "SCALE", "SWING"}, {"REVERB", "CHORUS", "SPIN", "LIGHT"}};
static const char *const SPEED_NAME[4] = {"1/4", "1/8", "1/16", "1/32"};
static const uint16_t GATE_MS_UI[16] = {20, 40, 60, 80, 120, 160, 240, 320, 480, 640, 960, 1280, 1920, 2560, 3840, 5000};

static void say(const char *a, const char *b, uint32_t now) {
  char *d = put(msg, a);
  if (b) d = put(put(d, " "), b);
  msg_until = now + 1500;
}

static layer_t *cur(void) { return &BLK[seq_cur_block].layer[layer]; }
static int col_line(void) { int m = layer_mode(layer); return m != MD_BOUNCE && m != MD_SOLO; }

static void knob(int k, int e, uint32_t now) {
  layer_t *L = cur();
  char b[24];
  int d = e > 0 ? 1 : -1, a = e < 0 ? -e : e;
  b[0] = 0;
  switch (page * 4 + k) {
    case 0:
      L->sound = (uint8_t)((L->sound + SOUNDS * 8 + e) % SOUNDS);
      put(put(put_2(b, L->sound + 1), " "), sound_name(L->sound));
      break;
    case 1: L->len = (uint8_t)clampi(L->len + d, 0, 15), put(put_int(b, GATE_MS_UI[L->len]), "ms"); break;
    case 2: L->oct = (int8_t)clampi(L->oct + d, -5, 5), put_int(put(b, L->oct > 0 ? "+" : ""), L->oct); break;
    case 3: L->vol = (uint8_t)clampi(L->vol + d * (a > 1 ? 4 * a : 1), 0, 127), put_int(b, L->vol); break;
    case 4: L->lstart = (uint8_t)clampi(L->lstart + d, 0, 15), put_int(b, L->lstart + 1); break;
    case 5: L->lend = (uint8_t)clampi(L->lend + d, 0, 15), put_int(b, L->lend + 1); break;
    case 6: L->speed = (uint8_t)clampi(L->speed + d, 0, 3), put(b, SPEED_NAME[L->speed]); break;
    case 7: L->pan = (int8_t)clampi(L->pan + d * (a > 1 ? 4 * a : 2), -64, 63), put_int(b, L->pan); break;
    case 8: SONG.tempo = (uint8_t)clampi(SONG.tempo + d * (a > 1 ? 2 * a : 1), 40, 240), put_int(b, SONG.tempo); break;
    case 9: SONG.transpose = (int8_t)clampi(SONG.transpose + d, -7, 8), put_int(put(b, SONG.transpose > 0 ? "+" : ""), SONG.transpose); break;
    case 10: SONG.scale = (uint8_t)clampi(SONG.scale + d, 0, SCALES - 1), put(b, SCALE_NAME[SONG.scale]); break;
    case 11: SONG.swing = (uint8_t)clampi(SONG.swing + d * (a > 1 ? 3 * a : 1), 0, 100), put_int(b, SONG.swing); break;
    case 12: SONG.reverb = (uint8_t)clampi(SONG.reverb + d * (a > 1 ? 4 * a : 1), 0, 127), put_int(b, SONG.reverb); break;
    case 13: SONG.chorus = (uint8_t)clampi(SONG.chorus + d * (a > 1 ? 4 * a : 1), 0, 127), put_int(b, SONG.chorus); break;
    case 14: SONG.spin = (uint8_t)clampi(SONG.spin + d, 0, 4), put(b, SONG.spin ? (const char *[]){"", "SLOW", "MEDIUM", "FAST", "FASTEST"}[SONG.spin] : "OFF"); break;
    default: SONG.theme = (uint8_t)clampi(SONG.theme + d, 0, 1), put(b, SONG.theme ? "AMBER" : "WHITE"); break;
  }
  say(KNOB_NAME[page][k], b, now);
}

/* ---------------------------------------------------------------- the light grid */

#define GX 16
#define GY 20
#define PITCH 13
static uint8_t lvl[N * N], shown[N * N];
static uint8_t disc_w[12];
static uint16_t pal[2][8];

static void draw_cell(int c, int v) {
  int x = GX + (c % N) * PITCH, y = GY + (15 - c / N) * PITCH;
  uint16_t col = pal[SONG.theme & 1][v & 7];
  for (int j = 0; j < 12; j++) {
    int w = disc_w[j];
    rect(x, y + j, 12, 1, C_BG);
    rect(x + 6 - w, y + j, 2 * w, 1, col);
  }
}

static void bump(int c, int v) {
  if (c >= 0 && c < N * N && lvl[c] < v) lvl[c] = (uint8_t)v;
}

static void compute(void) {
  const layer_t *L = cur();
  int md = layer_mode(layer);
  for (int c = 0; c < N * N; c++) lvl[c] = 0;
  if (show_all) /* every layer's notes, dim */
    for (int l = 0; l < LAYERS; l++) {
      const layer_t *K = &BLK[seq_cur_block].layer[l];
      int m = layer_mode(l);
      if (m == MD_SCORE || m == MD_PUSH)
        for (int x = 0; x < N; x++)
          for (int y = 0; y < N; y++)
            if (K->grid[x] >> y & 1u) bump(y * N + x, 2);
      if (m == MD_RANDOM || m == MD_DRAW)
        for (int i = 0; i < K->n; i++) bump(K->cell[i], 2);
    }
  if ((md == MD_SCORE || md == MD_DRAW) && seq_playing) /* the playing column */
    for (int y = 0; y < N; y++) bump(y * N + (seq_pos[layer] & 15), 1);
  switch (md) {
    case MD_SCORE:
    case MD_PUSH:
      for (int x = 0; x < N; x++)
        for (int y = 0; y < N; y++)
          if (L->grid[x] >> y & 1u) bump(y * N + x, md == MD_PUSH ? 6 : 4);
      break;
    case MD_RANDOM:
      for (int i = 0; i < L->n; i++) bump(L->cell[i], 4);
      if (seq_playing && L->n > 1) bump(((seq_rnd_y + 8) / 16 & 15) * N + ((seq_rnd_x + 8) / 16 & 15), 6);
      break;
    case MD_DRAW:
      for (int i = 0; i < L->n; i++) bump(L->cell[i], 3);
      break;
    case MD_BOUNCE:
      for (int x = 0; x < N; x++) {
        int h = L->grid[x] & 15;
        if (!h) continue;
        bump(h * N + x, 3);
        if (seq_playing && seq_ball[x] < 16) bump(seq_ball[x] * N + x, 6);
      }
      break;
    default: break;
  }
  /* what just sounded, and the ripple round it (other layers: a dim flash) */
  for (int l = 0; l < LAYERS; l++)
    for (int c = 0; c < N * N; c++) {
      int f = seq_flash[l][c];
      if (!f) continue;
      if (l != layer) {
        bump(c, 2);
        continue;
      }
      bump(c, 7);
      int r = 6 - f, x0 = c % N, y0 = c / N;
      if (r > 0 && r < 4)
        for (int d = -r; d <= r; d++) {
          int pts[4][2] = {{x0 + d, y0 - r}, {x0 + d, y0 + r}, {x0 - r, y0 + d}, {x0 + r, y0 + d}};
          for (int p = 0; p < 4; p++)
            if (pts[p][0] >= 0 && pts[p][0] < N && pts[p][1] >= 0 && pts[p][1] < N) bump(pts[p][1] * N + pts[p][0], 4 - r);
        }
    }
  /* PUSH: the held note (and the ones that stayed) glow wider as they grow */
  if (md == MD_PUSH && seq_push_lvl) {
    int rad = seq_push_lvl / 32, held = seq_push_cell ? seq_push_cell - 1 : -1;
    for (int c = 0; c < N * N; c++) {
      if (c != held && !(L->grid[c % N] >> (c / N) & 1u)) continue;
      bump(c, 7);
      for (int dx = -rad; dx <= rad; dx++)
        for (int dy = -rad; dy <= rad; dy++) {
          int x = c % N + dx, y = c / N + dy, d = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
          if (d && x >= 0 && x < N && y >= 0 && y < N) bump(y * N + x, 5 - d);
        }
    }
  }
}

static void draw_marks(void) { /* the line the keys play: a mark under its column, or beside its row */
  rect(0, GY, GX - 2, 16 * PITCH, C_BG), rect(GX + 16 * PITCH, GY, SCREEN_W - GX - 16 * PITCH, 16 * PITCH, C_BG);
  rect(0, GY + 16 * PITCH, SCREEN_W, SCREEN_H - GY - 16 * PITCH, C_BG);
  rect(0, 16, SCREEN_W, GY - 16, C_BG);
  if (col_line()) {
    int x = GX + cx * PITCH;
    rect(x, GY + 16 * PITCH + 2, 12, 4, C_MARK), rect(x, GY - 4, 12, 3, C_MARK);
  } else {
    int y = GY + (15 - cy) * PITCH;
    rect(4, y, 6, 12, C_MARK), rect(GX + 16 * PITCH + 4, y, 6, 12, C_MARK);
  }
}

static void draw_bar(uint32_t now) {
  rect(0, 0, SCREEN_W, 16, C_BAR);
  char b[40], *d;
  if (now < msg_until) {
    text(4, 0, msg, C_TEXT, FONT_SMALL);
    return;
  }
  d = put(put_2(put(b, "L"), layer + 1), " ");
  put(d, MODE_NAME[layer_mode(layer)]);
  text(4, 0, b, (SONG.mute >> layer & 1u) ? C_DIM : C_TEXT, FONT_SMALL);
  put_2(put(b, "B"), seq_cur_block + 1);
  text(112, 0, b, C_TEXT, FONT_SMALL);
  put_int(b, SONG.tempo);
  text(SCREEN_W - 4 - text_width(b, FONT_SMALL), 0, b, C_DIM, FONT_SMALL);
  if (seq_rec) rect(160, 4, 8, 8, C_REC);
  if (seq_playing) /* a play triangle */
    for (int j = 0; j < 11; j++) rect(176, 2 + j, j < 6 ? j + 1 : 11 - j, 1, C_TEXT);
}

/* ---------------------------------------------------------------- setup and loop */

static const int8_t WHITE_OF[KEY_COUNT] = {0, -1, 1, -1, 2, -1, 3, 4, -1, 5, -1, 6, 7, -1, 8, -1, 9, -1, 10, 11, -1, 12, -1, 13, 14, -1, 15};

void setup(void) {
  if (!song_load()) demo();
  snd_init();
  for (int j = 0; j < 12; j++) { /* a round LED, 12 px */
    int dy = 2 * j - 11, w = 0;
    while ((2 * w + 1) * (2 * w + 1) + dy * dy <= 12 * 12 + 4 && w < 6) w++;
    disc_w[j] = (uint8_t)w;
  }
  for (int v = 0; v < 8; v++) {
    static const uint8_t W[8] = {26, 60, 95, 130, 170, 205, 235, 255};
    pal[0][v] = RGB(W[v], W[v], W[v] < 250 ? W[v] + 4 : 255);
    pal[1][v] = RGB(W[v], W[v] * 3 / 5, W[v] / 6);
  }
  led_backlight(1);
  clear(C_BG);
  for (int c = 0; c < N * N; c++) shown[c] = 255;
}

static uint32_t sh_bar = ~0u, sh_mark = ~0u, last_draw, last_fade, led_k = ~0u, led_b = ~0u;

void loop(void) {
  uint32_t now = millis();
  int32_t e;
  seq_view_layer = layer;

  /* ---- buttons */
  if (button_pressed(BTN_PLAY)) seq_play(!seq_playing);
  if (button_pressed(BTN_REC)) seq_rec ^= 1;
  if (button_pressed(BTN_GLO)) SONG.mute ^= (uint16_t)(1u << layer), say(SONG.mute >> layer & 1u ? "MUTE" : "UNMUTE", 0, now);
  if (button_pressed(BTN_SEQ)) show_all ^= 1, say(show_all ? "ALL LAYERS" : "ONE LAYER", 0, now);
  if (button_pressed(BTN_ARP)) SONG.theme ^= 1;
  static const uint8_t PAGE_BTN[4] = {BTN_HOME, BTN_ENV, BTN_LFO, BTN_FX};
  for (int i = 0; i < 4; i++)
    if (button_pressed(PAGE_BTN[i])) {
      page = (uint8_t)i;
      char b[40], *d = put(put(b, PAGE_NAME[i]), ":");
      for (int k = 0; k < 4; k++) d = put(put(d, " "), KNOB_NAME[i][k]);
      say(b, 0, now);
    }
  if (button_pressed(BTN_SEL)) sel_down = now, sel_done = 0;
  if (button_held(BTN_SEL) && !sel_done && now - sel_down > 1000) {
    for (int l = 0; l < LAYERS; l++) seq_clear(l);
    sel_done = 1, say("CLEAR BLOCK", 0, now);
  }
  if (button_released(BTN_SEL) && !sel_done) seq_clear(layer), say("CLEAR LAYER", 0, now);
  if (button_pressed(BTN_EDIT)) {
    int to = (seq_cur_block + 1) % BLOCKS;
    char b[16];
    seq_copy(to), put_2(put(b, "COPY TO B"), to + 1), say(b, 0, now);
  }
  if (button_pressed(BTN_SAVE)) {
    if (song_save()) saved_t = now ? now : 1, say("SAVED", 0, now);
    else say("TOO MUCH TO SAVE", 0, now);
  }
  int mv = (button_pressed(BTN_OCT_UP) ? 1 : 0) - (button_pressed(BTN_OCT_DOWN) ? 1 : 0) + encoder(ENC_SELECT);
  if (mv) {
    if (col_line()) cx = (uint8_t)clampi(cx + mv, 0, 15);
    else cy = (uint8_t)clampi(cy + mv, 0, 15);
  }

  /* ---- encoders */
  if ((e = encoder(ENC_PRESETS))) {
    layer = (uint8_t)((layer + LAYERS * 8 + e) % LAYERS);
    char b[24];
    put(put(put_2(put(b, "LAYER "), layer + 1), " "), MODE_NAME[layer_mode(layer)]);
    say(b, sound_name(cur()->sound), now);
  }
  if ((e = encoder(ENC_ALGORITHM))) {
    int b = clampi(seq_cur_block + e, 0, BLOCKS - 1);
    char s[12];
    seq_block(b), put_2(put(s, "BLOCK "), b + 1), say(s, 0, now);
  }
  for (int k = 0; k < 4; k++)
    if ((e = encoder(ENC_KNOB1 + k))) knob(k, e, now);

  /* ---- the keys: the 16 cells of the marked line */
  uint32_t held = 0;
  for (int k = 0; k < KEY_COUNT; k++) {
    int w = WHITE_OF[k];
    if (w < 0) continue;
    if (key_held(k)) held |= 1u << k;
    if (key_pressed(k)) {
      int x, y;
      if (col_line()) x = (seq_rec && seq_playing && layer_mode(layer) == MD_SCORE) ? -1 : cx, y = w;
      else x = w, y = cy;
      seq_key(layer, x, y, 1);
      held_cell[k] = (uint8_t)(y * N + (x < 0 ? 0 : x)); /* where its release goes */
    }
    if (key_released(k)) seq_key(layer, held_cell[k] % N, held_cell[k] / N, 0);
  }

  /* ---- MIDI in: channel n plays layer n */
  uint8_t st, d1, d2;
  while (midi_read(&st, &d1, &d2)) {
    int t = st & 0xF0, ch = st & 15;
    if (t == 0x90 && d2) seq_midi(ch, d1, 1);
    else if (t == 0x80 || (t == 0x90 && !d2)) seq_midi(ch, d1, 0);
  }

  /* ---- LEDs: the white keys light the line's cells that hold a note */
  const layer_t *L = cur();
  uint32_t lk = held, lb = 0;
  for (int k = 0; k < KEY_COUNT; k++) {
    int w = WHITE_OF[k], on = 0;
    if (w < 0) continue;
    switch (layer_mode(layer)) {
      case MD_SCORE:
      case MD_PUSH: on = L->grid[cx] >> w & 1u; break;
      case MD_RANDOM:
      case MD_DRAW:
        for (int i = 0; i < L->n; i++) on |= L->cell[i] == w * N + cx;
        break;
      case MD_BOUNCE: on = (L->grid[w] & 15) == cy && cy; break;
      default: break;
    }
    if (on) lk |= 1u << k;
  }
  if (seq_playing) lb |= 1u << BTN_PLAY;
  if (seq_rec) lb |= 1u << BTN_REC;
  if (SONG.mute >> layer & 1u) lb |= 1u << BTN_GLO;
  if (show_all) lb |= 1u << BTN_SEQ;
  lb |= 1u << PAGE_BTN[page];
  if (lk != led_k) {
    for (int i = 0; i < KEY_COUNT; i++)
      if ((lk ^ led_k) >> i & 1u) led_key(i, lk >> i & 1u);
    led_k = lk;
  }
  if (lb != led_b) {
    for (int i = 0; i < BTN_COUNT; i++)
      if ((lb ^ led_b) >> i & 1u) led_button(i, lb >> i & 1u);
    led_b = lb;
  }

  /* ---- the flashes fade */
  if (now - last_fade >= 40) {
    last_fade = now;
    for (int l = 0; l < LAYERS; l++)
      for (int c = 0; c < N * N; c++)
        if (seq_flash[l][c]) seq_flash[l][c]--;
  }

  /* ---- the screen: the bar, the marks, and the cells that changed (at most every 30 ms) */
  uint32_t h = (uint32_t)layer | (uint32_t)seq_cur_block << 4 | (uint32_t)SONG.tempo << 8 | (uint32_t)seq_rec << 16 |
               (uint32_t)seq_playing << 17 | (uint32_t)(SONG.mute >> layer & 1u) << 18 | (uint32_t)(now < msg_until) << 19;
  for (int i = 0; now < msg_until && msg[i]; i++) h = h * 31u + (uint8_t)msg[i];
  if (h != sh_bar) sh_bar = h, draw_bar(now);
  h = (uint32_t)col_line() << 8 | (col_line() ? cx : cy);
  if (h != sh_mark) sh_mark = h, draw_marks();
  static uint8_t theme_shown = 255;
  if (theme_shown != SONG.theme) {
    theme_shown = SONG.theme;
    for (int c = 0; c < N * N; c++) shown[c] = 255;
  }
  if (now - last_draw >= 30) {
    last_draw = now;
    compute();
    for (int c = 0; c < N * N; c++)
      if (lvl[c] != shown[c]) shown[c] = lvl[c], draw_cell(c, lvl[c]);
  }
}

/* ---------------------------------------------------------------- the sound */

void audio(int32_t *out, uint32_t frames) { seq_render(out, frames); }
