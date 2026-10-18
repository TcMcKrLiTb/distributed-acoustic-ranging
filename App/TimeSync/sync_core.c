#include "sync_core.h"
#include <limits.h>
#include <string.h>

static void put32(uint8_t *p, uint32_t v) {
    p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16);
    p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v;
}
static uint32_t get32(const uint8_t *p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}
static void put64(uint8_t *p, uint64_t v) { put32(p,(uint32_t)(v>>32)); put32(p+4,(uint32_t)v); }
static uint64_t get64(const uint8_t *p) { return ((uint64_t)get32(p)<<32)|get32(p+4); }

void Sync_Encode(uint8_t out[SYNC_WIRE_SIZE], const SyncMessage *m) {
    memset(out,0,SYNC_WIRE_SIZE);
    put32(out,0x5453594EU); out[4]=1; out[5]=m->type;
    out[6]=(uint8_t)(m->flags>>8); out[7]=(uint8_t)m->flags;
    put32(out+8,m->sequence); memcpy(out+12,m->uid,12); put32(out+24,m->session);
    put64(out+28,m->timestamp_ns); put64(out+36,(uint64_t)m->offset_ns);
    put32(out+44,m->delay_ns); put32(out+48,(uint32_t)m->frequency_ppb); put32(out+52,m->samples);
}
bool Sync_Decode(SyncMessage *m, const uint8_t *p, size_t length) {
    if (!m || !p || length<SYNC_WIRE_SIZE || get32(p)!=0x5453594EU || p[4]!=1 ||
        p[5]<SYNC_HELLO || p[5]>SYNC_DELAY_RESP || (p[6]!=0) || (p[7]&~3U)) return false;
    m->type=p[5]; m->flags=p[7]; m->sequence=get32(p+8);
    memcpy(m->uid,p+12,12); m->session=get32(p+24); m->timestamp_ns=get64(p+28);
    /* Decode signed fields without implementation-defined out-of-range casts. */
    uint64_t raw=get64(p+36);
    m->offset_ns=(raw<=INT64_MAX) ? (int64_t)raw : -1-(int64_t)(UINT64_MAX-raw);
    m->delay_ns=get32(p+44);
    uint32_t freq=get32(p+48);
    m->frequency_ppb=(freq<=INT32_MAX) ? (int32_t)freq : -1-(int32_t)(UINT32_MAX-freq);
    m->samples=get32(p+52);
    return true;
}
static int64_t absolute(int64_t v) { return v<0 ? -v : v; }
static int32_t clamp_frequency(int64_t v) {
    if(v>SYNC_MAX_FREQ_PPB) return SYNC_MAX_FREQ_PPB;
    if(v<-SYNC_MAX_FREQ_PPB) return -SYNC_MAX_FREQ_PPB;
    return (int32_t)v;
}
void SyncServo_Reset(SyncServo *s) { memset(s,0,sizeof(*s)); }

SyncCorrection SyncServo_Update(SyncServo *s, uint64_t t1, uint64_t t2,
                               uint64_t t3, uint64_t t4) {
    SyncCorrection c={0};
    if(t1>INT64_MAX || t2>INT64_MAX || t3>INT64_MAX || t4>INT64_MAX ||
       t3<t2 || t4<t1 || t3-t2>1000000000ULL || t4-t1>1000000000ULL) goto rejected;
    int64_t forward=(int64_t)t2-(int64_t)t1;
    int64_t backward=(int64_t)t4-(int64_t)t3;
    /* Startup offsets cancel in the sum; bound before adding to avoid overflow. */
    if(absolute(forward)>86400000000000LL || absolute(backward)>86400000000000LL) goto rejected;
    int64_t delay=(forward+backward)/2;
    int64_t offset=(forward-backward)/2;
    if(delay<0 || delay>2000000) goto rejected;
    uint32_t minimum=(uint32_t)delay;
    for(uint32_t i=0;i<s->delay_count;i++)
        if(s->delay_window[i]<minimum) minimum=s->delay_window[i];
    /* Age the envelope even on congestion: a changed path must be able to recover. */
    bool delayed=s->delay_count>=4 && delay>(int64_t)minimum+4000;
    s->delay_window[s->delay_index]=(uint32_t)delay;
    s->delay_index=(s->delay_index+1U)%16U;
    if(s->delay_count<16) s->delay_count++;
    if(delayed) goto rejected;
    s->offset_ns=offset; s->delay_ns=(uint32_t)delay;
    s->valid=true; s->samples++; c.accepted=true;
    if(absolute(offset)>50000) {
        c.step=true; c.phase_ns=-offset;
        s->good_run=0; s->locked=false;
    } else {
        /* 8 Hz discrete PI servo, ppb = ns/s. Integral retains oscillator drift. */
        s->integral_ppb=clamp_frequency(s->integral_ppb-offset*2/5);
        s->frequency_ppb=clamp_frequency(s->integral_ppb-offset*3);
        if(absolute(offset)<=SYNC_LOCK_LIMIT_NS) {
            if(s->good_run<SYNC_LOCK_SAMPLES) s->good_run++;
        } else s->good_run=0;
        s->locked=s->good_run>=SYNC_LOCK_SAMPLES;
    }
    c.frequency_ppb=s->frequency_ppb;
    return c;
rejected:
    s->rejected++; s->good_run=0; s->locked=false;
    return c;
}

