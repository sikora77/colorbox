/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2026 Rockbox contributors
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#include "config.h"

#if defined(HAVE_ALBUMART) && defined(HAVE_LCD_COLOR)

#include <string.h>
#include "lcd.h"
#include "settings.h"
#include "kernel.h"
#include "audio.h"
#include "misc.h"
#include "playback.h"
#include "buffering.h"
#include "appevents.h"
#include "skin_albumart_color.h"

#define AA_FADE_DURATION (HZ / 4) /* 250ms */
#define HISTOGRAM_BUCKETS 4096
#define SAMPLE_STRIDE 4         /* sample every 4th pixel */
#define MIN_CONTRAST 100        /* minimum luminance contrast 0-255 */
#define SATURATION_BASE 8       /* base score for unsaturated colors */
#define NO_ART_TIMEOUT HZ       /* 1s timeout before concluding no art */

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

struct dynamic_colors_cache
{
    unsigned int dominant; /* target bg color */
    unsigned int accent;   /* target fg color */
    unsigned int highlight;
    unsigned int separator;
    unsigned int prev_dominant; /* fade-start bg */
    unsigned int prev_accent;   /* fade-start fg */
    unsigned int prev_highlight;
    unsigned int prev_separator;
    unsigned int theme_fg;  /* saved original theme fg */
    unsigned int theme_bg;  /* saved original theme bg */
    unsigned int theme_lss; /* saved selector start color */
    unsigned int theme_lse; /* saved selector end color */
    unsigned int theme_lst; /* saved selector text color */
    unsigned int theme_sep; /* saved list separator color */
    long fade_start_tick;
    long track_change_tick;  /* when TRACK_CHANGE fired */
    bool valid;              /* have valid AA colors */
    bool fading;             /* fade in progress */
    bool fading_out;         /* fading to defaults after disable/stop */
    bool was_enabled;        /* track setting state for toggle detection */
    bool needs_full_update;  /* set when fade completes for full redraw */
    bool needs_screen_clear; /* set when fade completes to clear bg gaps */
};

static struct dynamic_colors_cache cache;
static volatile bool needs_extraction;
static uint16_t histogram[HISTOGRAM_BUCKETS];
static bool is_default_theme = false;

static int hue_to_rgb(int p, int q, int t)
{
    if (t < 0)
        t += 360;
    if (t >= 360)
        t -= 360;
    if (t < 60)
        return p + ((q - p) * t) / 60;
    if (t < 180)
        return q;
    if (t < 240)
        return p + ((q - p) * (240 - t)) / 60;
    return p;
}

static void rgb_to_hsl(int r, int g, int b, int *h, int *s, int *l)
{
    int cmax = MAX(r, MAX(g, b));
    int cmin = MIN(r, MIN(g, b));
    int delta = cmax - cmin;

    *l = (cmax + cmin) / 2;

    if (delta == 0)
    {
        *h = 0;
        *s = 0;
    }
    else
    {
        if (*l < 128)
            *s = (delta * 255) / (cmax + cmin);
        else
            *s = (delta * 255) / (510 - cmax - cmin);

        if (cmax == r)
            *h = ((g - b) * 60) / delta;
        else if (cmax == g)
            *h = ((b - r) * 60) / delta + 120;
        else
            *h = ((r - g) * 60) / delta + 240;

        if (*h < 0)
            *h += 360;
    }
}

static void hsl_to_rgb(int h, int s, int l, int *r, int *g, int *b)
{
    if (s == 0)
    {
        *r = *g = *b = l;
    }
    else
    {
        int q = l < 128 ? (l * (255 + s)) / 255 : (l + s - (l * s) / 255);
        int p = 2 * l - q;
        *r = hue_to_rgb(p, q, h + 120);
        *g = hue_to_rgb(p, q, h);
        *b = hue_to_rgb(p, q, h - 120);
    }
}

static int compute_luminance(int r8, int g8, int b8)
{
    return (r8 * 77 + g8 * 150 + b8 * 29) >> 8;
}

static unsigned int lerp_color(unsigned int c1, unsigned int c2, int t)
{
    /* t: 0..256, 0 = fully c1, 256 = fully c2 */
    int r1 = RGB_UNPACK_RED(c1);
    int g1 = RGB_UNPACK_GREEN(c1);
    int b1 = RGB_UNPACK_BLUE(c1);
    int r2 = RGB_UNPACK_RED(c2);
    int g2 = RGB_UNPACK_GREEN(c2);
    int b2 = RGB_UNPACK_BLUE(c2);

    int r = r1 + (((r2 - r1) * t) >> 8);
    int g = g1 + (((g2 - g1) * t) >> 8);
    int b = b1 + (((b2 - b1) * t) >> 8);

    return LCD_RGBPACK(r, g, b);
}

static int fade_progress(void)
{
    long elapsed = current_tick - cache.fade_start_tick;
    if (elapsed <= 0)
        return 0;
    if (elapsed >= AA_FADE_DURATION)
        return 256;
    return (int)((elapsed * 256) / AA_FADE_DURATION);
}

static void start_fade(unsigned int new_accent, unsigned int new_dominant,
                       unsigned int new_highlight, unsigned int new_separator,
                       bool to_defaults)
{
    /* Capture current effective colors as fade start */
    if (cache.fading || cache.fading_out)
    {
        int p = fade_progress();
        if (cache.fading_out)
        {
            cache.prev_accent = lerp_color(cache.prev_accent, cache.theme_fg, p);
            cache.prev_dominant = lerp_color(cache.prev_dominant, cache.theme_bg, p);
            cache.prev_highlight = lerp_color(cache.prev_highlight, cache.theme_lss, p);
            cache.prev_separator = lerp_color(cache.prev_separator, cache.theme_sep, p);
        }
        else
        {
            cache.prev_accent = lerp_color(cache.prev_accent, cache.accent, p);
            cache.prev_dominant = lerp_color(cache.prev_dominant, cache.dominant, p);
            cache.prev_highlight = lerp_color(cache.prev_highlight, cache.highlight, p);
            cache.prev_separator = lerp_color(cache.prev_separator, cache.separator, p);
        }
    }
    else if (cache.valid)
    {
        cache.prev_accent = cache.accent;
        cache.prev_dominant = cache.dominant;
        cache.prev_highlight = cache.highlight;
        cache.prev_separator = cache.separator;
    }
    else
    {
        cache.prev_accent = cache.theme_fg;
        cache.prev_dominant = cache.theme_bg;
        cache.prev_highlight = cache.theme_lss;
        cache.prev_separator = cache.theme_sep;
    }

    cache.accent = new_accent;
    cache.dominant = new_dominant;
    cache.highlight = new_highlight;
    cache.separator = new_separator;
    cache.fade_start_tick = current_tick;
    cache.fading = !to_defaults;
    cache.fading_out = to_defaults;
    cache.valid = true;
}

static void extract_colors(const struct bitmap *bmp)
{
    if (!bmp->data || bmp->width <= 0 || bmp->height <= 0)
        return;

    fb_data *pixels = (fb_data *)bmp->data;
    int width = bmp->width;
    int height = bmp->height;
    int total_pixels = width * height;
    int i;

    memset(histogram, 0, sizeof(histogram));

    /* Pass 1: build quantized histogram */
    for (i = 0; i < total_pixels; i += SAMPLE_STRIDE)
    {
        fb_data px = pixels[i];
        int r8 = FB_UNPACK_RED(px);
        int g8 = FB_UNPACK_GREEN(px);
        int b8 = FB_UNPACK_BLUE(px);
        int r4 = r8 >> 4;
        int g4 = g8 >> 4;
        int b4 = b8 >> 4;
        int bucket = (r4 << 8) | (g4 << 4) | b4;
        if (histogram[bucket] < UINT16_MAX)
            histogram[bucket]++;
    }

/* Helper: get sum of pixels in a 3x3x3 radius around a bucket */
#define GET_CLUSTER_COUNT(r, g, b)                                                            \
    ({                                                                                        \
        int _sum = 0;                                                                         \
        for (int _dr = -1; _dr <= 1; _dr++)                                                   \
        {                                                                                     \
            for (int _dg = -1; _dg <= 1; _dg++)                                               \
            {                                                                                 \
                for (int _db = -1; _db <= 1; _db++)                                           \
                {                                                                             \
                    int _nr = (r) + _dr;                                                      \
                    int _ng = (g) + _dg;                                                      \
                    int _nb = (b) + _db;                                                      \
                    if (_nr >= 0 && _nr < 16 && _ng >= 0 && _ng < 16 && _nb >= 0 && _nb < 16) \
                    {                                                                         \
                        _sum += histogram[(_nr << 8) | (_ng << 4) | _nb];                     \
                    }                                                                         \
                }                                                                             \
            }                                                                                 \
        }                                                                                     \
        _sum;                                                                                 \
    })

    /* Find dominant bucket (skip near-black/near-white, prefer saturated) */
    int best_bucket = -1;
    unsigned int best_score = 0;
    int fallback_bucket = -1;
    uint16_t fallback_count = 0;

    for (i = 0; i < HISTOGRAM_BUCKETS; i++)
    {
        if (histogram[i] == 0)
            continue;

        int r4 = (i >> 8) & 0xF;
        int g4 = (i >> 4) & 0xF;
        int b4 = i & 0xF;
        int cluster_count = GET_CLUSTER_COUNT(r4, g4, b4);

        /* Track unfiltered best as fallback */
        if (cluster_count > fallback_count)
        {
            fallback_count = cluster_count;
            fallback_bucket = i;
        }

        /* No filters, pure black or white covers should use black or white backgrounds */

        /* Score by count weighted by saturation (vibrant colors preferred) */
        int max_c = MAX(MAX(r4, g4), b4);
        int min_c = MIN(MIN(r4, g4), b4);
        int sat = max_c > 0 ? ((max_c - min_c) * 15) / max_c : 0;
        unsigned int score = (unsigned int)cluster_count * (sat + SATURATION_BASE) / SATURATION_BASE;

        if (score > best_score)
        {
            best_score = score;
            best_bucket = i;
        }
    }

    if (best_bucket < 0)
        best_bucket = fallback_bucket;
    if (best_bucket < 0)
        return; /* empty image? */

    /* Pass 2: average full-precision RGB for dominant bucket */
    long sum_r = 0, sum_g = 0, sum_b = 0;
    int count = 0;

    for (i = 0; i < total_pixels; i += SAMPLE_STRIDE)
    {
        fb_data px = pixels[i];
        int r8 = FB_UNPACK_RED(px);
        int g8 = FB_UNPACK_GREEN(px);
        int b8 = FB_UNPACK_BLUE(px);
        int r4 = r8 >> 4;
        int g4 = g8 >> 4;
        int b4 = b8 >> 4;
        int bucket = (r4 << 8) | (g4 << 4) | b4;

        if (bucket == best_bucket)
        {
            sum_r += r8;
            sum_g += g8;
            sum_b += b8;
            count++;
        }
    }

    int dom_r = count ? (int)(sum_r / count) : 0;
    int dom_g = count ? (int)(sum_g / count) : 0;
    int dom_b = count ? (int)(sum_b / count) : 0;
    unsigned int dominant = LCD_RGBPACK(dom_r, dom_g, dom_b);
    int dom_lum = compute_luminance(dom_r, dom_g, dom_b);

    /* Find accent: best-scored bucket with sufficient contrast */
    int accent_bucket = -1;
    unsigned int accent_score = 0;

    for (i = 0; i < HISTOGRAM_BUCKETS; i++)
    {
        if (histogram[i] == 0 || i == best_bucket)
            continue;

        int r4 = (i >> 8) & 0xF;
        int g4 = (i >> 4) & 0xF;
        int b4 = i & 0xF;
        int cluster_count = GET_CLUSTER_COUNT(r4, g4, b4);

        int r8 = (r4 << 4) | r4;
        int g8 = (g4 << 4) | g4;
        int b8 = (b4 << 4) | b4;
        int lum = compute_luminance(r8, g8, b8);

        int contrast = dom_lum > lum ? dom_lum - lum : lum - dom_lum;

        /* We use the user's relaxed contrast setting as the baseline, relying on our
         * euclidean distance multiplier to pick vibrant, visually distinct colors. */
        int required_contrast = global_settings.dynamic_colors_relaxed_contrast;

        if (contrast >= required_contrast)
        {
            /* Calculate distance early since we use it for scoring */
            int dr = r8 - dom_r;
            int dg = g8 - dom_g;
            int db = b8 - dom_b;
            long dist_sq = (long)dr * dr + (long)dg * dg + (long)db * db;

            /* Exponentially reward saturation so vibrant colors obliterate greys */
            int max_c = MAX(MAX(r4, g4), b4);
            int min_c = MIN(MIN(r4, g4), b4);
            int sat = max_c > 0 ? ((max_c - min_c) * 15) / max_c : 0;
            unsigned int sat_weight = (sat * sat * sat) + 1;
            unsigned int base_score = (unsigned int)cluster_count * sat_weight;
            
            /* Boost colors that are visually very distinct from the background */
            unsigned int score = (unsigned int)((long)base_score * (dist_sq + 1000) / 1000);

            if (score > accent_score)
            {
                accent_score = score;
                accent_bucket = i;
            }
        }
    }

    int acc_r, acc_g, acc_b;
    unsigned int accent;
    if (accent_bucket >= 0)
    {
        /* Average full-precision RGB for accent bucket */
        sum_r = sum_g = sum_b = 0;
        count = 0;
        for (i = 0; i < total_pixels; i += SAMPLE_STRIDE)
        {
            fb_data px = pixels[i];
            int r8 = FB_UNPACK_RED(px);
            int g8 = FB_UNPACK_GREEN(px);
            int b8 = FB_UNPACK_BLUE(px);
            int r4 = r8 >> 4;
            int g4 = g8 >> 4;
            int b4 = b8 >> 4;
            int bucket = (r4 << 8) | (g4 << 4) | b4;

            if (bucket == accent_bucket)
            {
                sum_r += r8;
                sum_g += g8;
                sum_b += b8;
                count++;
            }
        }
        acc_r = count ? (int)(sum_r / count) : 0;
        acc_g = count ? (int)(sum_g / count) : 0;
        acc_b = count ? (int)(sum_b / count) : 0;
    }
    else
    {
        /* Fallback: use the theme's intended foreground color */
        acc_r = RGB_UNPACK_RED(cache.theme_fg);
        acc_g = RGB_UNPACK_GREEN(cache.theme_fg);
        acc_b = RGB_UNPACK_BLUE(cache.theme_fg);
    }

    /* Readability enforcement using HSL */
    int h, s, l;
    rgb_to_hsl(acc_r, acc_g, acc_b, &h, &s, &l);

    int acc_lum = compute_luminance(acc_r, acc_g, acc_b);
    int contrast = dom_lum > acc_lum ? dom_lum - acc_lum : acc_lum - dom_lum;

    /* Prefer making text brighter, but only if it can actually reach MIN_CONTRAST */
    bool push_up;
    if (255 - dom_lum >= MIN_CONTRAST && dom_lum < 180)
    {
        push_up = true;
    }
    else if (dom_lum >= MIN_CONTRAST)
    {
        push_up = false;
    }
    else
    {
        push_up = (255 - dom_lum) > dom_lum;
    }

    /* Default initialization in case we don't need to push it */
    accent = LCD_RGBPACK(acc_r, acc_g, acc_b);

    /* Determine the target contrast. If we naturally found a prominent color
     * in the album art that passed the relaxed contrast check, we don't want
     * to artificially push it all the way to MIN_CONTRAST. */
    int target_contrast = (accent_bucket >= 0) ? global_settings.dynamic_colors_relaxed_contrast : MIN_CONTRAST;

    if (contrast < target_contrast)
    {
        while (contrast < target_contrast)
        {
            if (push_up)
            {
                l += 10;
                /* Cap at 230 to prevent pure white, keeping it pastel/colorful */
                if (l > 230)
                {
                    l = 230;
                    break;
                }
            }
            else
            {
                l -= 10;
                /* Cap at 25 to prevent pure black, keeping it dark rich */
                if (l < 25)
                {
                    l = 25;
                    break;
                }
            }
            hsl_to_rgb(h, s, l, &acc_r, &acc_g, &acc_b);
            acc_lum = compute_luminance(acc_r, acc_g, acc_b);
            contrast = dom_lum > acc_lum ? dom_lum - acc_lum : acc_lum - dom_lum;
        }

        /* Force minimum saturation so it stays colorful even if pushed far */
        if (s < 64)
            s = 64;
        hsl_to_rgb(h, s, l, &acc_r, &acc_g, &acc_b);
        accent = LCD_RGBPACK(acc_r, acc_g, acc_b);
    }

    /* Generate highlight and separator from dominant */
    rgb_to_hsl(dom_r, dom_g, dom_b, &h, &s, &l);
    int hl_r, hl_g, hl_b;
    unsigned int highlight, separator;
    if (dom_lum < 180)
    {
        hsl_to_rgb(h, s, MIN(l + 35, 255), &hl_r, &hl_g, &hl_b);
        highlight = LCD_RGBPACK(hl_r, hl_g, hl_b);
        hsl_to_rgb(h, s, MIN(l + 15, 255), &hl_r, &hl_g, &hl_b);
        separator = LCD_RGBPACK(hl_r, hl_g, hl_b);
    }
    else
    {
        hsl_to_rgb(h, s, MAX(l - 35, 0), &hl_r, &hl_g, &hl_b);
        highlight = LCD_RGBPACK(hl_r, hl_g, hl_b);
        hsl_to_rgb(h, s, MAX(l - 15, 0), &hl_r, &hl_g, &hl_b);
        separator = LCD_RGBPACK(hl_r, hl_g, hl_b);
    }

    start_fade(accent, dominant, highlight, separator, false);
    is_default_theme = false;
}

static void track_change_cb(unsigned short id, void *param)
{
    (void)param;
    needs_extraction = true;
    if (id == PLAYBACK_EVENT_TRACK_CHANGE)
        cache.track_change_tick = current_tick;
}

static void save_all_theme_colors(void)
{
    cache.theme_fg = global_settings.fg_color;
    cache.theme_bg = global_settings.bg_color;
    cache.theme_lss = global_settings.lss_color;
    cache.theme_lse = global_settings.lse_color;
    cache.theme_lst = global_settings.lst_color;
    cache.theme_sep = global_settings.list_separator_color;
}

void dynamic_colors_init(void)
{
    static bool events_registered = false;

    memset(&cache, 0, sizeof(cache));
    save_all_theme_colors();
    cache.was_enabled = global_settings.dynamic_colors;
    needs_extraction = false;

    if (!events_registered)
    {
        add_event(PLAYBACK_EVENT_TRACK_CHANGE, track_change_cb);
        add_event(PLAYBACK_EVENT_CUR_TRACK_READY, track_change_cb);
        events_registered = true;
    }
}

void dynamic_colors_save_theme(void)
{
    save_all_theme_colors();
    /* Invalidate cached colors — they were for the old theme */
    cache.valid = false;
    cache.fading = false;
    cache.fading_out = false;
    cache.needs_screen_clear = true;
    needs_extraction = true;
}

static void apply_default_stylish_theme(void)
{
    if (is_default_theme && cache.valid && !cache.fading_out)
        return;

    unsigned int def_dom, def_acc, def_hl, def_sep;

    /* Use the global theme background and foreground as the default */
    def_dom = cache.theme_bg;
    def_acc = cache.theme_fg;

    /* Calculate highlight and separator by blending background and foreground */
    def_hl = lerp_color(def_dom, def_acc, 64);   /* approx 25% foreground */
    def_sep = lerp_color(def_dom, def_acc, 128); /* approx 50% foreground */

    if (!cache.valid)
    {
        /* First time — set directly with no fade so the very first
         * render shows the styled defaults, not raw black/white. */
        cache.accent = def_acc;
        cache.dominant = def_dom;
        cache.highlight = def_hl;
        cache.separator = def_sep;
        cache.prev_accent = def_acc;
        cache.prev_dominant = def_dom;
        cache.prev_highlight = def_hl;
        cache.prev_separator = def_sep;
        cache.valid = true;
        cache.fading = false;
        cache.fading_out = false;
    }
    else
    {
        start_fade(def_acc, def_dom, def_hl, def_sep, false);
    }
    is_default_theme = true;
}

void dynamic_colors_check_extraction(int aa_slot)
{
    /* Remember valid AA slots from skin_render calls so list_draw
     * can trigger extraction with aa_slot = -1 (use last known) */
    static int last_aa_slot = -1;
    if (aa_slot >= 0)
        last_aa_slot = aa_slot;
    else
        aa_slot = last_aa_slot;

    /* Detect playback stop — initiate fade to defaults */
    if (global_settings.dynamic_colors && !(audio_status() & AUDIO_STATUS_PLAY))
    {
        apply_default_stylish_theme();
    }

    /* Detect setting toggle */
    bool enabled = global_settings.dynamic_colors;
    if (!enabled && cache.was_enabled && cache.valid && !cache.fading_out)
    {
        /* Setting just turned off — start fade to defaults */
        start_fade(cache.theme_fg, cache.theme_bg, cache.theme_lss, cache.theme_sep, true);
    }
    if (enabled && !cache.was_enabled)
    {
        /* Setting just turned on — try extraction */
        needs_extraction = true;
    }
    cache.was_enabled = enabled;

    if (!needs_extraction)
        return;
    if (!enabled)
    {
        needs_extraction = false;
        return;
    }
    if (aa_slot < 0)
    {
        /* No known AA slot yet — can't extract */
        return;
    }

    int handle = playback_current_aa_hid(aa_slot);
    if (handle >= 0)
    {
        struct bitmap *bmp;
        if (bufgetdata(handle, 0, (void *)&bmp) > 0)
            extract_colors(bmp);
        needs_extraction = false;
    }
    else
    {
        /* Art not available yet — check timeout */
        long elapsed = current_tick - cache.track_change_tick;
        if (elapsed > NO_ART_TIMEOUT)
        {
            /* No art for this track — fade to defaults */
            apply_default_stylish_theme();
            needs_extraction = false;
        }
        /* else: keep trying on next render */
    }
}

/* Map a color to its dynamic equivalent using pre-computed effective colors.
 * Matches against all 6 saved theme colors with role-based fallbacks. */
static unsigned int resolve_mapped(unsigned int original,
                                   unsigned int eff_accent,
                                   unsigned int eff_dominant,
                                   unsigned int eff_highlight,
                                   unsigned int eff_separator)
{
    /* Background always maps to dominant */
    if (original == cache.theme_bg)
        return eff_dominant;

    /* Selector bar */
    if (original == cache.theme_lss)
        return eff_highlight;

    /* Selector text */
    if (original == cache.theme_lst)
        return eff_dominant;

    /* Selector gradient end */
    if (original == cache.theme_lse)
        return lerp_color(eff_highlight, eff_dominant, 64);

    /* List separator */
    if (original == cache.theme_sep)
        return eff_separator;

    /* Enforce strict two-tone: ALL other foreground colors (including
     * hardcoded skin colors) become the dynamic accent color. */
    return eff_accent;
}

unsigned int dynamic_colors_resolve(unsigned int original)
{
    /* Fade-out continues even after setting is toggled off */
    if (cache.fading_out)
    {
        int p = fade_progress();
        if (p >= 256)
        {
            cache.fading_out = false;
            cache.valid = false;
            cache.needs_full_update = true;
            cache.needs_screen_clear = true;
            return original;
        }
        return resolve_mapped(original,
                              lerp_color(cache.prev_accent, cache.theme_fg, p),
                              lerp_color(cache.prev_dominant, cache.theme_bg, p),
                              lerp_color(cache.prev_highlight, cache.theme_lss, p),
                              lerp_color(cache.prev_separator, cache.theme_sep, p));
    }

    if (!global_settings.dynamic_colors || !cache.valid)
        return original;

    if (cache.fading)
    {
        int p = fade_progress();
        if (p >= 256)
        {
            cache.fading = false;
            cache.needs_full_update = true;
            cache.needs_screen_clear = true;
        }
        else
            return resolve_mapped(original,
                                  lerp_color(cache.prev_accent, cache.accent, p),
                                  lerp_color(cache.prev_dominant, cache.dominant, p),
                                  lerp_color(cache.prev_highlight, cache.highlight, p),
                                  lerp_color(cache.prev_separator, cache.separator, p));
    }

    return resolve_mapped(original, cache.accent, cache.dominant, cache.highlight, cache.separator);
}

bool dynamic_colors_fading(void)
{
    return cache.fading || cache.fading_out;
}

bool dynamic_colors_needs_full_update(void)
{
    if (cache.needs_full_update)
    {
        cache.needs_full_update = false;
        return true;
    }
    return false;
}

bool dynamic_colors_screen_clear_needed(void)
{
    if (cache.needs_screen_clear)
    {
        cache.needs_screen_clear = false;
        return true;
    }
    return false;
}

bool dynamic_colors_pending(void)
{
    return needs_extraction && global_settings.dynamic_colors;
}

#endif /* HAVE_ALBUMART && HAVE_LCD_COLOR */
