/* =============================================================================
 * robovox_vst.cpp - Robovox, a robot vocoder as a VST2 INSERT EFFECT for the MPC OS plugin host
 * (Force, MPC Live/One/X/Key). Put it on an audio track whose input is the mic: the voice is the
 * modulator, a built-in synth the carrier.
 *
 *   VOICE    the carrier: SAW, SQUARE, PULSE, SUPER (three detuned saws), BUZZ (narrow pulse),
 *            NOISE (whisper). PITCH: FIXED (ROOT + CHORD), LATCH (the last MIDI note becomes the
 *            root), MIDI (plays the held notes, silent without), FOLLOW (tracks the singer's pitch
 *            and snaps it to SCALE - the "auto-tuned robot"). OCTAVE, GLIDE, SPREAD.
 *   VOCODER  8-24 bands, SHIFT (carrier bands +-12 semitones: giant / tiny robot), Q (wide to
 *            ringing), RELEASE, SIBILANCE (the mic's highs straight through), UNVOICED (noise into
 *            the carrier when the mic is hissy, for consonants), FREEZE (holds the spectrum).
 *   ROBOT    CRUSH (sample rate + bits), RING (ring modulator, 20 Hz-1.8 kHz), WIDTH (bands spread
 *            left/right), GATE SYNC (tempo-synced trance gate from the host transport).
 *   IN/OUT   INPUT (L+R, LEFT, RIGHT), IN GAIN, NOISE GATE, MIX, OUTPUT (with a soft knee).
 *   PRESETS  32 slots, the first 14 factory sounds; LOAD/SAVE, and the slots are the plugin's
 *            programs, so they show in the MPC's preset list. Saved slots live in
 *            <plugin dir>/robovox_user/slot_NN.txt.
 *
 * MIDI: MPC OS does not send MIDI to insert effects. Robovox opens its own ALSA sequencer port
 * ("Robovox" / "MIDI In", libasound opened with dlopen at run time); a MIDI track with that port as
 * its output plays the carrier (LATCH, MIDI) - same way as Oneiroi FX. Program change 0-31 there
 * loads a slot. VST MIDI events, should a host send any, are taken too.
 *
 * CPU: one source, no engine. When the noise gate is closed and the band envelopes have died away
 * the plugin skips the vocoder entirely. Load goes to /tmp/robovox_vst.log every ~5 s.
 *
 * GPL-3.0-or-later (the VST shell follows mpc-vst-fsvr's fsvr_vst.cpp).
 * ========================================================================== */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include <dlfcn.h>
#include <poll.h>
#include <sys/stat.h>
#include <ctime>

#include "params.h"
#include "popup.h"    /* mpc-vst-plugins wrapper/popup.h, copied into build/ by build.sh */

/* ---- VST2 ABI (hand-written; no Steinberg SDK) ---------------------------- */
struct AEffect;
typedef intptr_t (*audioMasterCallback)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
struct AEffect {
    int32_t magic;
    intptr_t (*dispatcher)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
    void (*process)(AEffect *, float **, float **, int32_t);
    void (*setParameter)(AEffect *, int32_t, float);
    float (*getParameter)(AEffect *, int32_t);
    int32_t numPrograms, numParams, numInputs, numOutputs, flags;
    intptr_t resvd1, resvd2;
    int32_t initialDelay, realQualities, offQualities;
    float ioRatio;
    void *object, *user;
    int32_t uniqueID, version;
    void (*processReplacing)(AEffect *, float **, float **, int32_t);
    void (*processDoubleReplacing)(AEffect *, double **, double **, int32_t);
    char future[56];
};
typedef struct { int32_t type, byteSize, deltaFrames, flags; char data[16]; } VstEvent;
typedef struct {
    int32_t type, byteSize, deltaFrames, flags, noteLength, noteOffset;
    unsigned char midiData[4];
    char detune, noteOffVelocity, reserved1, reserved2;
} VstMidiEvent;
typedef struct { int32_t numEvents; intptr_t reserved; VstEvent *events[2]; } VstEvents;

enum {
    effOpen = 0, effClose = 1, effSetProgram = 2, effGetProgram = 3, effSetProgramName = 4,
    effGetProgramName = 5, effGetParamLabel = 6, effGetParamDisplay = 7, effGetParamName = 8,
    effSetSampleRate = 10, effSetBlockSize = 11, effMainsChanged = 12, effGetChunk = 23,
    effSetChunk = 24, effProcessEvents = 25, effCanBeAutomated = 26, effGetProgramNameIndexed = 29,
    effGetPlugCategory = 35, effGetEffectName = 45, effGetVendorString = 47, effGetProductString = 48,
    effGetVendorVersion = 49, effCanDo = 51, effGetTailSize = 52, effGetVstVersion = 58,
};
enum { audioMasterAutomate = 0, audioMasterGetTime = 7, audioMasterUpdateDisplay = 42 };
enum { kVstTransportPlaying = 1 << 1, kVstPpqPosValid = 1 << 9, kVstTempoValid = 1 << 10 };
typedef struct {
    double samplePos, sampleRate, nanoSeconds, ppqPos, tempo, barStartPos, cycleStartPos, cycleEndPos;
    int32_t timeSigNumerator, timeSigDenominator, smpteOffset, smpteFrameRate, samplesToNextClock, flags;
} VstTimeInfo;
enum { effFlagsCanReplacing = 1 << 4, effFlagsProgramChunks = 1 << 5 };

static FILE *g_log;
static std::mutex g_logMutex;
#define LOG(...) do { std::lock_guard<std::mutex> lk_(g_logMutex); \
    if (g_log) { std::fprintf(g_log, __VA_ARGS__); std::fflush(g_log); } } while (0)

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

/* ---- parameter indices ------------------------------------------------------ */
static int param_index(const char *key) {
    for (int i = 0; i < NPARAMS; i++) if (!std::strcmp(PARAMS[i].key, key)) return i;
    return -1;
}
static int IDX_PRESET, IDX_CARRIER, IDX_PITCH, IDX_ROOT, IDX_CHORD, IDX_SCALE, IDX_OCTAVE, IDX_GLIDE,
    IDX_SPREAD, IDX_BANDS, IDX_SHIFT, IDX_Q, IDX_RELEASE, IDX_SIB, IDX_UNVOICED, IDX_FREEZE, IDX_CRUSH,
    IDX_RING, IDX_WIDTH, IDX_TGATE, IDX_INPUT, IDX_INGAIN, IDX_GATE, IDX_MIX, IDX_OUTPUT;

enum { C_SAW, C_SQUARE, C_PULSE, C_SUPER, C_BUZZ, C_NOISE };
enum { P_FIXED, P_LATCH, P_MIDI, P_FOLLOW };
static const int BAND_COUNTS[5] = {8, 12, 16, 20, 24};
static const int MAXB = 24;
static const int CHORDS[7][4] = {{0, -1, -1, -1}, {0, 12, -1, -1}, {0, 7, 12, -1}, {0, 3, 7, 12},
                                 {0, 4, 7, 12},   {0, 5, 7, 12},   {0, 3, 7, 10}};
static const int SCALES[6] = {0xFFF, 0xFFF, 0xAB5, 0x5AD, 0x5AB, 0x4A9};   /* bit n = semitone n above ROOT */
static const char *const NOTE_NAMES[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static const double GATE_STEPS[5] = {0, 1.0, 0.5, 0.25, 0.125};   /* quarter notes */

/* ---- presets ---------------------------------------------------------------- */
struct Factory { const char *name; const char *values; };
/* Only what differs from the parameter defaults (module.json). */
static const Factory FACTORY[] = {
    {"CLASSIC ROBOT", ""},
    {"AUTOTUNE BOT", "carrier=3;pitch_mode=3;scale=3;glide=8;spread=18;bands=3;q=45;unvoiced=45;"},
    {"CYLON", "carrier=1;root=41;bands=0;q=70;crush=35;width=0;release=20;"},
    {"RING DROID", "root=48;bands=1;ring=9;q=50;width=10;"},
    {"MACHINE CHOIR", "carrier=3;chord=6;spread=40;bands=3;width=85;q=30;release=50;"},
    {"VOCODER 78", "carrier=2;root=43;chord=2;bands=1;q=55;release=35;sibilance=50;"},
    {"WHISPER UNIT", "carrier=5;bands=3;q=30;sibilance=50;unvoiced=0;width=60;"},
    {"GIANT MECH", "root=28;chord=1;shift=-7;crush=10;q=50;width=20;"},
    {"NANO BOT", "carrier=4;root=64;shift=7;bands=1;q=50;"},
    {"FREEZE PAD", "carrier=3;chord=3;spread=35;release=80;width=75;q=35;"},
    {"BITCRUSH UNIT", "carrier=1;root=38;crush=60;bands=0;tgate=3;"},
    {"GOA ORACLE", "carrier=3;pitch_mode=3;scale=4;chord=2;spread=30;width=70;bands=3;glide=20;"},
    {"TRANCE GATE", "pitch_mode=1;chord=3;tgate=3;width=50;"},
    {"MIDI KEYS", "pitch_mode=2;bands=3;width=40;"},
};
static const int NFACTORY = (int)(sizeof FACTORY / sizeof FACTORY[0]);
static const int NSLOTS = 32;
/* Keys saved in the chunk and the slot files (everything but the triggers and popup flags). */
static bool saved_key(int i) { return !PARAMS[i].momentary && !popup_is(i) && i != IDX_PRESET; }

/* ---- small DSP blocks ------------------------------------------------------- */
struct Biquad {
    float b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    inline float run(float x) { float y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
    void bandpass(double f, double q, double sr) {   /* RBJ, 0 dB peak; keeps the state */
        const double w = 2 * M_PI * std::min(f, sr * 0.45) / sr, al = std::sin(w) / (2 * q), a0 = 1 + al;
        b0 = (float)(al / a0); b1 = 0; b2 = (float)(-al / a0); a1 = (float)(-2 * std::cos(w) / a0); a2 = (float)((1 - al) / a0);
    }
    void highpass(double f, double q, double sr) {
        const double w = 2 * M_PI * f / sr, al = std::sin(w) / (2 * q), c = std::cos(w), a0 = 1 + al;
        b0 = (float)((1 + c) / 2 / a0); b1 = (float)(-(1 + c) / a0); b2 = b0; a1 = (float)(-2 * c / a0); a2 = (float)((1 - al) / a0);
    }
    void clear() { z1 = z2 = 0; }
};
struct Band { Biquad m1, m2, c1, c2; float env = 0, gl = 1, gr = 1; };

static inline float blep(float t, float dt) {
    if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
    if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
    return 0.0f;
}
static inline float saw(float t, float dt) { return 2.0f * t - 1.0f - blep(t, dt); }
static inline float pulse(float t, float dt, float pw) {
    float v = t < pw ? 1.0f : -1.0f;
    v += blep(t, dt);
    float t2 = t + 1.0f - pw; if (t2 >= 1.0f) t2 -= 1.0f;
    v -= blep(t2, dt);
    return v - (2.0f * pw - 1.0f);
}

/* ---- pitch tracker (FOLLOW): YIN on the input decimated to ~11 kHz ----------- */
struct Tracker {
    static const int BUF = 1024, W = 300, TMIN = 14, TMAX = 220, HOP = 96;
    float buf[BUF] = {0};
    int wpos = 0, hop = 0, dec = 4, dcount = 0;
    float lp = 0, acc = 0;
    double dsr = 11025;
    float last_note = -1;                 /* -1: nothing tracked yet */
    float hist[3] = {-1, -1, -1};
    void setup(double sr) { dec = std::max(1, (int)std::lround(sr / 11025.0)); dsr = sr / dec; }
    /* one input sample; returns true when a new estimate is in last_note */
    bool push(float x, bool voiced_gate) {
        lp += 0.28f * (x - lp);
        acc += lp;
        if (++dcount < dec) return false;
        buf[wpos] = acc / dec; wpos = (wpos + 1) & (BUF - 1);
        dcount = 0; acc = 0;
        if (++hop < HOP) return false;
        hop = 0;
        return voiced_gate && analyse();
    }
    bool analyse() {
        float x[W + TMAX];
        const int start = (wpos - (W + TMAX)) & (BUF - 1);
        double energy = 0;
        for (int i = 0; i < W + TMAX; i++) { x[i] = buf[(start + i) & (BUF - 1)]; }
        for (int i = 0; i < W; i++) energy += (double)x[i] * x[i];
        if (energy / W < 1e-7) return false;
        float d[TMAX + 1];
        d[0] = 0;
        float run = 0;
        int best = -1;
        float cm_prev = 1, dmin = 1e9f; int tmin_i = -1;
        for (int t = 1; t <= TMAX; t++) {
            float s = 0;
            const float *a = x, *b = x + t;
            for (int j = 0; j < W; j++) { const float e = a[j] - b[j]; s += e * e; }
            d[t] = s;
            run += s;
            if (t < TMIN) continue;
            const float cm = run > 0 ? s * t / run : 1;
            if (cm < dmin) { dmin = cm; tmin_i = t; }
            if (best < 0 && cm < 0.15f) best = t;
            else if (best > 0 && t == best + 1 && cm < cm_prev) best = t;   /* walk down to the local minimum */
            cm_prev = cm;
            if (best > 0 && t > best + 1) break;
        }
        if (best < 0) { if (dmin < 0.3f) best = tmin_i; else return false; }
        float tau = (float)best;
        if (best > TMIN && best < TMAX) {
            const float a = d[best - 1], b = d[best], c = d[best + 1], den = a - 2 * b + c;
            if (den > 0) tau += 0.5f * (a - c) / den;
        }
        const float f = (float)(dsr / tau);
        if (f < 50 || f > 800) return false;
        const float n = 69.0f + 12.0f * std::log2(f / 440.0f);
        hist[0] = hist[1]; hist[1] = hist[2]; hist[2] = n;
        if (hist[0] < 0) { last_note = n; return true; }
        float a = hist[0], b = hist[1], c = hist[2];   /* median of three: no octave hiccups */
        last_note = std::max(std::min(a, b), std::min(std::max(a, b), c));
        return true;
    }
};

/* ---- ALSA sequencer MIDI-in port, libasound opened at run time ------------- */
struct Alsa {
    void *lib = nullptr;
    int (*seq_open)(void **, const char *, int, int) = nullptr;
    int (*seq_close)(void *) = nullptr;
    int (*set_client_name)(void *, const char *) = nullptr;
    int (*create_simple_port)(void *, const char *, unsigned, unsigned) = nullptr;
    int (*nonblock)(void *, int) = nullptr;
    int (*poll_count)(void *, short) = nullptr;
    int (*poll_desc)(void *, struct pollfd *, unsigned, short) = nullptr;
    int (*event_input)(void *, void **) = nullptr;
    int (*client_id)(void *) = nullptr;
    int (*me_new)(size_t, void **) = nullptr;
    void (*me_free)(void *) = nullptr;
    void (*me_no_status)(void *, int) = nullptr;
    long (*me_decode)(void *, unsigned char *, long, const void *) = nullptr;
    bool load() {
        if (lib) return true;
        lib = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
        if (!lib) return false;
#define SYM(f, n) f = (decltype(f))dlsym(lib, n); if (!f) { dlclose(lib); lib = nullptr; return false; }
        SYM(seq_open, "snd_seq_open") SYM(seq_close, "snd_seq_close") SYM(set_client_name, "snd_seq_set_client_name")
        SYM(create_simple_port, "snd_seq_create_simple_port") SYM(nonblock, "snd_seq_nonblock")
        SYM(poll_count, "snd_seq_poll_descriptors_count") SYM(poll_desc, "snd_seq_poll_descriptors")
        SYM(event_input, "snd_seq_event_input") SYM(client_id, "snd_seq_client_id")
        SYM(me_new, "snd_midi_event_new") SYM(me_free, "snd_midi_event_free")
        SYM(me_no_status, "snd_midi_event_no_status") SYM(me_decode, "snd_midi_event_decode")
#undef SYM
        return true;
    }
};
static Alsa g_alsa;
static std::mutex g_alsaMutex;

/* ---- per-instance state ----------------------------------------------------- */
struct Msg { uint8_t b[3]; };

struct Plugin {
    AEffect fx;
    audioMasterCallback master = nullptr;
    std::atomic<float> cache[NPARAMS];
    std::atomic<int> notify[NPARAMS];
    float open[NPARAMS] = {0};
    volatile int release[NPARAMS] = {0};
    std::atomic<bool> update_display{false};
    float sr = 44100.0f;
    std::vector<uint8_t> chunk;
    std::mutex uiMutex;                   /* slot names, chunk */
    std::string slot_names[NSLOTS];
    std::atomic<int> program{0};
    /* MIDI from the ALSA port and from VST events -> the audio thread (single consumer) */
    Msg mq[512];
    std::atomic<int> mq_w{0}, mq_r{0};
    std::mutex mqWrite;                   /* two producers: the ALSA thread and effProcessEvents */
    std::thread midi_thread;
    std::atomic<bool> run{true};
    /* --- DSP, audio thread only --- */
    Band bands[MAXB];
    int nb = 0, cur_bands = -1, cur_shift = 999, cur_q = -1;
    double cur_sr = 0;
    Biquad hp1, hp2, hfm1, hfm2;          /* sibilance; the mic's highs for UNVOICED */
    float env_att = 0, env_rel = 0;
    float in_env = 0, gate_gain = 0, hf_env = 0, full_env = 0, hf_ratio = 0;
    bool gate_open = false;
    long idle = 0;
    bool idle_now = false;
    double ph[4][3] = {{0}};              /* carrier phases: 4 voices x 3 (SUPER) */
    float vf[4] = {0}, vt[4] = {0};       /* voice frequency now / target */
    int nv = 0;
    float pre_x = 0;                      /* carrier pre-emphasis state */
    uint32_t noise = 22222;
    std::vector<int> held;                /* MIDI notes held, oldest first */
    int latched = -1;
    Tracker trk;
    float follow_note = -1;
    double ring_ph = 0;
    float crush_hold_l = 0, crush_hold_r = 0;
    int crush_count = 0;
    float tg_gain = 1;
    float mix_s = 1, out_s = 1;           /* smoothed */
    double load_acc = 0, load_audio = 0, load_peak = 0;
};

/* ---- parameter helpers ------------------------------------------------------ */
static void copy_str(void *dst, const char *s, size_t max) {
    std::strncpy((char *)dst, s, max - 1);
    ((char *)dst)[max - 1] = 0;
}
static int norm_to_ui(const param_t *p, float n) {
    if (p->nopts) return (int)std::lround(clamp01(n) * (p->nopts - 1));
    return (int)std::lround(p->min + (p->max - p->min) * clamp01(n));
}
static float ui_to_norm(const param_t *p, double v) {
    if (p->nopts) return p->nopts > 1 ? clamp01((float)(v / (p->nopts - 1))) : 0.0f;
    return p->max > p->min ? clamp01((float)((v - p->min) / (p->max - p->min))) : 0.0f;
}
static int ui(Plugin *w, int i) { return i < 0 ? 0 : norm_to_ui(&PARAMS[i], w->cache[i].load()); }
static void set_ui(Plugin *w, int i, int v) {
    if (i < 0) return;
    w->cache[i].store(ui_to_norm(&PARAMS[i], v));
    w->notify[i].store(1);
}

/* ---- MIDI queue ------------------------------------------------------------- */
static void load_slot(Plugin *w, int slot);
static void push_msg(Plugin *w, const uint8_t *b) {
    if ((b[0] & 0xF0) == 0xC0) {             /* program change 0-31: a slot, here, not in the audio thread */
        if (b[1] < NSLOTS) load_slot(w, b[1]);
        return;
    }
    std::lock_guard<std::mutex> lk(w->mqWrite);
    const int wr = w->mq_w.load(std::memory_order_relaxed);
    if (wr - w->mq_r.load(std::memory_order_acquire) >= 512) return;
    std::memcpy(w->mq[wr & 511].b, b, 3);
    w->mq_w.store(wr + 1, std::memory_order_release);
}

static void midi_thread_main(Plugin *w) {
    if (std::getenv("ROBOVOX_NO_ALSA")) return;
    {
        std::lock_guard<std::mutex> lk(g_alsaMutex);
        if (!g_alsa.load()) { LOG("[robovox] no libasound: MIDI port off\n"); return; }
    }
    void *seq = nullptr, *dec = nullptr;
    if (g_alsa.seq_open(&seq, "default", 2 /* SND_SEQ_OPEN_INPUT */, 0) < 0) { LOG("[robovox] snd_seq_open failed\n"); return; }
    g_alsa.set_client_name(seq, "Robovox");
    const int port = g_alsa.create_simple_port(seq, "MIDI In",
        (1u << 1) | (1u << 6) /* CAP_WRITE | CAP_SUBS_WRITE */, (1u << 1) | (1u << 20) /* MIDI_GENERIC | APPLICATION */);
    g_alsa.nonblock(seq, 1);
    g_alsa.me_new(64, &dec);
    if (dec) g_alsa.me_no_status(dec, 1);
    LOG("[robovox] ALSA client %d port %d \"Robovox MIDI In\"\n", g_alsa.client_id(seq), port);
    struct pollfd pfd[4];
    int np = std::min(4, g_alsa.poll_count(seq, POLLIN));
    np = np > 0 ? g_alsa.poll_desc(seq, pfd, (unsigned)np, POLLIN) : 0;
    while (w->run.load()) {
        if (np > 0) ::poll(pfd, (nfds_t)np, 50);
        else std::this_thread::sleep_for(std::chrono::milliseconds(2));
        void *ev = nullptr;
        while (w->run.load() && g_alsa.event_input(seq, &ev) >= 0 && ev) {
            unsigned char b[16];
            const long n = dec ? g_alsa.me_decode(dec, b, sizeof b, ev) : -1;
            if (n >= 2 && b[0] >= 0x80 && b[0] < 0xF0) {
                uint8_t m[3] = {b[0], b[1], n >= 3 ? b[2] : (uint8_t)0};
                push_msg(w, m);
            }
            ev = nullptr;
        }
    }
    if (dec) g_alsa.me_free(dec);
    g_alsa.seq_close(seq);
}

/* ---- presets: factory, user files ------------------------------------------- */
static std::string plugin_dir() {
    Dl_info info;
    if (dladdr((void *)&plugin_dir, &info) && info.dli_fname) {
        std::string p = info.dli_fname;
        size_t s = p.rfind('/');
        if (s != std::string::npos) return p.substr(0, s);
    }
    return "/tmp";
}
static std::string user_dir() { return plugin_dir() + "/robovox_user"; }
static std::string slot_path(int slot) {   /* slot 0..31 */
    char buf[32];
    std::snprintf(buf, sizeof buf, "/slot_%02d.txt", slot + 1);
    return user_dir() + buf;
}
static bool read_text(const std::string &path, std::string &out) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[1024];
    size_t n;
    out.clear();
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0 && out.size() < 16384) out.append(buf, n);
    std::fclose(f);
    return !out.empty();
}
/* "key=value;..." -> parameters. Returns the name= entry if any. Unknown keys are skipped. */
static std::string apply_text(Plugin *w, const std::string &t) {
    std::string name;
    size_t pos = 0;
    while (pos < t.size()) {
        size_t semi = t.find(';', pos);
        if (semi == std::string::npos) semi = t.size();
        std::string kv = t.substr(pos, semi - pos);
        pos = semi + 1;
        size_t eq = kv.find('=');
        if (eq == std::string::npos) continue;
        std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
        while (!k.empty() && (k[0] == '\n' || k[0] == '\r' || k[0] == ' ')) k.erase(0, 1);
        if (k == "name") { name = v; continue; }
        const int i = param_index(k.c_str());
        if (i >= 0 && saved_key(i)) set_ui(w, i, std::atoi(v.c_str()));
    }
    return name;
}
static std::string settings_text(Plugin *w) {
    std::string t;
    char buf[64];
    for (int i = 0; i < NPARAMS; i++) {
        if (!saved_key(i)) continue;
        std::snprintf(buf, sizeof buf, "%s=%d;", PARAMS[i].key, ui(w, i));
        t += buf;
    }
    return t;
}
static void reset_defaults(Plugin *w) {
    for (int i = 0; i < NPARAMS; i++)
        if (saved_key(i)) { w->cache[i].store(PARAMS[i].def); w->notify[i].store(1); }
}
static void scan_slots(Plugin *w) {
    std::lock_guard<std::mutex> lk(w->uiMutex);
    for (int s = 0; s < NSLOTS; s++) {
        std::string t;
        std::string nm = s < NFACTORY ? FACTORY[s].name : "INIT";
        if (read_text(slot_path(s), t)) {
            size_t p = t.find("name=");
            if (p != std::string::npos) { size_t e = t.find(';', p); nm = t.substr(p + 5, e == std::string::npos ? std::string::npos : e - p - 5); }
        }
        w->slot_names[s] = nm;
    }
}
static void load_slot(Plugin *w, int slot) {
    slot = clampi(slot, 0, NSLOTS - 1);
    reset_defaults(w);
    std::string t;
    if (read_text(slot_path(slot), t)) apply_text(w, t);
    else if (slot < NFACTORY) apply_text(w, FACTORY[slot].values);
    w->program.store(slot);
    set_ui(w, IDX_PRESET, slot + 1);
    w->update_display.store(true);
    std::string nm;
    { std::lock_guard<std::mutex> lk(w->uiMutex); nm = w->slot_names[slot]; }
    LOG("[robovox] slot %d \"%s\"\n", slot + 1, nm.c_str());
}
static void save_slot(Plugin *w, int slot) {
    slot = clampi(slot, 0, NSLOTS - 1);
    ::mkdir(user_dir().c_str(), 0755);
    std::string nm;
    {
        std::lock_guard<std::mutex> lk(w->uiMutex);
        if (w->slot_names[slot] == "INIT" || w->slot_names[slot].empty()) {
            char b[24]; std::snprintf(b, sizeof b, "USER %02d", slot + 1); w->slot_names[slot] = b;
        }
        nm = w->slot_names[slot];
    }
    const std::string t = "name=" + nm + ";" + settings_text(w) + "\n";
    FILE *f = std::fopen(slot_path(slot).c_str(), "wb");
    if (!f) { LOG("[robovox] cannot write %s\n", slot_path(slot).c_str()); return; }
    std::fwrite(t.data(), 1, t.size(), f);
    std::fclose(f);
    w->program.store(slot);
    w->update_display.store(true);
    LOG("[robovox] saved slot %d \"%s\"\n", slot + 1, nm.c_str());
}

/* ---- audio ------------------------------------------------------------------ */
struct NoDenormals {
#if defined(__arm__) && defined(__ARM_FP)
    uint32_t old = 0;
    NoDenormals() { asm volatile("vmrs %0, fpscr" : "=r"(old)); asm volatile("vmsr fpscr, %0" : : "r"(old | (1u << 24))); }
    ~NoDenormals() { asm volatile("vmsr fpscr, %0" : : "r"(old)); }
#elif defined(__x86_64__) || defined(__i386__)
    unsigned old = __builtin_ia32_stmxcsr();
    NoDenormals() { __builtin_ia32_ldmxcsr(old | 0x8040); }
    ~NoDenormals() { __builtin_ia32_ldmxcsr(old); }
#endif
};

static inline float white(uint32_t &s) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return (int32_t)s * (1.0f / 2147483648.0f);
}

static void design(Plugin *w, int nb, int shift, int qk) {
    const double sr = w->sr;
    const double lo = 120.0, hi = 7500.0;
    const double q = nb * 0.42 * (0.5 + 2.0 * qk / 100.0);
    const double sh = std::pow(2.0, shift / 12.0);
    if (nb != w->nb) for (int k = 0; k < MAXB; k++) { Band &b = w->bands[k]; b.m1.clear(); b.m2.clear(); b.c1.clear(); b.c2.clear(); b.env = 0; }
    w->nb = nb;
    for (int k = 0; k < nb; k++) {
        const double f = lo * std::pow(hi / lo, (k + 0.5) / nb);
        Band &b = w->bands[k];
        b.m1.bandpass(f, q, sr); b.m2.bandpass(f, q, sr);
        b.c1.bandpass(f * sh, q, sr); b.c2.bandpass(f * sh, q, sr);
    }
    w->hp1.highpass(5500, 0.7, sr); w->hp2.highpass(5500, 0.7, sr);
    w->hfm1.highpass(3500, 0.7, sr); w->hfm2.highpass(3500, 0.7, sr);
    w->env_att = (float)(1 - std::exp(-1.0 / (0.0015 * sr)));
    w->trk.setup(sr);
    w->cur_bands = nb; w->cur_shift = shift; w->cur_q = qk; w->cur_sr = sr;
}

static float quantize(float note, int root, int scale) {
    if (scale == 0) return note;
    const int mask = SCALES[scale];
    const int n0 = (int)std::lround(note);
    for (int d = 0; d < 12; d++) {                     /* the nearest note of the scale */
        for (int sgn : {1, -1}) {
            const int n = n0 + sgn * d;
            if (mask >> (((n - root) % 12 + 12) % 12) & 1) return (float)n;
        }
    }
    return (float)n0;
}

static void handle_midi(Plugin *w, const uint8_t *b) {
    const uint8_t t = b[0] & 0xF0;
    if (t == 0x90 && b[2] > 0) {
        w->held.erase(std::remove(w->held.begin(), w->held.end(), (int)b[1]), w->held.end());
        w->held.push_back(b[1]);
        if (w->held.size() > 16) w->held.erase(w->held.begin());
        w->latched = b[1];
    } else if (t == 0x80 || t == 0x90) {
        w->held.erase(std::remove(w->held.begin(), w->held.end(), (int)b[1]), w->held.end());
    } else if (t == 0xB0 && (b[1] == 120 || b[1] == 123)) {
        w->held.clear();
    }
}

static void processReplacing(AEffect *e, float **in, float **out, int32_t n) {
    Plugin *w = (Plugin *)e->object;
    const auto t0 = std::chrono::steady_clock::now();
    NoDenormals nd;
    /* MIDI first */
    while (w->mq_r.load() != w->mq_w.load(std::memory_order_acquire)) {
        const int r = w->mq_r.load();
        Msg m = w->mq[r & 511];
        w->mq_r.store(r + 1, std::memory_order_release);
        handle_midi(w, m.b);
    }
    /* settings, once per block */
    const int carrier = ui(w, IDX_CARRIER), pmode = ui(w, IDX_PITCH), root = ui(w, IDX_ROOT);
    const int chord = clampi(ui(w, IDX_CHORD), 0, 6), scale = clampi(ui(w, IDX_SCALE), 0, 5), oct = ui(w, IDX_OCTAVE);
    const int nbands = BAND_COUNTS[clampi(ui(w, IDX_BANDS), 0, 4)], shift = ui(w, IDX_SHIFT), qk = ui(w, IDX_Q);
    if (nbands != w->cur_bands || shift != w->cur_shift || qk != w->cur_q || w->sr != w->cur_sr) design(w, nbands, shift, qk);
    const float rel_s = 0.008f * std::pow(2.0f, ui(w, IDX_RELEASE) / 100.0f * 5.6f);
    w->env_rel = 1 - std::exp(-1.0f / (rel_s * w->sr));
    const float sib = ui(w, IDX_SIB) / 100.0f, unv = ui(w, IDX_UNVOICED) / 100.0f;
    const bool freeze = ui(w, IDX_FREEZE) != 0;
    const int crush = ui(w, IDX_CRUSH), ringk = ui(w, IDX_RING), tgate = clampi(ui(w, IDX_TGATE), 0, 4);
    const float width = ui(w, IDX_WIDTH) / 100.0f, spread = ui(w, IDX_SPREAD) / 100.0f;
    const int input = ui(w, IDX_INPUT);
    const float in_gain = std::pow(10.0f, ui(w, IDX_INGAIN) / 20.0f);
    const int gatek = ui(w, IDX_GATE);
    const float gate_thr = gatek > 0 ? std::pow(10.0f, (-80.0f + gatek * 0.6f) / 20.0f) : 0.0f;
    const float mix_t = ui(w, IDX_MIX) / 100.0f, out_t = std::pow(10.0f, ui(w, IDX_OUTPUT) / 20.0f);
    const float glide_s = ui(w, IDX_GLIDE) / 100.0f * 0.4f;
    const float glide_c = glide_s > 0.0005f ? 1 - std::exp(-1.0f / (glide_s * w->sr)) : 1.0f;

    /* band panning (WIDTH) */
    for (int k = 0; k < w->nb; k++) {
        const float p = ((k & 1) ? 1.0f : -1.0f) * width;
        w->bands[k].gl = std::sqrt(1.0f - p); w->bands[k].gr = std::sqrt(1.0f + p);
    }

    /* carrier pitches for this block */
    float notes[4];
    int nn = 0;
    if (pmode == P_MIDI) {
        for (int k = std::max(0, (int)w->held.size() - 4); k < (int)w->held.size(); k++) notes[nn++] = (float)(w->held[(size_t)k] + 12 * oct);
    } else {
        float base = (float)root;
        if (pmode == P_LATCH && w->latched >= 0) base = (float)w->latched;
        if (pmode == P_FOLLOW && w->follow_note >= 0) base = quantize(w->follow_note, root, scale);
        base += 12.0f * oct;
        for (int k = 0; k < 4 && CHORDS[chord][k] >= 0; k++) notes[nn++] = base + CHORDS[chord][k];
    }
    for (int k = 0; k < nn; k++) {
        const float f = 440.0f * std::pow(2.0f, (std::min(notes[k], 120.0f) - 69.0f) / 12.0f);
        if (k >= w->nv) w->vf[k] = f;   /* a new voice starts at its pitch */
        w->vt[k] = f;
    }
    w->nv = nn;
    const float vnorm = nn > 0 ? 1.0f / std::sqrt((float)nn) : 0.0f;

    /* trance gate (host transport) */
    double ppq = 0, ppq_step = 0;
    bool tg_on = false;
    if (tgate > 0 && w->master) {
        VstTimeInfo *ti = (VstTimeInfo *)w->master(&w->fx, audioMasterGetTime, 0, kVstPpqPosValid | kVstTempoValid, 0, 0);
        if (ti && (ti->flags & kVstTransportPlaying) && (ti->flags & kVstPpqPosValid) && ti->tempo > 0) {
            tg_on = true; ppq = ti->ppqPos; ppq_step = ti->tempo / 60.0 / w->sr;
        }
    }
    const double tg_len = GATE_STEPS[tgate];
    const float smooth2ms = 1 - std::exp(-1.0f / (0.002f * w->sr));
    const float gate_att = 1 - std::exp(-1.0f / (0.003f * w->sr)), gate_rel = 1 - std::exp(-1.0f / (0.12f * w->sr));
    const float in_att = 1 - std::exp(-1.0f / (0.001f * w->sr)), in_rel = 1 - std::exp(-1.0f / (0.08f * w->sr));
    const float hf_c = 1 - std::exp(-1.0f / (0.01f * w->sr));
    const float sm_c = 1 - std::exp(-1.0f / (0.02f * w->sr));
    const double ring_inc = ringk > 0 ? 20.0 * std::pow(2.0, ringk / 100.0 * 6.5) / w->sr : 0;
    const int crush_ds = 1 + (int)(crush * crush / 10000.0f * 23);
    const float crush_q = crush > 0 ? std::pow(2.0f, 15.0f - crush * 0.09f) : 0;
    const float makeup = 17.0f * std::sqrt((float)w->nb);
    const int nbands_now = w->nb;
    const float spr = spread * 0.012f;
    const float pw = carrier == C_PULSE ? 0.18f : 0.05f;

    for (int i = 0; i < n; i++) {
        const float il = in ? in[0][i] : 0.0f, ir = in ? in[1][i] : 0.0f;
        float x = (input == 1 ? il : input == 2 ? ir : 0.5f * (il + ir)) * in_gain;
        /* noise gate on the mic */
        const float ax = std::fabs(x);
        w->in_env += (ax > w->in_env ? in_att : in_rel) * (ax - w->in_env);
        if (gate_thr <= 0) w->gate_open = true;
        else if (w->in_env > gate_thr) w->gate_open = true;
        else if (w->in_env < gate_thr * 0.5f) w->gate_open = false;
        w->gate_gain += (w->gate_open ? gate_att : gate_rel) * ((w->gate_open ? 1.0f : 0.0f) - w->gate_gain);
        const float mod = x * w->gate_gain;
        w->mix_s += sm_c * (mix_t - w->mix_s);
        w->out_s += sm_c * (out_t - w->out_s);

        if (pmode == P_FOLLOW && w->trk.push(x, w->gate_open && w->in_env > 1e-3f)) w->follow_note = w->trk.last_note;

        /* idle: no input and nothing ringing -> skip the vocoder */
        if ((i & 63) == 0) {
            float emax = 0;
            for (int k = 0; k < nbands_now; k++) emax = std::max(emax, w->bands[k].env);
            const bool quiet = !freeze && emax < 1e-5f &&
                               (gate_thr > 0 ? (!w->gate_open && w->gate_gain < 1e-3f) : w->in_env < 1e-5f);
            if (quiet) w->idle += 64; else w->idle = 0;
            const bool was = w->idle_now;
            w->idle_now = w->idle > (long)(0.3f * w->sr);
            if (w->idle_now && !was) {
                for (int k = 0; k < MAXB; k++) { Band &b = w->bands[k]; b.m1.clear(); b.m2.clear(); b.c1.clear(); b.c2.clear(); b.env = 0; }
                w->hp1.clear(); w->hp2.clear(); w->hfm1.clear(); w->hfm2.clear(); w->pre_x = 0;
            }
        }
        float wl = 0, wr = 0;
        if (!w->idle_now) {
            /* carrier */
            float c = 0;
            if (carrier == C_NOISE) {
                c = nn > 0 ? white(w->noise) * 0.35f : 0.0f;
            } else {
                for (int v = 0; v < nn; v++) {
                    w->vf[v] += glide_c * (w->vt[v] - w->vf[v]);
                    const float dt = std::min(0.45f, w->vf[v] / w->sr);
                    if (carrier == C_SUPER) {
                        float s = 0;
                        for (int u = 0; u < 3; u++) {
                            const float d = dt * (1.0f + (u - 1) * spr * (1.0f + 0.13f * v));
                            const float t = (float)w->ph[v][u];
                            s += saw(t, d);
                            w->ph[v][u] += d; if (w->ph[v][u] >= 1.0) w->ph[v][u] -= 1.0;
                        }
                        c += s * 0.6f;
                    } else {
                        const float t = (float)w->ph[v][0];
                        c += carrier == C_SAW ? saw(t, dt) : carrier == C_SQUARE ? pulse(t, dt, 0.5f) : pulse(t, dt, pw);
                        w->ph[v][0] += dt; if (w->ph[v][0] >= 1.0) w->ph[v][0] -= 1.0;
                    }
                }
                c *= vnorm;
                const float pe = c - 0.85f * w->pre_x;   /* pre-emphasis: a flatter spectrum, clearer words */
                w->pre_x = c;
                c = pe;
            }
            /* UNVOICED: noise into the carrier while the mic is mostly hiss (s, t, f, sh) */
            const float hf = std::fabs(w->hfm2.run(w->hfm1.run(mod)));
            w->hf_env += hf_c * (hf - w->hf_env);
            w->full_env += hf_c * (std::fabs(mod) - w->full_env);
            const float ratio = w->full_env > 1e-6f ? std::min(1.0f, w->hf_env / w->full_env * 1.6f) : 0.0f;
            w->hf_ratio += 0.01f * (ratio - w->hf_ratio);
            if (unv > 0 && nn > 0) c += white(w->noise) * unv * w->hf_ratio * w->hf_ratio * 1.5f;
            /* the bands */
            for (int k = 0; k < nbands_now; k++) {
                Band &b = w->bands[k];
                if (!freeze) {
                    const float e = std::fabs(b.m2.run(b.m1.run(mod)));
                    b.env += (e > b.env ? w->env_att : w->env_rel) * (e - b.env);
                }
                const float y = b.c2.run(b.c1.run(c)) * b.env;
                wl += y * b.gl; wr += y * b.gr;
            }
            const float s = sib * 0.8f * w->hp2.run(w->hp1.run(mod));
            wl = wl * makeup + s; wr = wr * makeup + s;
            /* ROBOT */
            if (ringk > 0) {
                const float rm = std::sin((float)(2 * M_PI * w->ring_ph));
                w->ring_ph += ring_inc; if (w->ring_ph >= 1) w->ring_ph -= 1;
                wl *= rm; wr *= rm;
            }
            if (crush > 0) {
                if (w->crush_count <= 0) {
                    w->crush_hold_l = std::round(wl * crush_q) / crush_q;
                    w->crush_hold_r = std::round(wr * crush_q) / crush_q;
                    w->crush_count = crush_ds;
                }
                w->crush_count--;
                wl = w->crush_hold_l; wr = w->crush_hold_r;
            }
        }
        if (tgate > 0) {
            float target = 1;
            if (tg_on) { const double p = ppq + i * ppq_step; target = std::fmod(p, tg_len) < tg_len * 0.5 ? 1.0f : 0.0f; }
            w->tg_gain += smooth2ms * (target - w->tg_gain);
            wl *= w->tg_gain; wr *= w->tg_gain;
        } else w->tg_gain = 1;
        /* MIX, OUTPUT, soft knee above -3 dBFS */
        const float m = w->mix_s;
        float ol = (il * (1 - m) + wl * m) * w->out_s, orr = (ir * (1 - m) + wr * m) * w->out_s;
        for (float *pv : {&ol, &orr}) {
            float v = *pv;
            const float a = std::fabs(v);
            if (a > 0.7f) {
                const float t = std::min((a - 0.7f) / 0.3f, 3.0f), t2 = t * t;
                v = std::copysign(0.7f + 0.3f * t * (27 + t2) / (27 + 9 * t2), v);
            }
            *pv = v;
        }
        out[0][i] = ol; out[1][i] = orr;
    }

    /* load meter */
    const double used = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const double real = n / (double)w->sr;
    w->load_acc += used; w->load_audio += real;
    if (real > 0 && used / real > w->load_peak) w->load_peak = used / real;
    if (w->load_audio >= 5.0) {
        LOG("[robovox] load avg %.1f %%, peak %.1f %% (%d bands, %d voices%s)\n", 100.0 * w->load_acc / w->load_audio,
            100.0 * w->load_peak, w->nb, w->nv, w->idle_now ? ", idle" : "");
        w->load_acc = w->load_audio = w->load_peak = 0;
    }
    /* tell the host what changed */
    bool any = false;
    for (int i = 0; i < NPARAMS; i++) {
        if (w->release[i]) { w->release[i] = 0; any = true; w->master(&w->fx, audioMasterAutomate, i, 0, 0, 0.0f); }
        const int f = w->notify[i].exchange(0);
        if (!f) continue;
        any = true;
        w->master(&w->fx, audioMasterAutomate, i, 0, 0, f == 2 ? 0.0f : w->cache[i].load());
    }
    if (w->update_display.exchange(false) || any) w->master(&w->fx, audioMasterUpdateDisplay, 0, 0, 0, 0.0f);
}

static void process_acc(AEffect *e, float **in, float **out, int32_t n) {   /* old accumulating call */
    std::vector<float> l((size_t)n), r((size_t)n);
    float *o[2] = {l.data(), r.data()};
    processReplacing(e, in, o, n);
    for (int i = 0; i < n; i++) { out[0][i] += l[(size_t)i]; out[1][i] += r[(size_t)i]; }
}

/* ---- parameters ------------------------------------------------------------- */
static void setParameter(AEffect *e, int32_t i, float n) {
    Plugin *w = (Plugin *)e->object;
    if (i < 0 || i >= NPARAMS) return;
    const param_t *p = &PARAMS[i];
    if (popup_set(w->open, i, n)) return;
    if (p->momentary) {
        if (n <= 0.5f) return;
        if (!std::strcmp(p->key, "preset_load")) load_slot(w, ui(w, IDX_PRESET) - 1);
        else if (!std::strcmp(p->key, "preset_save")) save_slot(w, ui(w, IDX_PRESET) - 1);
        w->notify[i].store(2);
        return;
    }
    bool nudge = false;
    if (p->nopts > 1) {   /* a Q-Link nudge lands between options -> step one option */
        float pos = clamp01(n) * (p->nopts - 1);
        if (std::fabs(pos - std::round(pos)) > 0.001f) {
            float cur = w->cache[i].load() * (p->nopts - 1);
            int idx = clampi((int)std::lround(cur) + (pos > cur ? 1 : -1), 0, p->nopts - 1);
            n = (float)idx / (p->nopts - 1);
            nudge = true;
        }
    }
    w->cache[i].store(clamp01(n));
    if (!nudge) popup_picked(w->open, w->release, i);
}
static float getParameter(AEffect *e, int32_t i) {
    Plugin *w = (Plugin *)e->object;
    if (i < 0 || i >= NPARAMS) return 0.0f;
    if (popup_is(i)) return w->open[i];
    return PARAMS[i].momentary ? 0.0f : w->cache[i].load();
}

/* ---- chunk: "RVOX" 1, the slot number, settings as text --------------------- */
static intptr_t get_chunk(Plugin *w, void **ptr) {
    char head[32];
    std::snprintf(head, sizeof head, "slot=%d;", w->program.load() + 1);
    const std::string t = std::string(head) + settings_text(w);
    std::lock_guard<std::mutex> lk(w->uiMutex);
    w->chunk = {'R', 'V', 'O', 'X', 1};
    w->chunk.insert(w->chunk.end(), t.begin(), t.end());
    *ptr = w->chunk.data();
    return (intptr_t)w->chunk.size();
}
static intptr_t set_chunk(Plugin *w, const void *data, intptr_t len) {
    const uint8_t *b = (const uint8_t *)data;
    if (!b || len < 5 || std::memcmp(b, "RVOX", 4) || b[4] != 1) return 0;
    const std::string t((const char *)b + 5, (size_t)len - 5);
    reset_defaults(w);
    apply_text(w, t);
    const size_t p = t.find("slot=");
    if (p != std::string::npos) {
        const int s = clampi(std::atoi(t.c_str() + p + 5), 1, NSLOTS) - 1;
        w->program.store(s);
        set_ui(w, IDX_PRESET, s + 1);
    }
    w->update_display.store(true);
    return 1;
}

/* ---- dispatcher ------------------------------------------------------------- */
static std::string slot_label(Plugin *w, int s) {
    std::lock_guard<std::mutex> lk(w->uiMutex);
    return s >= 0 && s < NSLOTS ? w->slot_names[s] : "";
}
static void display(Plugin *w, int idx, char *out) {
    const param_t *pp = &PARAMS[idx];
    const int u = popup_is(idx) ? norm_to_ui(pp, w->open[idx]) : ui(w, idx);
    char buf[48];
    if (idx == IDX_PRESET) std::snprintf(buf, sizeof buf, "%02d %s", u, slot_label(w, u - 1).c_str());
    else if (idx == IDX_ROOT) std::snprintf(buf, sizeof buf, "%s%d", NOTE_NAMES[u % 12], u / 12 - 1);
    else if (idx == IDX_SHIFT) std::snprintf(buf, sizeof buf, "%+d st", u);
    else if (idx == IDX_OCTAVE) std::snprintf(buf, sizeof buf, "%+d", u);
    else if (idx == IDX_INGAIN || idx == IDX_OUTPUT) std::snprintf(buf, sizeof buf, "%+d dB", u);
    else if (idx == IDX_GATE) {
        if (u == 0) std::snprintf(buf, sizeof buf, "OFF");
        else std::snprintf(buf, sizeof buf, "%d dB", (int)std::lround(-80 + u * 0.6));
    } else if (idx == IDX_RING) {
        if (u == 0) std::snprintf(buf, sizeof buf, "OFF");
        else std::snprintf(buf, sizeof buf, "%d Hz", (int)std::lround(20.0 * std::pow(2.0, u / 100.0 * 6.5)));
    } else if (idx == IDX_RELEASE) std::snprintf(buf, sizeof buf, "%d ms", (int)std::lround(8.0 * std::pow(2.0, u / 100.0 * 5.6)));
    else if (idx == IDX_GLIDE) std::snprintf(buf, sizeof buf, "%d ms", u * 4);
    else if (pp->nopts) std::snprintf(buf, sizeof buf, "%s", pp->opts[clampi(u, 0, pp->nopts - 1)]);
    else if (pp->max - pp->min >= 99 && pp->min == 0) std::snprintf(buf, sizeof buf, "%d %%", u);
    else std::snprintf(buf, sizeof buf, "%d", u);
    copy_str(out, buf, 24);
}

static intptr_t dispatcher(AEffect *e, int32_t op, int32_t idx, intptr_t v, void *p, float o) {
    Plugin *w = (Plugin *)e->object;
    switch (op) {
    case effOpen: return 1;
    case effClose:
        LOG("[robovox] closed\n");
        w->run.store(false);
        if (w->midi_thread.joinable()) w->midi_thread.join();
        delete w;
        return 1;
    case effSetProgram:
        if (v >= 0 && v < NSLOTS) load_slot(w, (int)v);
        return 1;
    case effGetProgram: return w->program.load();
    case effSetProgramName: return 1;   /* names come from the slot */
    case effGetProgramName: copy_str(p, slot_label(w, w->program.load()).c_str(), 24); return 1;
    case effGetProgramNameIndexed:
        if (idx < 0 || idx >= NSLOTS) return 0;
        copy_str(p, slot_label(w, idx).c_str(), 24);
        return 1;
    case effGetPlugCategory: return 1;   /* kPlugCategEffect */
    case effGetEffectName:
    case effGetProductString: copy_str(p, PLUG_NAME, 32); return 1;
    case effGetVendorString: copy_str(p, PLUG_VENDOR, 32); return 1;
    case effGetVendorVersion: return PLUG_VERSION;
    case effGetVstVersion: return 2400;
    case effGetTailSize: return (intptr_t)(w->sr * 0.5f);
    case effCanBeAutomated: return idx >= 0 && idx < NPARAMS;
    case effGetParamName:
        if (idx >= 0 && idx < NPARAMS) copy_str(p, PARAMS[idx].name, 32);
        return 1;
    case effGetParamLabel:
        if (idx >= 0 && idx < NPARAMS) copy_str(p, "", 8);
        return 1;
    case effGetParamDisplay:
        if (idx < 0 || idx >= NPARAMS) return 0;
        if (PARAMS[idx].momentary) { copy_str(p, "", 24); return 1; }
        display(w, idx, (char *)p);
        return 1;
    case effSetSampleRate: if (o > 0) w->sr = o; return 1;
    case effSetBlockSize: return 1;
    case effMainsChanged:
        if (!v) { const uint8_t off[3] = {0xB0, 123, 0}; push_msg(w, off); }
        return 1;
    case effProcessEvents: {
        VstEvents *ev = (VstEvents *)p;
        for (int i = 0; ev && i < ev->numEvents; i++) {
            if (ev->events[i]->type != 1) continue;   /* kVstMidiType */
            const VstMidiEvent *me = (const VstMidiEvent *)ev->events[i];
            push_msg(w, me->midiData);
        }
        return 1;
    }
    case effCanDo: {
        const char *s = (const char *)p;
        return (!std::strcmp(s, "receiveVstEvents") || !std::strcmp(s, "receiveVstMidiEvent") || !std::strcmp(s, "bypass")) ? 1 : -1;
    }
    case effGetChunk: return get_chunk(w, (void **)p);
    case effSetChunk: return set_chunk(w, p, v);
    default: return 0;
    }
}

extern "C" __attribute__((visibility("default"))) AEffect *VSTPluginMain(audioMasterCallback master) {
    {
        std::lock_guard<std::mutex> lk(g_logMutex);
        if (!g_log) g_log = std::fopen("/tmp/robovox_vst.log", "a");
    }
    static std::once_flag idx_once;
    std::call_once(idx_once, [] {
        IDX_PRESET = param_index("preset"); IDX_CARRIER = param_index("carrier"); IDX_PITCH = param_index("pitch_mode");
        IDX_ROOT = param_index("root"); IDX_CHORD = param_index("chord"); IDX_SCALE = param_index("scale");
        IDX_OCTAVE = param_index("octave"); IDX_GLIDE = param_index("glide"); IDX_SPREAD = param_index("spread");
        IDX_BANDS = param_index("bands"); IDX_SHIFT = param_index("shift"); IDX_Q = param_index("q");
        IDX_RELEASE = param_index("release"); IDX_SIB = param_index("sibilance"); IDX_UNVOICED = param_index("unvoiced");
        IDX_FREEZE = param_index("freeze"); IDX_CRUSH = param_index("crush"); IDX_RING = param_index("ring");
        IDX_WIDTH = param_index("width"); IDX_TGATE = param_index("tgate"); IDX_INPUT = param_index("input");
        IDX_INGAIN = param_index("in_gain"); IDX_GATE = param_index("gate"); IDX_MIX = param_index("mix");
        IDX_OUTPUT = param_index("output");
    });
    Plugin *w = new Plugin();
    w->master = master;
    w->noise ^= (uint32_t)std::time(nullptr) ^ (uint32_t)(uintptr_t)w;
    if (!w->noise) w->noise = 1;
    for (int i = 0; i < NPARAMS; i++) { w->cache[i].store(PARAMS[i].def); w->notify[i].store(0); }
    scan_slots(w);
    load_slot(w, 0);
    for (int i = 0; i < NPARAMS; i++) w->notify[i].store(0);

    AEffect *e = &w->fx;
    std::memset(e, 0, sizeof *e);
    e->magic = 0x56737450; /* 'VstP' */
    e->dispatcher = dispatcher;
    e->process = process_acc;
    e->setParameter = setParameter;
    e->getParameter = getParameter;
    e->processReplacing = processReplacing;
    e->numPrograms = NSLOTS;
    e->numParams = NPARAMS;
    e->numInputs = 2;
    e->numOutputs = 2;
    e->flags = effFlagsCanReplacing | effFlagsProgramChunks;
    e->uniqueID = PLUG_UID;
    e->version = PLUG_VERSION;
    e->object = w;
    w->midi_thread = std::thread(midi_thread_main, w);
    LOG("[robovox] up, %d params\n", NPARAMS);
    return e;
}
