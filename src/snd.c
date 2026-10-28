/* SPDX-License-Identifier: GPL-3.0-only */
/* GRIDLIGHT's sound engine (after GRVBOX's): 16 synth voices and 8 drum voices shared by the 16 layers, a reverb
 * and a chorus. Integers only: audio Q15, phases 32-bit, envelopes Q30.
 *
 * A synth voice: an oscillator (saw, square, pulse, two detuned saws, sine, 2-op FM, drawbar organ, noise) with
 * an optional sub, a resonant 12 dB state-variable low-pass with its own envelope, an ADSR and a pitch envelope.
 * The drum voices are synthesised models (swept sines, tone + noise, six-square metal, ...). */
#include "fm1.h"
#include "tn.h"
#include "tables.h"

#define Q15 32767
#define ONE30 1073741824
#define CTRL 16u
#define SYN_VOICES 16
#define DRUM_VOICES 8
#define DRUMS 16

static inline int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline int32_t sat15(int32_t x) {
  if (x >= 49152) return Q15;
  if (x <= -49152) return -Q15;
  int32_t x2 = (int32_t)(((int64_t)x * x) >> 15);
  int32_t x3 = (int32_t)(((int64_t)x2 * x) >> 15);
  return x - ((x3 * 4855) >> 15);
}
static uint32_t nz = 77777u;
static inline int32_t noise(void) {
  nz ^= nz << 13, nz ^= nz >> 17, nz ^= nz << 5;
  return (int32_t)nz >> 16;
}
static inline uint32_t hz_inc(int32_t hz) { return (uint32_t)clampi(hz, 0, 20000) * 97391u; }
static uint32_t coef(int32_t ms) { /* per sample, Q16 */
  if (ms < 1) ms = 1;
  return 65536u - (uint32_t)(655360 / (ms * 441));
}
static uint32_t coef_block(int32_t ms) { /* per CTRL frames, Q16 */
  int32_t n = ms * 441;
  if (n <= (int32_t)CTRL * 10) return 0;
  return 65536u - (uint32_t)(10485760 / n);
}
static int32_t attack_step(int32_t ms) {
  int32_t blocks = (ms * 441) / ((int32_t)CTRL * 10);
  return blocks < 1 ? ONE30 : ONE30 / blocks;
}
static inline int32_t decay(int32_t e, uint32_t c) { return (int32_t)(((int64_t)e * c) >> 16); }
static inline int32_t tms(int v) { return ENV_MS[clampi(v, 0, 127)]; }

/* ---------------------------------------------------------------- drums */

typedef struct {
  int32_t low, band;
} svf_t;
static inline int32_t svf_f(int32_t hz) { return (clampi(hz, 10, 5500) * 4669) >> 10; }
static inline void svf(svf_t *s, int32_t in, int32_t f, int32_t q, int32_t *bp) {
  s->low += (int32_t)(((int64_t)f * s->band) >> 15);
  int32_t h = in - s->low - (int32_t)(((int64_t)q * s->band) >> 15);
  s->band += (int32_t)(((int64_t)f * h) >> 15);
  s->band = clampi(s->band, -262144, 262144), s->low = clampi(s->low, -262144, 262144);
  *bp = s->band;
}
static inline int32_t onepole_a(int32_t hz) {
  int32_t w = (clampi(hz, 1, 20000) * 4669) >> 10;
  return (int32_t)(((uint32_t)w << 15) / (uint32_t)(32768 + w));
}

enum { M_KICK, M_SNARE, M_METAL, M_TOM, M_CLAP, M_BELL, M_ZAP, M_NOISE };
typedef struct {
  const char *name, *sh;
  uint8_t model;
  uint16_t f1, f2, pdec, dec, noise, hp, lvl;
  uint8_t choke;
  int8_t pan;
} dsnd_t;

/* the 16 pads: BD SD CH OH CP LT MT HT RS CB CY RD BD2 SD2 SH FX */
static const dsnd_t KIT[KITS][DRUMS] = {
    { /* 808 */
     {"BD 808", "BD", M_KICK, 64, 48, 50, 700, 40, 0, 1000, 0, 0},
     {"SD 808", "SD", M_SNARE, 180, 330, 40, 160, 640, 1200, 880, 0, 4},
     {"CH 808", "CH", M_METAL, 410, 0, 0, 40, 200, 7000, 780, 1, 16},
     {"OH 808", "OH", M_METAL, 410, 0, 0, 320, 200, 7000, 700, 1, 16},
     {"CP 808", "CP", M_CLAP, 1100, 0, 0, 220, 1024, 0, 880, 0, -8},
     {"LT 808", "LT", M_TOM, 100, 80, 120, 360, 0, 0, 860, 0, -20},
     {"MT 808", "MT", M_TOM, 140, 115, 100, 300, 0, 0, 840, 0, 0},
     {"HT 808", "HT", M_TOM, 190, 160, 90, 260, 0, 0, 820, 0, 20},
     {"RS 808", "RS", M_BELL, 1700, 840, 0, 20, 200, 0, 700, 0, 6},
     {"CB 808", "CB", M_BELL, 540, 800, 0, 200, 0, 0, 560, 0, -12},
     {"CY 808", "CY", M_METAL, 380, 0, 0, 1300, 600, 4500, 520, 0, 24},
     {"CL 808", "CL", M_BELL, 2500, 0, 0, 30, 0, 0, 640, 0, 12},
     {"BD SHORT", "B2", M_KICK, 70, 50, 30, 220, 80, 0, 980, 0, 0},
     {"SD SNAP", "S2", M_SNARE, 240, 400, 25, 110, 820, 2200, 860, 0, 4},
     {"MARACAS", "MA", M_METAL, 900, 0, 0, 40, 1024, 9000, 560, 0, -16},
     {"ZAP", "FX", M_ZAP, 3000, 80, 50, 180, 0, 0, 640, 0, 0},
    },
    { /* 909 */
     {"BD 909", "BD", M_KICK, 165, 47, 55, 420, 140, 0, 1020, 0, 0},
     {"SD 909", "SD", M_SNARE, 210, 360, 40, 150, 820, 1500, 880, 0, 4},
     {"CH 909", "CH", M_METAL, 340, 0, 0, 38, 520, 7500, 800, 1, 14},
     {"OH 909", "OH", M_METAL, 340, 0, 0, 260, 520, 7500, 720, 1, 14},
     {"CP 909", "CP", M_CLAP, 1150, 0, 0, 190, 1024, 0, 900, 0, -8},
     {"LT 909", "LT", M_TOM, 120, 75, 90, 240, 60, 0, 860, 0, -20},
     {"MT 909", "MT", M_TOM, 165, 105, 85, 210, 60, 0, 840, 0, 0},
     {"HT 909", "HT", M_TOM, 220, 145, 80, 190, 60, 0, 820, 0, 20},
     {"RS 909", "RS", M_BELL, 1900, 520, 0, 18, 220, 0, 720, 0, 6},
     {"TAMB", "TB", M_METAL, 1100, 0, 0, 120, 700, 8000, 560, 0, -14},
     {"CRASH 909", "CR", M_METAL, 360, 0, 0, 1100, 550, 4200, 560, 0, 24},
     {"RIDE 909", "RD", M_METAL, 470, 0, 0, 650, 250, 5000, 520, 0, 20},
     {"BD HARD", "B2", M_KICK, 190, 52, 40, 300, 300, 0, 1020, 0, 0},
     {"SD ROUGH", "S2", M_SNARE, 230, 380, 30, 180, 900, 1800, 900, 0, 4},
     {"SHAKER", "SH", M_METAL, 1000, 0, 0, 60, 1024, 8500, 540, 0, -16},
     {"NOISE RISE", "FX", M_NOISE, 300, 4500, 400, 450, 1024, 0, 560, 0, 0},
    },
    { /* 606 */
     {"BD 606", "BD", M_KICK, 120, 55, 35, 260, 80, 0, 980, 0, 0},
     {"SD 606", "SD", M_SNARE, 240, 420, 30, 120, 760, 2000, 840, 0, 4},
     {"CH 606", "CH", M_METAL, 480, 0, 0, 30, 400, 8000, 780, 1, 14},
     {"OH 606", "OH", M_METAL, 480, 0, 0, 220, 400, 8000, 700, 1, 14},
     {"CP 606", "CP", M_CLAP, 1300, 0, 0, 150, 1024, 0, 820, 0, -8},
     {"LT 606", "LT", M_TOM, 140, 110, 70, 200, 0, 0, 840, 0, -20},
     {"MT 606", "MT", M_TOM, 180, 145, 60, 180, 0, 0, 820, 0, 0},
     {"HT 606", "HT", M_TOM, 230, 190, 50, 160, 0, 0, 800, 0, 20},
     {"RS 606", "RS", M_BELL, 2000, 900, 0, 16, 200, 0, 700, 0, 6},
     {"CB 606", "CB", M_BELL, 600, 900, 0, 150, 0, 0, 520, 0, -12},
     {"CY 606", "CY", M_METAL, 420, 0, 0, 900, 600, 5000, 520, 0, 24},
     {"CL 606", "CL", M_BELL, 2800, 0, 0, 25, 0, 0, 620, 0, 12},
     {"BD DEEP", "B2", M_KICK, 90, 45, 60, 380, 60, 0, 1000, 0, 0},
     {"SD TIGHT", "S2", M_SNARE, 280, 460, 20, 80, 800, 2600, 820, 0, 4},
     {"HAT PEDAL", "HP", M_METAL, 480, 0, 0, 20, 400, 9000, 640, 1, 14},
     {"BLIP", "FX", M_BELL, 1320, 1980, 0, 60, 0, 0, 520, 0, 0},
    },
    { /* CR-78 */
     {"BD 78", "BD", M_KICK, 90, 62, 20, 200, 20, 0, 960, 0, 0},
     {"SD 78", "SD", M_SNARE, 260, 520, 30, 120, 500, 3000, 760, 0, 4},
     {"HH 78", "HH", M_METAL, 520, 0, 0, 45, 700, 8500, 760, 1, 14},
     {"HH OPEN 78", "OH", M_METAL, 520, 0, 0, 200, 700, 8500, 700, 1, 14},
     {"GUIRO", "GU", M_CLAP, 2200, 0, 0, 120, 1024, 0, 640, 0, -10},
     {"CONGA LO", "CL", M_TOM, 220, 200, 20, 150, 0, 0, 780, 0, -20},
     {"CONGA HI", "CH", M_TOM, 310, 290, 20, 130, 0, 0, 760, 0, 0},
     {"BONGO", "BG", M_TOM, 420, 400, 15, 100, 0, 0, 740, 0, 20},
     {"RIM 78", "RM", M_BELL, 1800, 700, 0, 16, 150, 0, 700, 0, 6},
     {"COWBELL 78", "CB", M_BELL, 800, 1180, 0, 130, 0, 0, 520, 0, -12},
     {"CYMBAL 78", "CY", M_METAL, 600, 0, 0, 800, 700, 6000, 520, 0, 24},
     {"CLAVES", "CV", M_BELL, 2600, 0, 0, 30, 0, 0, 640, 0, 12},
     {"BD 78 ACC", "B2", M_KICK, 95, 62, 25, 240, 40, 0, 1020, 0, 0},
     {"SD 78 ACC", "S2", M_SNARE, 260, 520, 30, 150, 520, 3000, 860, 0, 4},
     {"TAMBOURINE", "TB", M_METAL, 1200, 0, 0, 160, 800, 8000, 560, 0, -16},
     {"MARACAS 78", "MA", M_METAL, 1000, 0, 0, 35, 1024, 9500, 560, 0, 16},
    },
};
const char *const KIT_NAMES[KITS] = {"DRUMS 808", "DRUMS 909", "DRUMS 606", "PERCUSSION"};

typedef struct {
  const dsnd_t *s;
  uint32_t t, ph[6];
  int32_t env, penv, hp1, hp2, hpa, gain;
  uint32_t ac, pc;
  svf_t f1;
  int8_t pad, layer;
  int16_t pan, vol;
} dvoice_t;
static dvoice_t DV[DRUM_VOICES];
static uint32_t dage[DRUM_VOICES], dage_n;
static const uint16_t METAL_R[6] = {1024, 1518, 1844, 2607, 2693, 3990};

static void drum_hit(int layer, int kit, int d, int vel, int pan, int vol) {
  const dsnd_t *s = &KIT[clampi(kit, 0, KITS - 1)][clampi(d, 0, DRUMS - 1)];
  int best = -1;
  for (int i = 0; i < DRUM_VOICES; i++) {
    if (DV[i].s && DV[i].pad == d && DV[i].layer == layer) best = i;
    if (s->choke && DV[i].s && DV[i].s->choke == s->choke && DV[i].pad != d) DV[i].ac = coef(4);
  }
  if (best < 0)
    for (int i = 0; i < DRUM_VOICES; i++)
      if (!DV[i].s) {
        best = i;
        break;
      }
  if (best < 0) {
    best = 0;
    for (int i = 1; i < DRUM_VOICES; i++)
      if (dage[i] < dage[best]) best = i;
  }
  dvoice_t *v = &DV[best];
  v->s = s, v->pad = (int8_t)d, v->layer = (int8_t)layer, v->t = 0, v->env = ONE30, v->penv = ONE30;
  v->ac = coef(s->dec), v->pc = coef(s->pdec ? s->pdec : 1);
  for (int i = 0; i < 6; i++) v->ph[i] = (uint32_t)i * 715827883u;
  v->hp1 = v->hp2 = 0, v->hpa = onepole_a(s->hp ? s->hp : 1), v->f1 = (svf_t){0, 0};
  v->gain = (s->lvl * clampi(vel, 1, 127)) / 127;
  v->pan = (int16_t)clampi(pan + s->pan, -64, 63), v->vol = (int16_t)clampi(vol, 0, 127);
  dage[best] = ++dage_n;
}

static int32_t drum_sample(dvoice_t *v) {
  const dsnd_t *s = v->s;
  int32_t e15 = v->env >> 15, p15 = v->penv >> 15, out = 0;
  switch (s->model) {
    case M_KICK:
    case M_TOM:
    case M_ZAP: {
      v->ph[0] += hz_inc(s->f2 + (((s->f1 - s->f2) * p15) >> 15));
      out = sine(v->ph[0]);
      if (s->model == M_KICK && v->t < 130) out += (noise() * s->noise) >> 10;
      else if (s->model == M_TOM) out += (noise() * s->noise) >> 13;
      if (s->model == M_KICK) out = sat15(out + (out >> 1));
      break;
    }
    case M_SNARE: {
      v->ph[0] += hz_inc(s->f1), v->ph[1] += hz_inc(s->f2);
      int32_t tone = ((sine(v->ph[0]) + (sine(v->ph[1]) >> 1)) * p15) >> 15, n = noise();
      v->hp1 += ((n - v->hp1) * v->hpa) >> 15;
      n -= v->hp1;
      out = ((tone * (1024 - s->noise)) >> 10) + ((n * s->noise) >> 10);
      break;
    }
    case M_METAL: {
      int32_t sum = 0;
      for (int i = 0; i < 6; i++) {
        v->ph[i] += hz_inc((s->f1 * METAL_R[i]) >> 10);
        sum += v->ph[i] < 2147483648u ? 5461 : -5461;
      }
      sum = ((sum * (1024 - s->noise)) >> 10) + ((noise() * s->noise) >> 10);
      v->hp1 += ((sum - v->hp1) * v->hpa) >> 15, sum -= v->hp1;
      v->hp2 += ((sum - v->hp2) * v->hpa) >> 15;
      out = sat15((sum - v->hp2) * 4);
      break;
    }
    case M_CLAP: {
      int32_t bp;
      svf(&v->f1, noise(), svf_f(s->f1), 9000, &bp);
      out = bp * 2;
      if (v->t < 3 * 441) e15 = clampi((441 - (int32_t)(v->t % 441)) * 74, 0, Q15), v->env = ONE30;
      break;
    }
    case M_BELL: {
      v->ph[0] += hz_inc(s->f1), v->ph[1] += hz_inc(s->f2 ? s->f2 : s->f1);
      int32_t sq = (v->ph[0] < 2147483648u ? 12000 : -12000) + (v->ph[1] < 2147483648u ? 12000 : -12000), bp;
      svf(&v->f1, sq + ((noise() * s->noise) >> 10), svf_f(2600), 16000, &bp);
      out = bp * 2;
      break;
    }
    case M_NOISE: {
      int32_t bp;
      svf(&v->f1, noise(), svf_f(s->f2 + (((s->f1 - s->f2) * p15) >> 15)), 6000, &bp);
      int32_t sw = clampi((int32_t)(v->t / 7), 0, Q15);
      if (sw < e15) e15 = sw;
      out = bp * 2;
      break;
    }
  }
  v->env = decay(v->env, v->ac), v->penv = decay(v->penv, v->pc), v->t++;
  if (v->env < 16384) v->s = 0;
  return (int32_t)(((int64_t)out * e15 >> 15) * v->gain >> 10);
}

/* ---------------------------------------------------------------- synth voices */

enum { ST_IDLE, ST_ATTACK, ST_DECAY, ST_SUSTAIN, ST_RELEASE };
typedef struct {
  uint8_t layer, note, gate, vel, ast, fst, cyc;
  int8_t pan;
  uint8_t vol;
  uint32_t age;
  const tone_t *t;
  int32_t pitch;
  uint32_t ph, ph2, phm, rcp;
  int32_t aenv, fenv, penv, fmenv, amp;
  int32_t ic1, ic2;
} sv_t;
static sv_t SV[SYN_VOICES];
static uint32_t sage;
static volatile uint8_t push_amt[LAYERS];

static inline int32_t blep(uint32_t t, uint32_t inc, uint32_t rcp) {
  if (t < inc) {
    int32_t x = (int32_t)(((uint64_t)(t >> 16) * rcp) >> 17);
    return 2 * x - ((x * x) >> 15) - 32768;
  }
  uint32_t d = 0u - t;
  if (d < inc) {
    int32_t x = 32768 - (int32_t)(((uint64_t)(d >> 16) * rcp) >> 17);
    return (x * x) >> 15;
  }
  return 0;
}

static uint32_t inc_q8(int32_t p) {
  p = clampi(p, 0, 127 * 256);
  int n = p >> 8, f = p & 255;
  uint32_t a = note_inc(n), b = note_inc(n + 1);
  return a + (uint32_t)(((uint64_t)(b - a) * (uint32_t)f) >> 8);
}

const char *sound_name(int s) { return s < TONES ? TONE[clampi(s, 0, TONES - 1)].name : KIT_NAMES[clampi(s - TONES, 0, KITS - 1)]; }

void snd_note_on(int layer, int sound, int note, int vel, int pan, int vol) {
  if (sound >= TONES) {
    drum_hit(layer, sound - TONES, note, vel, pan, vol);
    return;
  }
  const tone_t *t = &TONE[clampi(sound, 0, TONES - 1)];
  note = clampi(note, 0, 120);
  int best = -1;
  for (int i = 0; i < SYN_VOICES; i++)
    if (SV[i].layer == layer && SV[i].ast != ST_IDLE && (t->mono || SV[i].note == note)) best = i;
  if (best < 0)
    for (int i = 0; i < SYN_VOICES; i++)
      if (SV[i].ast == ST_IDLE) {
        best = i;
        break;
      }
  if (best < 0) { /* steal: the oldest released voice, else the oldest */
    uint32_t old = 0xFFFFFFFFu;
    for (int i = 0; i < SYN_VOICES; i++)
      if (!SV[i].gate && SV[i].age < old) old = SV[i].age, best = i;
    if (best < 0)
      for (int i = 0; i < SYN_VOICES; i++)
        if (SV[i].age < old) old = SV[i].age, best = i;
  }
  sv_t *v = &SV[best];
  if (v->layer != layer || v->ast == ST_IDLE) v->ic1 = v->ic2 = 0, v->amp = 0;
  v->pitch = note * 256;
  v->layer = (uint8_t)layer, v->note = (uint8_t)note, v->gate = 1, v->vel = (uint8_t)clampi(vel, 1, 127), v->t = t;
  v->pan = (int8_t)clampi(pan, -64, 63), v->vol = (uint8_t)clampi(vol, 0, 127);
  v->age = ++sage;
  v->ast = ST_ATTACK, v->fst = ST_ATTACK, v->fenv = 0, v->penv = ONE30, v->fmenv = ONE30;
}

void snd_note_off(int layer, int note) {
  for (int i = 0; i < SYN_VOICES; i++)
    if (SV[i].layer == layer && SV[i].note == note && SV[i].gate) SV[i].gate = 0, SV[i].ast = ST_RELEASE;
}
void snd_layer_off(int layer) {
  for (int i = 0; i < SYN_VOICES; i++)
    if (SV[i].layer == layer && SV[i].gate) SV[i].gate = 0, SV[i].ast = ST_RELEASE;
}
void snd_push(int layer, int amount) { push_amt[clampi(layer, 0, LAYERS - 1)] = (uint8_t)clampi(amount, 0, 127); }

/* ---------------------------------------------------------------- effects */

#define RV_N 4
static const int16_t RV_LEN[RV_N] = {1427, 1693, 1951, 2213};
static int16_t rv0[1427] POOL, rv1[1693] POOL, rv2[1951] POOL, rv3[2213] POOL;
static int16_t *const RVB[RV_N] = {rv0, rv1, rv2, rv3};
static int16_t ap0[142] POOL, ap1[107] POOL;
static int32_t rv_w[RV_N], rv_lp[RV_N], ap_w[2];

#define CH_N 1024
static int16_t ch_buf[CH_N] POOL;
static int32_t ch_w;
static uint32_t ch_ph;
static int32_t half, acc_rev, rv_ol, rv_or, rv_pl, rv_pr;

static inline int16_t s16(int32_t v) { return (int16_t)clampi(v, -32767, 32767); }

static void reverb_half(int32_t in, int32_t *ol, int32_t *or_) {
  /* two input allpasses, then four lines mixed (Hadamard), damped, fed back */
  int32_t x = in;
  int16_t *ab[2] = {ap0, ap1};
  static const int16_t AL[2] = {142, 107};
  for (int k = 0; k < 2; k++) {
    int32_t v = ab[k][ap_w[k]], y = v - ((20000 * x) >> 15);
    ab[k][ap_w[k]] = s16(x + ((20000 * y) >> 15));
    if (++ap_w[k] >= AL[k]) ap_w[k] = 0;
    x = y;
  }
  int32_t r[RV_N];
  for (int l = 0; l < RV_N; l++) {
    int32_t v = RVB[l][rv_w[l]];
    rv_lp[l] += ((v - rv_lp[l]) * 14000) >> 15;
    r[l] = rv_lp[l];
  }
  int32_t h[RV_N] = {(r[0] + r[1] + r[2] + r[3]) >> 1, (r[0] - r[1] + r[2] - r[3]) >> 1, (r[0] + r[1] - r[2] - r[3]) >> 1,
                     (r[0] - r[1] - r[2] + r[3]) >> 1};
  for (int l = 0; l < RV_N; l++) {
    RVB[l][rv_w[l]] = s16(sat15(((h[l] * 28500) >> 15) + x));
    if (++rv_w[l] >= RV_LEN[l]) rv_w[l] = 0;
  }
  *ol = r[0] + r[2], *or_ = r[1] + r[3];
}

void snd_init(void) {
  for (int i = 0; i < SYN_VOICES; i++) SV[i] = (sv_t){0};
  for (int i = 0; i < DRUM_VOICES; i++) DV[i].s = 0;
  for (int l = 0; l < RV_N; l++)
    for (int i = 0; i < RV_LEN[l]; i++) RVB[l][i] = 0;
  for (int i = 0; i < 142; i++) ap0[i] = 0;
  for (int i = 0; i < 107; i++) ap1[i] = 0;
  for (int i = 0; i < CH_N; i++) ch_buf[i] = 0;
}

static void adsr(int32_t *lvl, uint8_t *st, int a, int d, int s, int r) {
  int32_t sus = (int32_t)(((int64_t)clampi(s, 0, 127) * ONE30) / 127);
  switch (*st) {
    case ST_ATTACK:
      *lvl += attack_step(tms(a));
      if (*lvl >= ONE30) *lvl = ONE30, *st = ST_DECAY;
      break;
    case ST_DECAY:
      *lvl = sus + (int32_t)(((int64_t)(*lvl - sus) * coef_block(tms(d))) >> 16);
      if (*lvl - sus < 65536 && *lvl - sus > -65536) *lvl = sus, *st = ST_SUSTAIN;
      break;
    case ST_SUSTAIN: *lvl = sus; break;
    case ST_RELEASE:
      *lvl = (int32_t)(((int64_t)*lvl * coef_block(tms(r))) >> 16);
      if (*lvl < 32768) *lvl = 0, *st = ST_IDLE;
      break;
    default: break;
  }
}

void snd_render(int32_t *out, uint32_t frames) {
  static int32_t bl[512], bdry[256], bcho[256];
  if (frames > 256) frames = 256;
  for (uint32_t i = 0; i < frames; i++) bdry[i] = bcho[i] = 0, bl[2 * i] = bl[2 * i + 1] = 0;

  /* ---- synth voices, a block at a time */
  for (uint32_t b0 = 0; b0 < frames; b0 += CTRL) {
    uint32_t n = frames - b0 < CTRL ? frames - b0 : CTRL;
    for (int vi = 0; vi < SYN_VOICES; vi++) {
      sv_t *v = &SV[vi];
      if (v->ast == ST_IDLE) continue;
      const tone_t *t = v->t;
      int push = push_amt[v->layer & 15];
      adsr(&v->aenv, &v->ast, t->a, t->d, t->s, t->r);
      if (v->fst == ST_ATTACK) {
        v->fenv += attack_step(tms(t->fa));
        if (v->fenv >= ONE30) v->fenv = ONE30, v->fst = ST_DECAY;
      } else {
        v->fenv = decay(v->fenv, coef_block(tms(t->fd)));
      }
      v->penv = decay(v->penv, coef_block(tms(t->pdec)));
      v->fmenv = decay(v->fmenv, coef_block(tms(t->fmdec)));
      int32_t p = v->pitch + (int32_t)(((int64_t)t->pitch_env * 256 * (v->penv >> 15)) >> 15);
      uint32_t inc = inc_q8(p);
      v->rcp = 0xFFFFFFFFu / ((inc >> 16) | 1u);
      uint32_t inc2 = inc_q8(p + (t->detune * 256) / 100);
      uint32_t incm = (uint32_t)(((uint64_t)inc * t->fmratio) >> 1);
      int32_t fmi = (int32_t)(((int64_t)(t->fmindex + push / 4) * 1304 * ((v->fmenv >> 15) + push * 64)) >> 15);
      int32_t ci = (t->cut + push / 2) * 8 + ((t->fenv * (v->fenv >> 15)) >> 12) + (v->note - 60) * 3;
      uint32_t g = SVF_G[clampi(ci, 0, 1023)];
      int32_t res = clampi(t->res + push / 4, 0, 124);
      uint32_t k = 131072u - (uint32_t)res * 1024u;
      uint32_t den = 65536u + (uint32_t)(((uint64_t)g * (g + k)) >> 16);
      int32_t a1 = (int32_t)(0xFFFFFFFFu / den), a2 = (int32_t)(((int64_t)g * a1) >> 16), a3 = (int32_t)(((int64_t)g * a2) >> 16);
      int32_t amp = (int32_t)(((int64_t)(v->aenv >> 15) * v->vel * v->vol * t->level) >> 21);
      int32_t a0 = v->amp, da = (amp - a0) / (int32_t)n;
      v->amp = amp;
      int32_t gl = 64 - v->pan, gr = 64 + v->pan; /* /128 */
      int32_t drive = 1024 + t->drive * 24;
      uint32_t pw = (uint32_t)t->pw << 25;
      for (uint32_t i = 0; i < n; i++) {
        uint32_t ph = v->ph;
        int32_t s, b;
        switch (t->osc) {
          case OSC_SAW: s = (int32_t)(ph >> 16) - 32768 - blep(ph, inc, v->rcp); break;
          case OSC_SQR:
            b = blep(ph, inc, v->rcp);
            s = (ph < 2147483648u ? 26000 : -26000) + ((b - blep(ph - 2147483648u, inc, v->rcp)) * 26000 >> 15);
            break;
          case OSC_PULSE:
            b = blep(ph, inc, v->rcp);
            s = (ph < pw ? 26000 : -26000) + ((b - blep(ph - pw, inc, v->rcp)) * 26000 >> 15);
            break;
          case OSC_2SAW: {
            int32_t a = (int32_t)(ph >> 16) - 32768 - blep(ph, inc, v->rcp);
            int32_t c = (int32_t)(v->ph2 >> 16) - 32768;
            v->ph2 += inc2;
            s = (a + c) >> 1;
            break;
          }
          case OSC_FM:
            v->phm += incm;
            s = sine(ph + (uint32_t)((int64_t)sine(v->phm) * fmi));
            break;
          case OSC_ORGAN: s = (sine(ph) >> 1) + (sine(ph << 1) * 11 >> 5) + (sine(ph * 3) >> 2) + (sine(ph << 2) >> 3); break;
          case OSC_NOISE: s = noise(); break;
          default: s = sine(ph); break;
        }
        v->ph = ph + inc;
        if (v->ph < ph) v->cyc++;
        if (t->sub) s += ((v->cyc & 1) ? 20000 : -20000) * t->sub >> 7;
        int32_t v0 = s * 64, v3 = v0 - v->ic2;
        int32_t v1 = (int32_t)(((int64_t)a1 * v->ic1 + (int64_t)a2 * v3) >> 16);
        int32_t v2 = v->ic2 + (int32_t)(((int64_t)a2 * v->ic1 + (int64_t)a3 * v3) >> 16);
        v->ic1 = clampi(2 * v1 - v->ic1, -67108864, 67108864), v->ic2 = clampi(2 * v2 - v->ic2, -67108864, 67108864);
        s = sat15(((v2 >> 6) * drive) >> 10);
        s = (s * (a0 + da * (int32_t)i)) >> 15;
        uint32_t o = b0 + i;
        bl[2 * o] += (s * gl) >> 7, bl[2 * o + 1] += (s * gr) >> 7;
        bdry[o] += s;
      }
      if (v->ast == ST_IDLE) v->amp = 0;
    }
  }

  /* ---- drums */
  for (int k = 0; k < DRUM_VOICES; k++) {
    dvoice_t *v = &DV[k];
    if (!v->s) continue;
    int32_t gl = 64 - v->pan, gr = 64 + v->pan, vol = v->vol;
    for (uint32_t i = 0; i < frames && v->s; i++) {
      int32_t s = (drum_sample(v) * vol) >> 7;
      bl[2 * i] += (s * gl) >> 7, bl[2 * i + 1] += (s * gr) >> 7;
      bdry[i] += s >> 1;
    }
  }

  /* ---- reverb, chorus and the master */
  int32_t srev = clampi(SONG.reverb, 0, 127), scho = clampi(SONG.chorus, 0, 127);
  for (uint32_t i = 0; i < frames; i++) {
    ch_buf[ch_w] = s16((bdry[i] * scho) >> 7);
    ch_ph += 58000u; /* ~0.6 Hz */
    int32_t m = (sine(ch_ph) * 140) >> 15;
    int32_t ra = ch_w - (300 + m), rb = ch_w - (300 - m);
    if (ra < 0) ra += CH_N;
    if (rb < 0) rb += CH_N;
    int32_t cl = ch_buf[ra], cr = ch_buf[rb];
    if (++ch_w >= CH_N) ch_w = 0;
    acc_rev += (bdry[i] * srev) >> 7;
    if (half) {
      int32_t l, r;
      reverb_half(acc_rev >> 2, &l, &r);
      rv_pl = rv_ol, rv_pr = rv_or, rv_ol = l, rv_or = r;
      acc_rev = 0;
    }
    int32_t rl = half ? rv_ol : (rv_pl + rv_ol) >> 1, rr = half ? rv_or : (rv_pr + rv_or) >> 1;
    half ^= 1;
    int32_t L = bl[2 * i] + rl + cl, R = bl[2 * i + 1] + rr + cr;
    out[2 * i] += sat15((L * 5) >> 3);
    out[2 * i + 1] += sat15((R * 5) >> 3);
  }
}
