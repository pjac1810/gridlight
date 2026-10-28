/* SPDX-License-Identifier: GPL-3.0-only */
/* GRIDLIGHT: a 16 x 16 light-grid sequencer for the M-VAVE FM-1, after the Yamaha TENORI-ON. Shared by the panel
 * (main.c), the sequencer and its six modes (seq.c) and the sound (snd.c, tones.c). */
#ifndef TN_H
#define TN_H
#include <stdint.h>

#ifndef RING_PUBLISH
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")
#endif

#define N 16 /* the grid, the layers, the blocks */
#define LAYERS 16
#define BLOCKS 16
#define MAXPT 64 /* RANDOM and DRAW: notes a layer can hold */

/* the layers' modes, as on the original: 1-7 SCORE, 8-11 RANDOM, 12-13 DRAW, 14 BOUNCE, 15 PUSH, 16 SOLO */
enum { MD_SCORE, MD_RANDOM, MD_DRAW, MD_BOUNCE, MD_PUSH, MD_SOLO };
int layer_mode(int layer);
extern const char *const MODE_NAME[6];

/* ---- sounds (tones.c, snd.c) */
enum { OSC_SAW, OSC_SQR, OSC_PULSE, OSC_2SAW, OSC_SINE, OSC_FM, OSC_ORGAN, OSC_NOISE };
typedef struct {
  const char *name;
  uint8_t osc, mono;
  uint8_t detune;                  /* cents (2SAW) */
  uint8_t pw;                      /* PULSE: 64 = 50 % */
  uint8_t fmratio, fmindex, fmdec; /* FM: ratio x 2, peak index (x 1/16), its decay (time 0..127) */
  uint8_t sub;                     /* a square an octave down, 0..127 */
  uint8_t cut, res;                /* the filter, 0..127 */
  int8_t fenv;                     /* filter envelope amount */
  uint8_t fa, fd;                  /* its attack and decay (time 0..127) */
  uint8_t a, d, s, r;              /* loudness */
  int8_t pitch_env;                /* semitones at the start, falling to 0 */
  uint8_t pdec;
  uint8_t drive, porta, level;
} tone_t;
#define TONES 28
#define KITS 4
#define SOUNDS (TONES + KITS) /* a layer's sound: a tone, or a drum kit (its rows are the 16 drums) */
extern const tone_t TONE[TONES];
extern const char *const KIT_NAMES[KITS];
const char *sound_name(int s);

/* ---- a layer, in a block */
typedef struct {
  uint8_t sound, len;    /* len: gate 0..15 (20 ms .. 5 s) */
  int8_t oct;            /* -5..5 */
  uint8_t vol;           /* 0..127 */
  uint8_t lstart, lend;  /* SCORE: the loop, columns 0..15 */
  uint8_t speed;         /* 0 quarter, 1 eighth, 2 sixteenth, 3 thirty-second */
  int8_t pan;            /* -64..63 */
  uint16_t grid[N];      /* SCORE, PUSH (latched): bit r of grid[c] = the note at column c, row r. BOUNCE: grid[c] = height */
  uint8_t n;             /* RANDOM, DRAW: notes */
  uint8_t cell[MAXPT];   /* row * 16 + column, in the order played */
  uint16_t tick[MAXPT];  /* DRAW: when, in ticks from the loop's start */
} layer_t;

typedef struct {
  layer_t layer[LAYERS];
} block_t;

typedef struct {
  uint8_t tempo;     /* 40..240 */
  int8_t transpose;  /* -7..8 */
  uint8_t scale;     /* SCALES */
  uint8_t swing;     /* 0..100 */
  uint8_t reverb, chorus, spin, theme;
  uint16_t mute;     /* bit l: layer l muted */
} song_t;

extern block_t BLK[BLOCKS] /* in the pool */;
extern song_t SONG;
#define SCALES 9
extern const char *const SCALE_NAME[SCALES];
int row_note(const layer_t *L, int row); /* the MIDI note of a row (or a column, BOUNCE and SOLO) */
void layer_reset(layer_t *L, int layer);

/* ---- snd.c */
void snd_init(void);
void snd_note_on(int layer, int sound, int note, int vel, int pan, int vol);
void snd_note_off(int layer, int note);
void snd_layer_off(int layer);
void snd_push(int layer, int amount); /* PUSH: the held note brightens, 0..127 */
void snd_render(int32_t *out, uint32_t frames);

/* ---- seq.c: the clock and the six modes, in audio() */
void seq_render(int32_t *out, uint32_t frames);
void seq_play(int on);
void seq_block(int b);                  /* switch block (at the next step when playing) */
void seq_key(int layer, int x, int y, int on); /* a press at (column, row) of a layer */
void seq_clear(int layer);              /* the layer, in the current block */
void seq_midi(int layer, int note, int on); /* MIDI in: the layer's sound */
void seq_copy(int to);                  /* the current block into block 'to' */
extern volatile uint8_t seq_playing, seq_rec, seq_cur_block;
extern volatile uint8_t seq_pos[LAYERS];          /* SCORE / DRAW: the column playing */
extern volatile uint8_t seq_ball[N];              /* BOUNCE (layer 14): each column's ball row (255 none) */
extern volatile uint8_t seq_rnd_x, seq_rnd_y;     /* RANDOM: where the light is (current layer), x 16 */
extern volatile uint8_t seq_flash[LAYERS][N * N]; /* a cell just sounded: a countdown the screen reads */
extern volatile uint8_t seq_push_lvl;             /* PUSH: how far the held note has grown, 0..127 */
extern volatile uint16_t seq_push_cell;           /* PUSH: the note held, + 1 (row * 16 + column), 0 none */
extern volatile uint8_t seq_view_layer;           /* the layer on screen (RANDOM's light is for it) */
extern volatile uint16_t seq_dirty;               /* bumped when the song changes from the audio side */

#endif
