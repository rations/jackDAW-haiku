#include <glib.h>

#include "timeruler.h"

/* -----------------------------------------------------------------------
 * Time formatting
 * ----------------------------------------------------------------------- */

gchar *format_timecode(guint32 samplerate, off_t samples, off_t samplemax, gchar *timebuf,
                       gint mode)
{
    static gchar internal[64];
    if (!timebuf)
        timebuf = internal;
    if (samplerate == 0)
        samplerate = 1;
    if (samplemax <= 0)
        samplemax = samples;
    if (mode < 0 || mode > TIMEMODE_30FPS)
        mode = TIMEMODE_REAL;

    /* Raw sample index. */
    if (mode == TIMEMODE_SAMPLES) {
        g_snprintf(timebuf, 64, "%05lld", (long long)samples);
        return timebuf;
    }

    double secs = (double)samples / (double)samplerate;
    if (secs < 0.0)
        secs = 0.0;

    /* Wall-clock display (tenths for REAL, milliseconds for REALLONG). The
     * hours field is only shown once the project is long enough to need it. */
    if (mode == TIMEMODE_REAL || mode == TIMEMODE_REALLONG) {
        guint total_ms = (guint)(secs * 1000.0 + 0.5);
        guint h = total_ms / 3600000u;
        guint m = (total_ms / 60000u) % 60u;
        guint s = (total_ms / 1000u) % 60u;
        guint ms = total_ms % 1000u;
        gboolean show_h = samplemax / ((off_t)samplerate * 3600) > 0;
        char head[24];
        if (show_h)
            g_snprintf(head, sizeof head, "%u:%02u:%02u", h, m, s);
        else
            g_snprintf(head, sizeof head, "%02u:%02u", m, s);
        /* Drop the fraction on whole-second ticks so the ruler isn't a wall
         * of ".000"; tenths for REAL, milliseconds for REALLONG otherwise. */
        if (ms == 0)
            g_snprintf(timebuf, 64, "%s", head);
        else if (mode == TIMEMODE_REAL)
            g_snprintf(timebuf, 64, "%s.%u", head, ms / 100);
        else
            g_snprintf(timebuf, 64, "%s.%03u", head, ms);
        return timebuf;
    }

    /* Frame-based timecode: HH:MM:SS[FF]. NTSC counts at 29.97 fps without
     * SMPTE drop-frame compensation (positions only, not broadcast TC). */
    int fps;
    double rate;
    switch (mode) {
        case TIMEMODE_24FPS:
            fps = 24;
            rate = 24.0;
            break;
        case TIMEMODE_25FPS:
            fps = 25;
            rate = 25.0;
            break;
        case TIMEMODE_NTSC:
            fps = 30;
            rate = 30000.0 / 1001.0;
            break;
        default:
            fps = 30;
            rate = 30.0;
            break; /* TIMEMODE_30FPS */
    }
    guint64 frames = (guint64)(secs * rate + 1e-6);
    guint ff = (guint)(frames % (guint64)fps);
    guint64 tsecs = frames / (guint64)fps;
    guint s = (guint)(tsecs % 60u);
    guint m = (guint)((tsecs / 60u) % 60u);
    guint h = (guint)(tsecs / 3600u);
    g_snprintf(timebuf, 64, "%02u:%02u:%02u[%02u]", h, m, s, ff);
    return timebuf;
}

/* -----------------------------------------------------------------------
 * Timescale point generation for the ruler
 * ----------------------------------------------------------------------- */

/* Candidate tick spacings in seconds, ascending: a 1-2-5 decade ladder
 * extended with the clock-natural 15/30 steps, so labels land on round
 * values as the user zooms. */
static const double tick_secs[] = {1e-3, 2e-3, 5e-3, 1e-2, 2e-2, 5e-2,  0.1,   0.2,  0.5,
                                   1,    2,    5,    10,   15,   30,    60,    120,  300,
                                   600,  900,  1800, 3600, 7200, 18000, 36000, 86400};

static off_t secs_to_samp(double s, guint32 sr)
{
    off_t v = (off_t)(s * (double)sr + 0.5);
    return v < 1 ? 1 : v;
}

/* Smallest of {1,2,5}x10^n that is >= raw (in samples). */
static off_t nice_samples(off_t raw)
{
    if (raw < 1)
        raw = 1;
    off_t p = 1;
    while (p < raw) {
        if (2 * p >= raw)
            return 2 * p;
        if (5 * p >= raw)
            return 5 * p;
        if (10 * p >= raw)
            return 10 * p;
        p *= 10;
    }
    return p;
}

/* Write each multiple of `step` lying in [lo,hi] into out[], up to `cap`
 * entries. Returns the count written. */
static int fill_ticks(off_t lo, off_t hi, off_t step, off_t *out, int cap)
{
    if (step < 1)
        step = 1;
    off_t first = (lo / step) * step;
    if (first < lo)
        first += step;
    int n = 0;
    for (off_t s = first; s <= hi && n < cap; s += step)
        out[n++] = s;
    return n;
}

/* Index of the coarsest tick_secs entry whose tick count fits `budget`. */
static int pick_secs_level(off_t span, guint32 sr, int budget)
{
    if (budget < 1)
        budget = 1;
    for (guint i = 0; i < ARRAY_LENGTH(tick_secs); i++)
        if (span / secs_to_samp(tick_secs[i], sr) <= (off_t)budget)
            return (int)i;
    return (int)ARRAY_LENGTH(tick_secs) - 1;
}

guint ruler_tick_positions(guint32 samplerate, off_t start_samp, off_t end_samp, off_t *points,
                           int *npoints, off_t *midpoints, int *nmidpoints, off_t *minor_points,
                           int *nminorpoints, int timemode)
{
    int cap_major = *npoints, cap_mid = *nmidpoints, cap_minor = *nminorpoints;
    *npoints = *nmidpoints = *nminorpoints = 0;

    if (samplerate == 0)
        samplerate = 1;
    off_t span = end_samp - start_samp;
    if (span < 1)
        span = 1;

    /* Raw sample positions step directly in decades. */
    if (timemode == TIMEMODE_SAMPLES) {
        off_t major = nice_samples(span / (cap_major > 1 ? cap_major - 1 : 1));
        off_t minor = nice_samples(span / (cap_minor > 1 ? cap_minor - 1 : 1));
        *npoints = fill_ticks(start_samp, end_samp, major, points, cap_major);
        *nminorpoints = fill_ticks(start_samp, end_samp, minor, minor_points, cap_minor);
        return 0;
    }

    /* Time / timecode modes choose round spacings in seconds. */
    int lv_major = pick_secs_level(span, samplerate, cap_major - 2);
    *npoints = fill_ticks(start_samp, end_samp, secs_to_samp(tick_secs[lv_major], samplerate),
                          points, cap_major);

    /* Descend to the finest level that still fits the minor-tick budget. */
    int lv_minor = lv_major;
    while (lv_minor > 0 &&
           span / secs_to_samp(tick_secs[lv_minor - 1], samplerate) <= (off_t)(cap_minor - 2))
        lv_minor--;
    if (lv_minor >= lv_major)
        return 0; /* too zoomed-out for sub-ticks */

    *nminorpoints = fill_ticks(start_samp, end_samp, secs_to_samp(tick_secs[lv_minor], samplerate),
                               minor_points, cap_minor);

    /* A mid level sits one rung above the minor ticks, when distinct. */
    int lv_mid = lv_minor + 1;
    if (lv_mid >= lv_major)
        return 0;
    *nmidpoints = fill_ticks(start_samp, end_samp, secs_to_samp(tick_secs[lv_mid], samplerate),
                             midpoints, cap_mid);
    return 1;
}
