#include "range_core.h"
#include <math.h>
#include <stddef.h>
#include <string.h>

static int64_t abs64(int64_t x) { return x < 0 ? -x : x; }
_Static_assert(RANGE_LATE_LIMIT + 96U <= RANGE_SAMPLES, "late confirmation stays inside the saved capture");

bool Range_Convert(int64_t raw, const RangeConfig *c, RangeEstimate *r) {
    if (!c || !r || c->sound_speed_mm_s < 300000U || c->sound_speed_mm_s > 380000U ||
        abs64((int64_t)c->bias_ns) > 10000000LL || raw < -RANGE_PAIR_NS || raw > RANGE_PAIR_NS)
        return false;
    r->raw_delay_ns = raw;
    r->delay_ns = raw - c->bias_ns;
    r->source_a = r->delay_ns > 0;
    r->distance_mm = (uint32_t)((abs64(r->delay_ns) * c->sound_speed_mm_s + 500000000LL) / 1000000000LL);
    r->error = r->distance_mm >= RANGE_MIN_MM - RANGE_EDGE_GUARD_MM &&
               r->distance_mm <= RANGE_MAX_MM + RANGE_EDGE_GUARD_MM ? RANGE_OK : RANGE_OUT_OF_RANGE;
    return r->error == RANGE_OK;
}

bool Range_Estimate(const RangeCapture *a, const RangeCapture *b,
                    const RangeConfig *config, RangeEstimate *r) {
    return Range_EstimateWindow(a, b, config, RANGE_LONG_BEGIN, RANGE_LONG_END, r);
}

static bool estimate_centered(const RangeCapture *a, const RangeCapture *b,
                              const RangeConfig *config, unsigned begin, unsigned end,
                              int center, RangeEstimate *r) {
    if (!r) return false;
    memset(r, 0, sizeof(*r));
    r->error = RANGE_BAD_CAPTURE;
    if (!a || !b || !config || begin >= end || end > RANGE_SAMPLES ||
        begin < RANGE_SEARCH_SAMPLES || end - begin < 3U ||
        config->sound_speed_mm_s < 300000U || config->sound_speed_mm_s > 380000U ||
        abs64((int64_t)config->bias_ns) > 10000000LL) return false;
    r->window_begin = (uint16_t)begin; r->window_end = (uint16_t)end;
    if (a->flags != RANGE_CAPTURE_LOCKED || b->flags != RANGE_CAPTURE_LOCKED ||
        a->period_ps < 20000000ULL || a->period_ps > 22000000ULL ||
        b->period_ps < 20000000ULL || b->period_ps > 22000000ULL ||
        a->onset_ns > INT64_MAX || b->onset_ns > INT64_MAX ||
        a->start_ns > a->onset_ns || b->start_ns > b->onset_ns ||
        a->onset_ns - a->start_ns > 10000000ULL || b->onset_ns - b->start_ns > 10000000ULL ||
        abs64((int64_t)b->onset_ns - (int64_t)a->onset_ns) > RANGE_PAIR_NS) return false;

    /* Resample B on A's sample spacing. start times stay in the final equation;
       PTP synchronization does not synchronize the SAI sampling edges. */
    float ratio = (float)a->period_ps / (float)b->period_ps;
    /* Check the entire interpolation domain before converting positions to
       unsigned indices, including unusual but otherwise valid clock ratios. */
    float first_position = ((float)begin + (float)center - RANGE_SEARCH_SAMPLES) * ratio;
    float last_position = ((float)(end - 1U) + (float)center + RANGE_SEARCH_SAMPLES) * ratio;
    if (first_position < 0.0f || last_position >= (float)(RANGE_SAMPLES - 1U)) return false;
    float scores[2 * RANGE_SEARCH_SAMPLES + 1];
    const float n = (float)(end - begin);
    float sum_a = 0.0f, sq_a = 0.0f;
    for (unsigned i = begin; i < end; ++i) {
        float x = a->pcm[i] / 32768.0f;
        sum_a += x; sq_a += x * x;
    }
    float energy_a = sq_a - sum_a * sum_a / n;
    if (energy_a < n * (16.0f / 32768.0f) * (16.0f / 32768.0f)) {
        r->error = RANGE_LOW_SIGNAL; return false;
    }
    float best = -2.0f;
    int peak = 0;
    for (int k = -RANGE_SEARCH_SAMPLES; k <= RANGE_SEARCH_SAMPLES; ++k) {
        float sum_b = 0.0f, sq_b = 0.0f, dot = 0.0f;
        for (unsigned i = begin; i < end; ++i) {
            float position = ((float)i + (float)k + (float)center) * ratio;
            unsigned j = (unsigned)position;
            if (j + 1U >= RANGE_SAMPLES) { r->error = RANGE_BAD_CAPTURE; return false; }
            float f = position - (float)j;
            float y = ((float)b->pcm[j] + f * ((float)b->pcm[j + 1U] - b->pcm[j])) / 32768.0f;
            float x = a->pcm[i] / 32768.0f;
            sum_b += y; sq_b += y * y; dot += x * y;
        }
        float energy_b = sq_b - sum_b * sum_b / n;
        float score = energy_b > 1.0e-10f ?
            (dot - sum_a * sum_b / n) / sqrtf(energy_a * energy_b) : -1.0f;
        scores[k + RANGE_SEARCH_SAMPLES] = score;
        if (score > best) { best = score; peak = k; }
    }
    r->quality_permille = best > 0.0f ? (uint32_t)(fminf(best, 1.0f) * 1000.0f + 0.5f) : 0;
    r->peak_lag_samples = peak + center;
    r->correlation_valid = true;
    float competitor = -2.0f;
    for (int k = -RANGE_SEARCH_SAMPLES; k <= RANGE_SEARCH_SAMPLES; ++k) {
        if (abs64(k - peak) > 12 && scores[k + RANGE_SEARCH_SAMPLES] > competitor) {
            competitor = scores[k + RANGE_SEARCH_SAMPLES];
            r->competitor_lag_samples = k + center;
        }
    }
    r->competitor_valid = competitor > -2.0f;
    r->competitor_quality_permille = competitor > 0.0f ?
        (uint32_t)(fminf(competitor, 1.0f) * 1000.0f + 0.5f) : 0;
    r->peak_margin_permille = (int32_t)lroundf((best - competitor) * 1000.0f);

    bool interior = peak != -RANGE_SEARCH_SAMPLES && peak != RANGE_SEARCH_SAMPLES;
    bool fraction_valid = false;
    float fraction = 0.0f;
    if (interior) {
        float left = scores[peak + RANGE_SEARCH_SAMPLES - 1];
        float right = scores[peak + RANGE_SEARCH_SAMPLES + 1];
        float curvature = left - 2.0f * best + right;
        fraction = curvature < -1.0e-6f ? 0.5f * (left - right) / curvature : 0.0f;
        fraction_valid = fraction >= -0.5f && fraction <= 0.5f;
        if (fraction_valid) {
            r->peak_fraction_samples = fraction;
            int64_t correction = (int64_t)llround((double)((float)peak + fraction + (float)center) *
                                                 (double)a->period_ps / 1000.0);
            int64_t delay = (int64_t)b->start_ns - (int64_t)a->start_ns + correction;
            RangeEstimate candidate = {0};
            /* A low-quality peak can have a diagnostic distance while remaining
               rejected. Range_Convert's physical range test still applies below. */
            Range_Convert(delay, config, &candidate);
            if (delay >= -RANGE_PAIR_NS && delay <= RANGE_PAIR_NS) {
                r->candidate_valid = true;
                r->candidate_raw_delay_ns = delay;
                r->candidate_distance_mm = candidate.distance_mm;
            }
        }
    }
    if (best < 0.65f) { r->error = RANGE_LOW_CORRELATION; return false; }
    if (!interior) {
        r->error = RANGE_AMBIGUOUS; return false;
    }
    for (int k = -RANGE_SEARCH_SAMPLES; k <= RANGE_SEARCH_SAMPLES; ++k) {
        if (abs64(k - peak) > 12 && scores[k + RANGE_SEARCH_SAMPLES] > best - 0.02f) {
            r->error = RANGE_AMBIGUOUS; return false;
        }
    }
    if (!fraction_valid) { r->error = RANGE_AMBIGUOUS; return false; }
    int64_t correction = (int64_t)llround((double)((float)peak + fraction + (float)center) * (double)a->period_ps / 1000.0);
    int64_t delay = (int64_t)b->start_ns - (int64_t)a->start_ns + correction;
    return Range_Convert(delay, config, r);
}

bool Range_EstimateWindow(const RangeCapture *a, const RangeCapture *b,
                         const RangeConfig *config, unsigned begin, unsigned end,
                         RangeEstimate *r) {
    return estimate_centered(a, b, config, begin, end, 0, r);
}

static bool reject_direct(RangeEstimate *r, RangeError error) {
    /* Keep diagnostic candidates, but never leave an accepted distance behind
       when another window vetoes the attempted result. */
    r->raw_delay_ns = r->delay_ns = 0;
    r->distance_mm = 0;
    r->source_a = false;
    r->error = error;
    return false;
}

static bool direct_initial(const RangeCapture *a, const RangeCapture *b,
                           const RangeConfig *config, RangeEstimate *r,
                           RangeEstimate *long_result, const RangeEstimate *preview) {
    if (!r) return false;
    memset(r, 0, sizeof(*r));
    r->error = RANGE_BAD_CAPTURE;
    if (long_result == r) return false;
    RangeEstimate support, check;
    Range_Estimate(a, b, config, &support);
    if (long_result) *long_result = support;
    if (support.error == RANGE_BAD_CAPTURE || support.error == RANGE_LOW_SIGNAL)
        return reject_direct(r, support.error);

    bool check_ok = Range_EstimateWindow(a, b, config, RANGE_FRONT_BEGIN,
                                         RANGE_DIRECT_CHECK_END, &check);
    *r = *preview;
    if (r->error != RANGE_OK) return reject_direct(r, r->error);
    if (!check_ok) return reject_direct(r, check.error);
    if (r->quality_permille < RANGE_DIRECT_MIN_QUALITY ||
        check.quality_permille < RANGE_DIRECT_MIN_QUALITY ||
        support.quality_permille < RANGE_SUPPORT_MIN_QUALITY)
        return reject_direct(r, RANGE_LOW_CORRELATION);

    /* A long-window correlation may be reduced by later reflections, but its
       unambiguous phase must still support the same direct-sound delay. This
       rejects unrelated early transients and conflicting periodic branches. */
    if ((support.error != RANGE_OK && support.error != RANGE_LOW_CORRELATION) ||
        !support.candidate_valid || !support.competitor_valid ||
        support.peak_margin_permille < RANGE_SUPPORT_MIN_MARGIN)
        return reject_direct(r, RANGE_AMBIGUOUS);
    int64_t period_ns = (int64_t)(a->period_ps / 1000U);
    if (abs64(check.raw_delay_ns - r->raw_delay_ns) > period_ns ||
        abs64(support.candidate_raw_delay_ns - r->raw_delay_ns) > 2 * period_ns)
        return reject_direct(r, RANGE_AMBIGUOUS);
    return true;
}

/* Only revisit a weak initial burst followed by a low-energy gap and a much
   stronger, confirmed burst. A continuous ramp has no gap and is not moved.
   Two passes plus a 128-sample ring keep the worker's stack bounded. */
static bool burst_anchor(const int16_t pcm[RANGE_SAMPLES], unsigned *onset) {
    int32_t hp = 0, previous = 0;
    uint32_t peak = 0, prefix = 0;
    for (unsigned i = 0; i < RANGE_SAMPLES; ++i) {
        hp = (int32_t)pcm[i] - previous + hp * 63 / 64;
        previous = pcm[i];
        uint32_t magnitude = (uint32_t)(hp < 0 ? -hp : hp);
        if (i >= 64U && magnitude > peak) peak = magnitude;
        if (i >= RANGE_PRE_SAMPLES && i < RANGE_DIRECT_END && magnitude > prefix)
            prefix = magnitude;
    }
    uint32_t threshold = peak / 4U;
    if (threshold < 128U) threshold = 128U;
    if (prefix >= threshold) {
        *onset = RANGE_PRE_SAMPLES;
        return true;
    }

    int32_t history[128];
    bool gap_history[128], gap = false;
    uint64_t energy = 0;
    hp = previous = 0;
    for (unsigned i = 0; i < RANGE_LATE_LIMIT + 96U; ++i) {
        hp = (int32_t)pcm[i] - previous + hp * 63 / 64;
        previous = pcm[i];
        if (i >= 48U) {
            int64_t old = history[(i - 48U) % 128U];
            energy -= (uint64_t)(old * old);
        }
        history[i % 128U] = hp;
        energy += (uint64_t)((int64_t)hp * hp);
        if (i >= RANGE_PRE_SAMPLES + 96U &&
            energy * 1024ULL <= 48ULL * peak * peak) gap = true;
        gap_history[i % 128U] = gap;
        if (i < RANGE_PRE_SAMPLES + 192U) continue;
        unsigned start = i - 96U;
        int32_t first = history[start % 128U], before = history[(start - 1U) % 128U];
        uint32_t magnitude = (uint32_t)(first < 0 ? -first : first);
        uint32_t prior = (uint32_t)(before < 0 ? -before : before);
        if (!gap_history[start % 128U] || magnitude < threshold || prior >= threshold) continue;
        unsigned hits = 0, crossings = 0;
        uint32_t local_peak = 0;
        bool sign = first >= 0;
        for (unsigned j = start; j <= i; ++j) {
            int32_t value = history[j % 128U];
            uint32_t abs_value = (uint32_t)(value < 0 ? -value : value);
            if (abs_value >= threshold) hits++;
            if (abs_value > local_peak) local_peak = abs_value;
            if ((value >= 0) != sign) crossings++;
            sign = value >= 0;
        }
        if (hits >= 8U && crossings >= 2U && local_peak >= 2U * threshold) {
            *onset = start;
            return true;
        }
    }
    return false;
}

static bool direct_late(const RangeCapture *a, const RangeCapture *b,
                        const RangeConfig *config, RangeEstimate *r,
                        RangeEstimate *long_result) {
    /* Invalid captures and out-of-range/ambiguous peaks never enter rescue. */
    unsigned onset_a, onset_b;
    if (!burst_anchor(a->pcm, &onset_a) || !burst_anchor(b->pcm, &onset_b) ||
        (onset_a == RANGE_PRE_SAMPLES && onset_b == RANGE_PRE_SAMPLES)) return false;
    int center = (int)lround((double)onset_b * (double)b->period_ps / (double)a->period_ps) - (int)onset_a;
    unsigned begin = onset_a - 128U;
    unsigned end = RANGE_LONG_END;
    float ratio = (float)a->period_ps / (float)b->period_ps;
    while (end > begin && ((float)(end - 1U) + center + RANGE_SEARCH_SAMPLES) * ratio >= RANGE_SAMPLES - 1U)
        end--;
    if (end - begin < RANGE_LATE_SUPPORT_MIN) return false;
    RangeEstimate support, check, direct;
    estimate_centered(a, b, config, begin, end, center, &support);
    bool check_ok = estimate_centered(a, b, config, begin, onset_a + 192U, center, &check);
    bool direct_ok = estimate_centered(a, b, config, begin, onset_a + 224U, center, &direct);
    if (!check_ok || !direct_ok || check.quality_permille < RANGE_DIRECT_MIN_QUALITY ||
        direct.quality_permille < RANGE_DIRECT_MIN_QUALITY ||
        support.quality_permille < RANGE_SUPPORT_MIN_QUALITY ||
        (support.error != RANGE_OK && support.error != RANGE_LOW_CORRELATION) ||
        !support.candidate_valid || !support.competitor_valid ||
        support.peak_margin_permille < RANGE_SUPPORT_MIN_MARGIN) return false;
    int64_t period_ns = (int64_t)(a->period_ps / 1000U);
    if (abs64(check.raw_delay_ns - direct.raw_delay_ns) > period_ns ||
        abs64(support.candidate_raw_delay_ns - direct.raw_delay_ns) > 2 * period_ns) return false;
    *r = direct;
    if (long_result) *long_result = support;
    return true;
}

bool Range_EstimateDirect(const RangeCapture *a, const RangeCapture *b,
                         const RangeConfig *config, RangeEstimate *r,
                         RangeEstimate *long_result) {
    if (!r) return false;
    if (long_result == r) {
        memset(r, 0, sizeof(*r)); r->error = RANGE_BAD_CAPTURE;
        return false;
    }
    RangeEstimate preview;
    Range_EstimateWindow(a, b, config, RANGE_FRONT_BEGIN, RANGE_DIRECT_END, &preview);
    /* Preview also validates flags, clocks and interpolation bounds. Try a
       distinct later burst only for low correlation, never to bypass an
       ambiguous or out-of-range early peak. Reuse the preview on the normal
       path so a rescue does not pay for two full long-window searches. */
    if (preview.error == RANGE_LOW_CORRELATION && direct_late(a, b, config, r, long_result))
        return true;
    return direct_initial(a, b, config, r, long_result, &preview);
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }
static void put32(uint8_t *p, uint32_t v) { for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (24U - i * 8U)); }
static uint32_t get32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)(v >> 32)); put32(p + 4, (uint32_t)v); }
static uint64_t get64(const uint8_t *p) { return ((uint64_t)get32(p) << 32) | get32(p + 4); }

bool Range_IsPacket(const uint8_t p[RANGE_WIRE_SIZE]) { return p && get32(p) == 0x524E4733U; }
void Range_Encode(uint8_t p[RANGE_WIRE_SIZE], const RangePacket *m) {
    memset(p, 0, RANGE_WIRE_SIZE);
    put32(p, 0x524E4733U); p[4] = 1; p[5] = m->type;
    put32(p + 8, m->session); put32(p + 12, m->sequence);
    if (m->type == RANGE_META) {
        put64(p + 16, m->onset_ns); put64(p + 24, m->start_ns); put64(p + 32, m->period_ps);
        put32(p + 40, m->generation); put32(p + 44, m->flags); put32(p + 48, RANGE_SAMPLES);
        put32(p + 52, m->config_revision);
    } else if (m->type == RANGE_DATA) {
        put16(p + 16, m->offset); put16(p + 18, m->count);
        for (unsigned i = 0; i < RANGE_CHUNK_SAMPLES; ++i) put16(p + 20 + 2U * i, (uint16_t)m->pcm[i]);
    } else if (m->type == RANGE_ACK) {
        put16(p + 16, m->offset);
    } else if (m->type == RANGE_RESULT) {
        put64(p + 16, (uint64_t)m->delay_ns); put32(p + 24, m->distance_mm);
        put32(p + 28, m->quality_permille); put64(p + 32, m->apply_ns);
        put32(p + 40, m->a_sequence); put32(p + 44, m->b_sequence);
        put32(p + 48, m->generation); /* Receiver B's local epoch, not A's. */
        put32(p + 52, m->config_revision);
    } else if (m->type >= RANGE_CONFIG_REQUEST && m->type <= RANGE_SENSOR_UPDATE) {
        put16(p + 16, (uint16_t)m->temperature_c_x10);
        p[18] = m->temperature_source; p[19] = m->sensor_present ? 1U : 0U;
        put32(p + 44, m->source_epoch);
        if (m->type == RANGE_CONFIG_STATE) {
            put32(p + 20, m->request_ack);
            put32(p + 24, m->flags);
            put64(p + 28, (uint64_t)m->delay_ns);
            put32(p + 36, m->a_sequence);
            put32(p + 40, m->quality_permille);
        }
    }
}
bool Range_Decode(RangePacket *m, const uint8_t p[RANGE_WIRE_SIZE]) {
    if (!m || !Range_IsPacket(p) || p[4] != 1 || p[5] < RANGE_META || p[5] > RANGE_SENSOR_UPDATE || p[6] || p[7]) return false;
    memset(m, 0, sizeof(*m));
    m->type = p[5]; m->session = get32(p + 8); m->sequence = get32(p + 12);
    if (m->type == RANGE_META) {
        m->onset_ns = get64(p + 16); m->start_ns = get64(p + 24); m->period_ps = get64(p + 32);
        m->generation = get32(p + 40); m->flags = get32(p + 44);
        m->config_revision = get32(p + 52);
        if (get32(p + 48) != RANGE_SAMPLES || m->flags != RANGE_CAPTURE_LOCKED ||
            m->start_ns > m->onset_ns || m->onset_ns > INT64_MAX ||
            m->onset_ns - m->start_ns > 10000000ULL ||
            m->period_ps < 20000000ULL || m->period_ps > 22000000ULL) return false;
    } else if (m->type == RANGE_DATA || m->type == RANGE_ACK) {
        m->offset = get16(p + 16);
        if (m->offset > RANGE_SAMPLES || m->offset % RANGE_CHUNK_SAMPLES) return false;
        if (m->type == RANGE_DATA) {
            m->count = get16(p + 18);
            if (m->count != RANGE_CHUNK_SAMPLES || m->offset + m->count > RANGE_SAMPLES) return false;
            for (unsigned i = 0; i < RANGE_CHUNK_SAMPLES; ++i) {
                uint16_t raw = get16(p + 20 + 2U * i);
                m->pcm[i] = raw <= INT16_MAX ? (int16_t)raw : (int16_t)(-1 - (int32_t)(UINT16_MAX - raw));
            }
        }
    } else if (m->type == RANGE_RESULT) {
        uint64_t raw = get64(p + 16);
        m->delay_ns = raw <= INT64_MAX ? (int64_t)raw : -1 - (int64_t)(UINT64_MAX - raw);
        m->distance_mm = get32(p + 24); m->quality_permille = get32(p + 28); m->apply_ns = get64(p + 32);
        m->a_sequence = get32(p + 40); m->b_sequence = get32(p + 44); m->generation = get32(p + 48);
        m->config_revision = get32(p + 52);
        if (m->distance_mm < RANGE_MIN_MM - RANGE_EDGE_GUARD_MM ||
            m->distance_mm > RANGE_MAX_MM + RANGE_EDGE_GUARD_MM || m->quality_permille < 650U ||
            m->quality_permille > 1000U || m->delay_ns == 0 || m->delay_ns < -RANGE_PAIR_NS ||
            m->delay_ns > RANGE_PAIR_NS || m->apply_ns > INT64_MAX) return false;
    } else if (m->type >= RANGE_CONFIG_REQUEST && m->type <= RANGE_SENSOR_UPDATE) {
        uint16_t raw = get16(p + 16);
        m->temperature_c_x10 = raw <= INT16_MAX ? (int16_t)raw :
            (int16_t)(-1 - (int32_t)(UINT16_MAX - raw));
        m->temperature_source = p[18]; m->sensor_present = p[19] != 0U;
        m->source_epoch = get32(p + 44);
        m->config_revision = m->type == RANGE_CONFIG_STATE || m->type == RANGE_CONFIG_ACK ? m->sequence : 0;
        m->request_ack = m->type == RANGE_CONFIG_STATE ? get32(p + 20) : 0;
        if (!m->sequence || m->temperature_c_x10 < -200 || m->temperature_c_x10 > 500 ||
            m->temperature_source > 2U || p[19] > 1U || !m->source_epoch ||
            (m->type == RANGE_SENSOR_UPDATE && m->temperature_source != 2U)) return false;
        if (m->type == RANGE_CONFIG_STATE) {
            m->flags = get32(p + 24);
            uint64_t delay = get64(p + 28);
            m->delay_ns = delay <= INT64_MAX ? (int64_t)delay : -1 - (int64_t)(UINT64_MAX - delay);
            m->a_sequence = get32(p + 36);
            m->quality_permille = get32(p + 40);
            if (m->flags > 1U) return false;
            if (m->flags && (!m->a_sequence || m->quality_permille < 650U ||
                m->quality_permille > 1000U || !m->delay_ns ||
                m->delay_ns < -RANGE_PAIR_NS || m->delay_ns > RANGE_PAIR_NS)) return false;
            if (!m->flags && (m->delay_ns || m->a_sequence || m->quality_permille)) return false;
        }
    }
    return true;
}
