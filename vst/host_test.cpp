/* Offline x86 test of robovox_vst.cpp (test.sh builds it with ASan/UBSan, then -O2 for the bench).
 * A synthetic "voice" (glottal pulses through vowel formants, syllable envelope) is the input:
 * the output must sound at the carrier's pitch, not the voice's; silence must stay silent and the
 * plugin go idle; every factory slot must sound; LATCH and MIDI follow VST MIDI notes; FOLLOW
 * tracks a sung pitch and snaps it to the scale; chunk restore, programs, slot SAVE/LOAD, no NaN or
 * denormals. With "bench" as the second argument it only measures the real-time factor.
 * Prints PASSED/FAILED; exit code follows. */
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <dlfcn.h>
#include <unistd.h>

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
typedef struct {
    int32_t type, byteSize, deltaFrames, flags, noteLength, noteOffset;
    unsigned char midiData[4];
    char detune, noteOffVelocity, reserved1, reserved2;
} VstMidiEvent;
typedef struct { int32_t numEvents; intptr_t reserved; void *events[64]; } VstEvents;
typedef struct {
    double samplePos, sampleRate, nanoSeconds, ppqPos, tempo, barStartPos, cycleStartPos, cycleEndPos;
    int32_t timeSigNumerator, timeSigDenominator, smpteOffset, smpteFrameRate, samplesToNextClock, flags;
} VstTimeInfo;

static long g_samples = 0;     /* the test's transport: 120 BPM, 4/4, playing */
static VstTimeInfo g_ti;
static intptr_t master(AEffect *, int32_t op, int32_t, intptr_t, void *, float) {
    if (op == 7) {
        std::memset(&g_ti, 0, sizeof g_ti);
        g_ti.sampleRate = 44100; g_ti.samplePos = (double)g_samples;
        g_ti.ppqPos = g_samples / 44100.0 * 2.0; g_ti.tempo = 120;
        g_ti.timeSigNumerator = 4; g_ti.timeSigDenominator = 4;
        g_ti.flags = (1 << 1) | (1 << 9) | (1 << 10) | (1 << 13);
        return (intptr_t)&g_ti;
    }
    return 0;
}
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)
static const int BS = 256;
static const float SR = 44100.0f;
typedef AEffect *(*MainFn)(audioMasterCallback);

static int pidx(AEffect *e, const char *name) {
    char buf[64];
    for (int i = 0; i < e->numParams; i++) {
        buf[0] = 0;
        e->dispatcher(e, 8, i, 0, buf, 0);
        if (!std::strcmp(buf, name)) return i;
    }
    std::printf("no param %s\n", name); std::exit(2);
}
/* sets a parameter by its UI value (ranges as in module.json) */
static void setv(AEffect *e, const char *name, double v, double lo, double hi) {
    e->setParameter(e, pidx(e, name), (float)((v - lo) / (hi - lo)));
}
static void seto(AEffect *e, const char *name, int opt, int nopts) { e->setParameter(e, pidx(e, name), (float)opt / (nopts - 1)); }
static std::string disp(AEffect *e, const char *name) { char b[64] = {0}; e->dispatcher(e, 7, pidx(e, name), 0, b, 0); return b; }

static void midi(AEffect *e, std::vector<std::vector<uint8_t>> msgs) {
    static VstMidiEvent ev[64];
    static VstEvents list;
    list.numEvents = 0;
    for (auto &m : msgs) {
        VstMidiEvent &v = ev[list.numEvents];
        std::memset(&v, 0, sizeof v);
        v.type = 1; v.byteSize = sizeof v;
        for (size_t k = 0; k < m.size() && k < 3; k++) v.midiData[k] = m[k];
        list.events[list.numEvents++] = &v;
    }
    e->dispatcher(e, 25, 0, 0, &list, 0);
}

/* A synthetic voice: a glottal pulse train at f0 through five vowel formants, syllables at 4 Hz. */
struct Voice {
    double f0 = 120, ph = 0, t = 0;
    float z[5][2] = {{0}};
    float cf[5][3];
    bool on = true;
    double syl = 4.0;
    Voice(double f0_ = 120) : f0(f0_) {
        static const double F[5] = {730, 1090, 2440, 3400, 4100}, BW[5] = {90, 110, 160, 250, 300};
        for (int k = 0; k < 5; k++) {
            const double r = std::exp(-M_PI * BW[k] / SR), w = 2 * M_PI * F[k] / SR;
            cf[k][0] = (float)(2 * r * std::cos(w)); cf[k][1] = (float)(-r * r); cf[k][2] = (float)(1 - r);
        }
    }
    uint32_t ns = 12345;
    float next() {
        ph += f0 / SR;
        float src = 0;
        if (ph >= 1) { ph -= 1; src = 1.0f; }
        ns ^= ns << 13; ns ^= ns >> 17; ns ^= ns << 5;
        src += 0.02f * ((int32_t)ns / 2147483648.0f);
        float y = 0;
        for (int k = 0; k < 5; k++) {
            const float o = cf[k][2] * src + cf[k][0] * z[k][0] + cf[k][1] * z[k][1];
            z[k][1] = z[k][0]; z[k][0] = o;
            y += o * (k < 2 ? 1.0f : 0.5f);
        }
        t += 1.0 / SR;
        const float envv = syl > 0 ? (float)std::max(0.0, std::sin(M_PI * std::fmod(t * syl, 1.0))) : 1.0f;
        return on ? y * 3.5f * envv : 0.0f;
    }
};

/* runs seconds with the voice (or silence) on the input; returns the mono output */
static std::vector<float> run(AEffect *e, Voice *v, double seconds, bool *bad = nullptr, std::vector<float> *inrec = nullptr) {
    std::vector<float> il(BS), ir(BS), ol(BS), orr(BS), all;
    float *in[2] = {il.data(), ir.data()}, *out[2] = {ol.data(), orr.data()};
    const int blocks = (int)(seconds * SR / BS);
    for (int b = 0; b < blocks; b++) {
        for (int i = 0; i < BS; i++) { il[i] = ir[i] = v ? v->next() : 0.0f; if (inrec) inrec->push_back(il[i]); }
        e->processReplacing(e, in, out, BS);
        g_samples += BS;
        for (int i = 0; i < BS; i++) {
            for (float s : {ol[i], orr[i]})
                if (!std::isfinite(s) || (s != 0 && std::fabs(s) < 1e-37f)) { if (bad) *bad = true; }
            all.push_back(0.5f * (ol[i] + orr[i]));
        }
    }
    return all;
}
static double rms(const std::vector<float> &a, double t0 = 0, double t1 = 1e9) {
    size_t i0 = (size_t)(t0 * SR), i1 = std::min(a.size(), (size_t)(t1 * SR));
    double s = 0;
    for (size_t i = i0; i < i1; i++) s += (double)a[i] * a[i];
    return i1 > i0 ? std::sqrt(s / (i1 - i0)) : 0;
}
/* fundamental by normalised autocorrelation, 60-600 Hz, over [t0, t0+0.2 s] */
static double f0_of(const std::vector<float> &a, double t0) {
    const size_t i0 = (size_t)(t0 * SR), N = (size_t)(0.2 * SR);
    if (i0 + N + 800 > a.size()) return 0;
    double best = 0; int bt = 0;
    std::vector<double> r(800, 0.0);
    for (int t = 70; t < 740; t++) {
        double s = 0, e0 = 0, e1 = 0;
        for (size_t i = 0; i < N; i++) { s += a[i0 + i] * a[i0 + i + t]; e0 += a[i0 + i] * a[i0 + i]; e1 += a[i0 + i + t] * a[i0 + i + t]; }
        r[t] = s / std::sqrt(e0 * e1 + 1e-20);
    }
    for (int t = 71; t < 739; t++) if (r[t] > best && r[t] >= r[t - 1] && r[t] >= r[t + 1]) { best = r[t]; bt = t; }
    /* the shortest period close to the best (no octave-down picks) */
    for (int t = 71; t < bt; t++) if (r[t] > 0.9 * best && r[t] >= r[t - 1] && r[t] >= r[t + 1]) { bt = t; break; }
    return bt ? SR / bt : 0;
}
static double semis(double f, double ref) { return 12 * std::log2(f / ref); }

int main(int argc, char **argv) {
    if (argc < 2) { std::printf("usage: host_test plugin.so [bench]\n"); return 2; }
    setenv("ROBOVOX_NO_ALSA", "1", 1);
    void *h = dlopen(argv[1], RTLD_NOW);
    if (!h) { std::printf("dlopen: %s\n", dlerror()); return 2; }
    MainFn mainf = (MainFn)dlsym(h, "VSTPluginMain");
    if (!mainf) { std::printf("no VSTPluginMain\n"); return 2; }
    const bool bench = argc > 2 && !std::strcmp(argv[2], "bench");

    AEffect *e = mainf(master);
    e->dispatcher(e, 10, 0, 0, nullptr, 44100.0f);
    e->dispatcher(e, 12, 0, 1, nullptr, 0);

    if (bench) {
        struct Case { const char *name; int slot; int bands; };
        const Case cases[] = {{"CLASSIC ROBOT (16 bands, saw)", 0, 2}, {"MACHINE CHOIR (20, super min7)", 4, 3},
                              {"GOA ORACLE (FOLLOW, 20, super)", 11, 3}, {"worst: 24 bands, super min7", 4, 4}};
        for (const Case &c : cases) {
            e->dispatcher(e, 2, 0, c.slot, nullptr, 0);
            seto(e, "Bands", c.bands, 5);
            Voice v(140);
            run(e, &v, 0.5);
            const auto t0 = std::chrono::steady_clock::now();
            run(e, &v, 10.0);
            const double used = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            std::printf("bench %-34s %5.2f %% of one core\n", c.name, 100.0 * used / 10.0);
        }
        e->dispatcher(e, 2, 0, 0, nullptr, 0);
        run(e, nullptr, 1.0);
        const auto t0 = std::chrono::steady_clock::now();
        run(e, nullptr, 10.0);
        std::printf("bench %-34s %5.2f %% of one core\n", "silent input (idle)",
                    100.0 * std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / 10.0);
        e->dispatcher(e, 1, 0, 0, nullptr, 0);
        return 0;
    }

    /* basics */
    CHECK(e->magic == 0x56737450, "magic");
    CHECK(e->numInputs == 2 && e->numOutputs == 2, "2 in / 2 out (%d/%d)", e->numInputs, e->numOutputs);
    CHECK(e->dispatcher(e, 35, 0, 0, nullptr, 0) == 1, "category effect");
    CHECK(e->numPrograms == 32, "32 programs");
    char nm[64] = {0};
    e->dispatcher(e, 29, 0, 0, nm, 0);
    CHECK(!std::strcmp(nm, "CLASSIC ROBOT"), "program 0 name '%s'", nm);
    e->dispatcher(e, 29, 31, 0, nm, 0);
    CHECK(!std::strcmp(nm, "INIT") || !std::strncmp(nm, "USER", 4), "program 31 name '%s'", nm);
    CHECK(disp(e, "Preset") == "01 CLASSIC ROBOT", "preset display '%s'", disp(e, "Preset").c_str());
    CHECK(disp(e, "Root") == "A2", "root display '%s'", disp(e, "Root").c_str());

    /* 1. classic robot: voice at 120 Hz in, the carrier's A2 (110 Hz) out */
    bool bad = false;
    {
        Voice v(120);
        v.syl = 0;   /* steady vowel for the pitch measurement */
        std::vector<float> in;
        std::vector<float> o = run(e, &v, 1.5, &bad, &in);
        const double ri = rms(in, 0.5, 1.5), ro = rms(o, 0.5, 1.5);
        std::printf("classic: in rms %.4f out rms %.4f (%.1f dB)\n", ri, ro, 20 * std::log10(ro / ri));
        CHECK(ro > ri * 0.3 && ro < ri * 6, "output level %.4f vs input %.4f", ro, ri);
        const double f = f0_of(o, 0.8);
        std::printf("classic: output f0 %.1f Hz (carrier A2 = 110)\n", f);
        CHECK(std::fabs(semis(f, 110.0)) < 0.5, "output pitch %.1f Hz, want 110", f);
    }
    /* 2. syllables: the output follows the voice's envelope */
    {
        Voice v(120);
        std::vector<float> o = run(e, &v, 2.0, &bad);
        double peak = 0, valley = 1e9;
        for (int k = 4; k < 8; k++) {   /* 4 Hz syllables: peaks at 0.125 + k/4 s, gaps at k/4 */
            peak = std::max(peak, rms(o, 0.125 + k * 0.25 - 0.03, 0.125 + k * 0.25 + 0.03));
            valley = std::min(valley, rms(o, k * 0.25 - 0.012, k * 0.25 + 0.012));
        }
        std::printf("syllables: peak %.4f valley %.4f\n", peak, valley);
        CHECK(peak > valley * 3, "envelope follows the voice (peak %.4f, valley %.4f)", peak, valley);
    }
    /* 3. silence: gate closes, output dies, plugin idles */
    {
        std::vector<float> o = run(e, nullptr, 2.0, &bad);
        CHECK(rms(o, 1.0, 2.0) < 1e-5, "silent input -> silence (%.2e)", rms(o, 1.0, 2.0));
        Voice v(120);
        o = run(e, &v, 0.6, &bad);
        CHECK(rms(o, 0.2, 0.6) > 0.01, "wakes up from idle (%.4f)", rms(o, 0.2, 0.6));
    }
    /* 4. every factory slot sounds (MIDI KEYS only with a note) */
    for (int s = 0; s < 14; s++) {
        e->dispatcher(e, 2, 0, s, nullptr, 0);
        char pn[64] = {0};
        e->dispatcher(e, 5, 0, 0, pn, 0);
        Voice v(130);
        if (s == 13) midi(e, {{0x90, 57, 100}});
        std::vector<float> o = run(e, &v, 1.2, &bad);
        const double r = rms(o, 0.4, 1.2);
        std::printf("slot %2d %-16s rms %.4f\n", s + 1, pn, r);
        CHECK(r > 0.01 && r < 0.9, "slot %d '%s' level %.4f", s + 1, pn, r);
        if (s == 13) {
            midi(e, {{0x80, 57, 0}});
            o = run(e, &v, 1.0, &bad);
            CHECK(rms(o, 0.5, 1.0) < 0.003, "MIDI mode silent without a note (%.4f)", rms(o, 0.5, 1.0));
        }
    }
    /* 5. LATCH: a MIDI note becomes the pitch and stays */
    {
        e->dispatcher(e, 2, 0, 0, nullptr, 0);
        seto(e, "Pitch", 1, 4);
        midi(e, {{0x90, 50, 100}, {0x80, 50, 0}});   /* D3 = 146.8 Hz */
        Voice v(120); v.syl = 0;
        std::vector<float> o = run(e, &v, 1.2, &bad);
        const double f = f0_of(o, 0.8);
        std::printf("latch: f0 %.1f Hz (D3 = 146.8)\n", f);
        CHECK(std::fabs(semis(f, 146.83)) < 0.5, "LATCH pitch %.1f", f);
    }
    /* 6. FOLLOW: a voice at 205 Hz (between G#3 and A3), scale A minor -> A3 220 Hz; CHROMATIC -> G#3 207.7 */
    {
        e->dispatcher(e, 2, 0, 0, nullptr, 0);
        seto(e, "Pitch", 3, 4);
        seto(e, "Scale", 3, 6);            /* MINOR, root A */
        Voice v(205); v.syl = 0;
        std::vector<float> o = run(e, &v, 1.5, &bad);
        double f = f0_of(o, 1.0);
        std::printf("follow minor: f0 %.1f Hz (want A3 = 220)\n", f);
        CHECK(std::fabs(semis(f, 220.0)) < 0.5, "FOLLOW minor %.1f", f);
        seto(e, "Scale", 1, 6);            /* CHROMATIC */
        o = run(e, &v, 1.0, &bad);
        f = f0_of(o, 0.6);
        std::printf("follow chromatic: f0 %.1f Hz (want G#3 = 207.7)\n", f);
        CHECK(std::fabs(semis(f, 207.65)) < 0.5, "FOLLOW chromatic %.1f", f);
        Voice v2(150); v2.syl = 0;         /* a new sung note moves it: 150 Hz -> D3 146.8 (in A minor: D) */
        seto(e, "Scale", 3, 6);
        o = run(e, &v2, 1.0, &bad);
        f = f0_of(o, 0.6);
        std::printf("follow minor 150 Hz: f0 %.1f Hz (want D3 = 146.8)\n", f);
        CHECK(std::fabs(semis(f, 146.83)) < 0.5, "FOLLOW new note %.1f", f);
    }
    /* 7. FREEZE keeps sounding with the input gone; MIX 0 = dry */
    {
        e->dispatcher(e, 2, 0, 0, nullptr, 0);
        Voice v(120); v.syl = 0;
        run(e, &v, 0.6, &bad);
        seto(e, "Freeze", 1, 2);
        std::vector<float> o = run(e, nullptr, 1.0, &bad);
        CHECK(rms(o, 0.5, 1.0) > 0.01, "FREEZE holds (%.4f)", rms(o, 0.5, 1.0));
        seto(e, "Freeze", 0, 2);
        o = run(e, nullptr, 1.0, &bad);
        CHECK(rms(o, 0.6, 1.0) < 1e-4, "unfrozen silence (%.5f)", rms(o, 0.6, 1.0));
        setv(e, "Mix", 0, 0, 100);
        std::vector<float> in;
        Voice v3(120);
        o = run(e, &v3, 0.6, &bad, &in);
        double d = 0;
        for (size_t i = (size_t)(0.3 * SR); i < o.size(); i++) d = std::max(d, (double)std::fabs(o[i] - in[i]));
        CHECK(d < 1e-3, "MIX 0 is the dry input (max diff %.5f)", d);
        setv(e, "Mix", 100, 0, 100);
    }
    /* 8. the robot section and every option at its extreme: finite, bounded */
    {
        setv(e, "Crush", 100, 0, 100); setv(e, "Ring", 100, 0, 100); seto(e, "Gate Sync", 4, 5);
        seto(e, "Bands", 4, 5); setv(e, "Shift", 12, -12, 12); setv(e, "Q", 100, 0, 100); setv(e, "Width", 100, 0, 100);
        seto(e, "Carrier", 3, 6); seto(e, "Chord", 6, 7); setv(e, "In Gain", 24, -12, 24); setv(e, "Output", 12, -12, 12);
        Voice v(120);
        std::vector<float> o = run(e, &v, 1.0, &bad);
        float pk = 0; for (float s : o) pk = std::max(pk, std::fabs(s));
        CHECK(pk <= 1.0001f && rms(o, 0.3, 1.0) > 0.01, "extremes bounded (peak %.3f, rms %.4f)", pk, rms(o, 0.3, 1.0));
        setv(e, "Shift", -12, -12, 12); seto(e, "Bands", 0, 5); setv(e, "Q", 0, 0, 100); seto(e, "Carrier", 5, 6);
        o = run(e, &v, 0.6, &bad);
        CHECK(rms(o, 0.2, 0.6) > 0.005, "other extremes sound (%.4f)", rms(o, 0.2, 0.6));
        seto(e, "Input", 2, 3);   /* RIGHT */
        o = run(e, &v, 0.3, &bad);
    }
    CHECK(!bad, "NaN, Inf or denormal in the output");

    /* 9. chunk into a second instance */
    {
        e->dispatcher(e, 2, 0, 4, nullptr, 0);   /* MACHINE CHOIR */
        setv(e, "Root", 50, 24, 72);
        void *ptr = nullptr;
        const intptr_t len = e->dispatcher(e, 23, 0, 0, &ptr, 0);
        CHECK(len > 10 && ptr, "chunk");
        AEffect *e2 = mainf(master);
        e2->dispatcher(e2, 10, 0, 0, nullptr, 44100.0f);
        CHECK(e2->dispatcher(e2, 24, 0, len, ptr, 0) == 1, "set chunk");
        CHECK(disp(e2, "Root") == "D3", "chunk root '%s'", disp(e2, "Root").c_str());
        CHECK(disp(e2, "Chord") == "MIN7", "chunk chord '%s'", disp(e2, "Chord").c_str());
        CHECK(disp(e2, "Preset") == "05 MACHINE CHOIR", "chunk slot '%s'", disp(e2, "Preset").c_str());
        CHECK(e2->dispatcher(e2, 24, 0, 4, (void *)"junk", 0) == 0, "junk chunk refused");
        e2->dispatcher(e2, 1, 0, 0, nullptr, 0);
    }
    /* 10. SAVE into slot 20, reload it */
    {
        setv(e, "Preset", 20, 1, 32);
        setv(e, "Root", 60, 24, 72);
        e->setParameter(e, pidx(e, "Save"), 1.0f);
        setv(e, "Root", 30, 24, 72);
        e->dispatcher(e, 2, 0, 0, nullptr, 0);
        e->dispatcher(e, 2, 0, 19, nullptr, 0);
        CHECK(disp(e, "Root") == "C4", "saved slot root '%s'", disp(e, "Root").c_str());
        char pn[64] = {0};
        e->dispatcher(e, 29, 19, 0, pn, 0);
        CHECK(!std::strcmp(pn, "USER 20"), "saved slot name '%s'", pn);
        Dl_info di;
        dladdr((void *)mainf, &di);
        std::string dir = di.dli_fname; dir = dir.substr(0, dir.rfind('/')) + "/robovox_user";
        std::remove((dir + "/slot_20.txt").c_str());
        rmdir(dir.c_str());
    }
    e->dispatcher(e, 1, 0, 0, nullptr, 0);
    std::printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}
