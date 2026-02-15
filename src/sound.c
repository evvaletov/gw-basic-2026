#include "sound.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef HAVE_PULSEAUDIO
#include <pulse/simple.h>
#include <pulse/error.h>
#endif

#define SAMPLE_RATE 44100
#define FADE_SAMPLES 220  /* ~5ms at 44100 Hz */

/* MML parser state (persists across PLAY calls, reset by snd_reset) */
static int mml_octave;
static int mml_length;   /* default note length (quarter = 4) */
static int mml_tempo;    /* BPM */
static int mml_artic;    /* articulation: 7 = normal, 8 = legato, 3 = staccato (n/8) */

#ifdef HAVE_PULSEAUDIO
static pa_simple *pa_conn = NULL;

static void pa_open(void)
{
    if (pa_conn)
        return;
    pa_sample_spec ss = {
        .format = PA_SAMPLE_S16LE,
        .rate = SAMPLE_RATE,
        .channels = 1
    };
    pa_conn = pa_simple_new(NULL, "gwbasic", PA_STREAM_PLAYBACK,
                            NULL, "sound", &ss, NULL, NULL, NULL);
}

static void pa_write_samples(int16_t *buf, int count)
{
    if (!pa_conn)
        return;
    pa_simple_write(pa_conn, buf, count * sizeof(int16_t), NULL);
}

static void pa_drain(void)
{
    if (pa_conn)
        pa_simple_drain(pa_conn, NULL);
}
#endif /* HAVE_PULSEAUDIO */

void snd_init(void)
{
    snd_reset();
}

void snd_shutdown(void)
{
#ifdef HAVE_PULSEAUDIO
    if (pa_conn) {
        pa_simple_drain(pa_conn, NULL);
        pa_simple_free(pa_conn);
        pa_conn = NULL;
    }
#endif
}

void snd_reset(void)
{
    mml_octave = 4;
    mml_length = 4;
    mml_tempo = 120;
    mml_artic = 7;
}

void snd_tone(int freq_hz, int duration_ticks)
{
    if (duration_ticks <= 0)
        return;

#ifdef HAVE_PULSEAUDIO
    pa_open();
    if (!pa_conn)
        return;

    double seconds = duration_ticks / 18.2;
    int total = (int)(seconds * SAMPLE_RATE);
    if (total <= 0)
        return;

    /* Allocate buffer (or use stack for short tones) */
    int16_t stack_buf[8192];
    int16_t *buf = (total <= 8192) ? stack_buf : malloc(total * sizeof(int16_t));
    if (!buf)
        return;

    if (freq_hz < 37) {
        /* Rest / silence */
        memset(buf, 0, total * sizeof(int16_t));
    } else {
        double phase_inc = 2.0 * M_PI * freq_hz / SAMPLE_RATE;
        int fade = (FADE_SAMPLES < total / 2) ? FADE_SAMPLES : total / 2;
        for (int i = 0; i < total; i++) {
            double sample = sin(phase_inc * i) * 24000.0;
            /* Fade in */
            if (i < fade)
                sample *= (double)i / fade;
            /* Fade out */
            if (i >= total - fade)
                sample *= (double)(total - 1 - i) / fade;
            buf[i] = (int16_t)sample;
        }
    }

    pa_write_samples(buf, total);

    if (buf != stack_buf)
        free(buf);
#else
    (void)freq_hz;
    (void)duration_ticks;
#endif
}

void snd_tone_sync(int freq_hz, int duration_ticks)
{
    snd_tone(freq_hz, duration_ticks);
#ifdef HAVE_PULSEAUDIO
    pa_drain();
#endif
}

void snd_beep(void)
{
    snd_tone_sync(800, 4);  /* ~220ms beep at 800 Hz */
}

/* --- MML Parser --- */

static const int note_semitones[] = {
    /* A  B  C  D  E  F  G */
       9, 11, 0, 2, 4, 5, 7
};

static double note_freq(int octave, int semitone)
{
    /* A4 = 440 Hz, semitone 9 in octave 4 */
    double n = (octave - 4) * 12.0 + (semitone - 9);
    return 440.0 * pow(2.0, n / 12.0);
}

static int parse_int(const char **p)
{
    int val = 0;
    while (isdigit((unsigned char)**p)) {
        val = val * 10 + (**p - '0');
        (*p)++;
    }
    return val;
}

void snd_play(const char *mml)
{
    if (!mml)
        return;

    const char *p = mml;
    while (*p) {
        while (*p == ' ')
            p++;
        if (!*p)
            break;

        char ch = toupper((unsigned char)*p);
        p++;

        /* Notes A-G */
        if (ch >= 'A' && ch <= 'G') {
            int semi = note_semitones[ch - 'A'];
            int oct = mml_octave;

            /* Accidentals */
            if (*p == '#' || *p == '+') {
                semi++;
                p++;
            } else if (*p == '-') {
                semi--;
                p++;
            }

            /* Wrap semitone into octave */
            if (semi < 0) { semi += 12; oct--; }
            if (semi >= 12) { semi -= 12; oct++; }

            /* Optional length */
            int len = mml_length;
            if (isdigit((unsigned char)*p))
                len = parse_int(&p);

            /* Dotted note */
            int dots = 0;
            while (*p == '.') { dots++; p++; }

            /* Duration in ticks: whole note = 4 beats, at tempo BPM */
            /* ticks = (whole_note_seconds / len) * 18.2 */
            double whole = 240.0 / mml_tempo;  /* seconds for whole note */
            double dur = whole / len;
            /* Each dot adds half the previous duration */
            double extra = dur / 2.0;
            for (int d = 0; d < dots; d++) {
                dur += extra;
                extra /= 2.0;
            }

            double ticks = dur * 18.2;
            double play_ticks = ticks * mml_artic / 8.0;
            double rest_ticks = ticks - play_ticks;

            int freq = (int)(note_freq(oct, semi) + 0.5);
            snd_tone(freq, (int)(play_ticks + 0.5));
            if (rest_ticks > 0.5)
                snd_tone(0, (int)(rest_ticks + 0.5));
            continue;
        }

        /* N-command: play note by number 0-84 */
        if (ch == 'N') {
            int n = parse_int(&p);
            if (n == 0) {
                /* Rest: one quarter note */
                double whole = 240.0 / mml_tempo;
                snd_tone(0, (int)(whole / mml_length * 18.2 + 0.5));
            } else if (n >= 1 && n <= 84) {
                int oct = (n - 1) / 12;
                int semi = (n - 1) % 12;
                int freq = (int)(note_freq(oct, semi) + 0.5);
                double whole = 240.0 / mml_tempo;
                double dur = whole / mml_length * 18.2;
                double play = dur * mml_artic / 8.0;
                double rest = dur - play;
                snd_tone(freq, (int)(play + 0.5));
                if (rest > 0.5)
                    snd_tone(0, (int)(rest + 0.5));
            }
            continue;
        }

        /* Octave */
        if (ch == 'O') {
            int o = parse_int(&p);
            if (o >= 0 && o <= 6)
                mml_octave = o;
            continue;
        }

        /* Relative octave */
        if (ch == '>') { if (mml_octave < 6) mml_octave++; continue; }
        if (ch == '<') { if (mml_octave > 0) mml_octave--; continue; }

        /* Default length */
        if (ch == 'L') {
            int l = parse_int(&p);
            if (l >= 1 && l <= 64)
                mml_length = l;
            continue;
        }

        /* Tempo */
        if (ch == 'T') {
            int t = parse_int(&p);
            if (t >= 32 && t <= 255)
                mml_tempo = t;
            continue;
        }

        /* Rest / Pause */
        if (ch == 'P' || ch == 'R') {
            int len = mml_length;
            if (isdigit((unsigned char)*p))
                len = parse_int(&p);
            int dots = 0;
            while (*p == '.') { dots++; p++; }

            double whole = 240.0 / mml_tempo;
            double dur = whole / len;
            double extra = dur / 2.0;
            for (int d = 0; d < dots; d++) {
                dur += extra;
                extra /= 2.0;
            }
            snd_tone(0, (int)(dur * 18.2 + 0.5));
            continue;
        }

        /* Articulation */
        if (ch == 'M') {
            char mode = toupper((unsigned char)*p);
            if (mode == 'N') { mml_artic = 7; p++; }
            else if (mode == 'L') { mml_artic = 8; p++; }
            else if (mode == 'S') { mml_artic = 3; p++; }
            else if (mode == 'F') { p++; }  /* foreground, already default */
            else if (mode == 'B') { p++; }  /* background, accepted but ignored */
            continue;
        }

        /* Semicolons and other separators: skip */
    }

#ifdef HAVE_PULSEAUDIO
    pa_drain();
#endif
}
