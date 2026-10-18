#include "sync_app.h"
#include "sync_core.h"
#include "sync_port.h"
#include "sync_format.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static struct {
    uint8_t uid[12], mac[6], peer_uid[12], peer_mac[6];
    uint32_t session, peer_session, hello_ms, event_ms, log_ms, peer_ms, sample_ms;
    uint32_t sequence, pending_sequence, pending_ms, rx_count, tx_failures, ignored;
    uint64_t t1,t2,t3;
    bool ready, link, peer, master, pending, follow, peer_locked, peer_valid, seen_event;
    int64_t peer_offset;
    uint32_t peer_delay;
    SyncServo servo;
} app;
static const uint8_t broadcast[6]={255,255,255,255,255,255};

static bool send_message(uint8_t type, uint32_t seq, uint64_t time, bool stamp, uint64_t *tx) {
    SyncMessage m = {0}; uint8_t payload[SYNC_WIRE_SIZE];
    m.type = type; m.sequence = seq; m.timestamp_ns = time; m.session = app.session;
    memcpy(m.uid, app.uid,12);
    m.offset_ns = app.servo.offset_ns; m.delay_ns = app.servo.delay_ns;
    m.frequency_ppb = app.servo.frequency_ppb; m.samples = app.servo.samples;
    if(app.servo.locked)
        m.flags |= SYNC_FLAG_LOCKED;
    if(app.servo.valid)
        m.flags |= SYNC_FLAG_SAMPLE_VALID;
    Sync_Encode(payload, &m);
    bool ok = SyncPort_Send(type == SYNC_HELLO ? broadcast : app.peer_mac, payload, stamp, tx);
    if(!ok)
        app.tx_failures++;
    return ok;
}
static void clear_peer(void) {
    app.peer=false; app.pending=false; app.follow=false;
    app.seen_event=false;
    app.peer_locked=false; app.peer_valid=false;
    SyncServo_Reset(&app.servo);
    if(!SyncPort_Correct(0,0)) app.ready=false;
}
static void receive(const SyncRxPacket *p, uint32_t now) {
    SyncMessage m;
    if(!Sync_Decode(&m,p->payload,SYNC_WIRE_SIZE) || !memcmp(m.uid,app.uid,12)) return;
    if(m.type==SYNC_HELLO) {
        if(app.peer && memcmp(m.uid,app.peer_uid,12) != 0) { app.ignored++; return; }
        if(!app.peer || m.session!=app.peer_session) {
            clear_peer();
            memcpy(app.peer_uid,m.uid,12); memcpy(app.peer_mac,p->source_mac,6);
            app.peer_session=m.session; app.peer=true;
            app.master=memcmp(app.uid,app.peer_uid,12)<0;
            app.event_ms=now-125U;
        }
        /* Source MAC must stay consistent with the selected UID. */
        if(memcmp(p->source_mac,app.peer_mac,6) != 0) return;
        app.peer_ms=now;
        app.peer_locked=(m.flags&SYNC_FLAG_LOCKED)!=0;
        app.peer_valid=(m.flags&SYNC_FLAG_SAMPLE_VALID)!=0;
        app.peer_offset=m.offset_ns; app.peer_delay=m.delay_ns;
        return;
    }
    if(!app.peer || memcmp(m.uid,app.peer_uid,12) != 0 || m.session!=app.peer_session ||
       memcmp(p->source_mac,app.peer_mac,6) != 0) return;
    app.rx_count++;
    if(app.master) {
        if(m.type==SYNC_DELAY_REQ && m.sequence==app.sequence && p->timestamp_valid)
            send_message(SYNC_DELAY_RESP,m.sequence,p->timestamp_ns,false,NULL);
        return;
    }
    if(m.type==SYNC_EVENT && p->timestamp_valid) {
        uint32_t delta=m.sequence-app.pending_sequence;
        if(app.seen_event && (delta==0U || delta>=0x80000000U)) return;
        app.seen_event=true;
        app.pending=true; app.follow=false; app.pending_sequence=m.sequence;
        app.t2=p->timestamp_ns; app.pending_ms=now;
    } else if(m.type==SYNC_FOLLOW_UP && app.pending && !app.follow &&
              m.sequence==app.pending_sequence && now-app.pending_ms<100U) {
        app.t1=m.timestamp_ns;
        app.follow=send_message(SYNC_DELAY_REQ,m.sequence,0,true,&app.t3);
        if(!app.follow) app.pending=false;
    } else if(m.type==SYNC_DELAY_RESP && app.pending && app.follow &&
              m.sequence==app.pending_sequence && now-app.pending_ms<100U) {
        app.pending=false;
        SyncCorrection c=SyncServo_Update(&app.servo,app.t1,app.t2,app.t3,m.timestamp_ns);
        if(c.accepted) {
            app.sample_ms=now;
            if(!SyncPort_Correct(c.phase_ns,c.frequency_ppb)) {
                app.ready=false; app.servo.locked=false;
                SyncPort_Log("ERROR: PTP clock update timeout\r\n");
            }
        }
    }
}

void SyncApp_Init(void) {
    memset(&app,0,sizeof(app));
    app.ready=SyncPort_Init(app.uid,app.mac);
    app.session=SyncPort_Session();
    app.hello_ms=SyncPort_Millis()-500U;
    SyncPort_Log("\r\nDistributed time sync / TouchGFX + FreeRTOS\r\n"
                 "PPS: ETH target -> TIM2 one-pulse -> D9 / PA15, 1 ms; UART1: 1152000 8N1\r\n"
                 "Private L2 protocol 0x88B5; hardware TX/RX timestamps\r\n");
    if(!app.ready) SyncPort_Log("ERROR: ETH/PHY/PTP initialization failed; reset after checking power/clock\r\n");
}
void SyncApp_Poll(void) {
    uint32_t now=SyncPort_Millis();
    if(app.ready) {
        bool link=SyncPort_Link();
        if(app.link && !link) clear_peer();
        app.link=link;
        if(app.peer && now-app.peer_ms>2000U) clear_peer();
        if(!app.master && app.servo.valid && now-app.sample_ms>500U) {
            app.servo.valid=false; app.servo.locked=false; app.servo.good_run=0;
        }
        SyncRxPacket p;
        for(unsigned i=0;i<16 && app.link && SyncPort_Receive(&p);i++) receive(&p,now);
        if(app.link && now-app.hello_ms>=500U) {
            app.hello_ms=now; send_message(SYNC_HELLO,0,0,false,NULL);
        }
        if(app.link && app.peer && app.master && now-app.event_ms>=125U) {
            app.event_ms=now; uint64_t tx;
            app.sequence++;
            if(send_message(SYNC_EVENT,app.sequence,0,true,&tx))
                send_message(SYNC_FOLLOW_UP,app.sequence,tx,false,NULL);
        }
        if(app.pending && now-app.pending_ms>=100U) app.pending=false;
        SyncPort_PpsPoll();
    }
    bool locked=app.ready && app.link && app.peer &&
                (app.master ? app.peer_locked : app.servo.locked);
    SyncPort_Led(locked);
    if(now-app.log_ms>=1000U) {
        app.log_ms=now; char line[384];
        uint32_t rises,falls,high_seen,low_seen,missed;
        SyncPort_PpsStats(&rises,&falls,&high_seen,&low_seen,&missed);
        const char *role=app.peer ? (app.master ? "MASTER(A)" : "SLAVE(B)") : "DISCOVERY";
        const char *state=!app.ready ? "ERROR" : !app.link ? "LINK_DOWN" :
                          !app.peer ? "WAIT_PEER" : locked ? "LOCKED_ESTIMATE" : "ACQUIRING";
        bool valid=app.peer && (app.master ? app.peer_valid : app.servo.valid);
        int64_t offset=app.master ? app.peer_offset : app.servo.offset_ns;
        uint32_t delay=app.master ? app.peer_delay : app.servo.delay_ns;
        char offset_text[SYNC_I64_TEXT_SIZE];
        SyncFormat_I64(offset_text,offset);
        snprintf(line,sizeof(line),"uptime_ms=%" PRIu32 " role=%s state=%s sample=%s "
            "slave_minus_master_ns=%s delay_ns=%" PRIu32 " correction_ppb=%" PRId32
            " samples=%" PRIu32 " rejected=%" PRIu32 " tx_fail=%" PRIu32 " rx=%" PRIu32
            " pps_rise=%" PRIu32 " pps_fall=%" PRIu32 " pin_hi=%" PRIu32 " pin_lo=%" PRIu32
            " pps_missed=%" PRIu32 "\r\n",
            now,role,state,valid?"VALID":"NONE",offset_text,delay,app.servo.frequency_ppb,
            app.servo.samples,app.servo.rejected,app.tx_failures,app.rx_count,
            rises,falls,high_seen,low_seen,missed);
        SyncPort_Log(line);
    }
}

void SyncApp_GetStatus(TimeSyncStatus *status) {
    memset(status,0,sizeof(*status));
    status->ready=app.ready;
    status->link_up=app.link;
    status->role=!app.peer ? TIME_SYNC_DISCOVERY :
                 app.master ? TIME_SYNC_MASTER : TIME_SYNC_SLAVE;
    status->sample_valid=app.peer && (app.master ? app.peer_valid : app.servo.valid);
    status->locked=app.ready && app.link && app.peer &&
                   (app.master ? app.peer_locked : app.servo.locked);
    status->state=!app.ready ? TIME_SYNC_ERROR : !app.link ? TIME_SYNC_LINK_DOWN :
                  !app.peer ? TIME_SYNC_WAIT_PEER : status->locked ?
                  TIME_SYNC_LOCKED_ESTIMATE : TIME_SYNC_ACQUIRING;
    memcpy(status->uid,app.uid,sizeof(status->uid));
    memcpy(status->mac,app.mac,sizeof(status->mac));
    status->uptime_ms=SyncPort_Millis();
    if(app.peer) {
        status->offset_ns=app.master ? app.peer_offset : app.servo.offset_ns;
        status->delay_ns=app.master ? app.peer_delay : app.servo.delay_ns;
    }
    status->frequency_ppb=app.servo.frequency_ppb;
    status->samples=app.servo.samples;
    status->rejected=app.servo.rejected;
    status->tx_failures=app.tx_failures;
    status->rx_count=app.rx_count;
    SyncPort_PpsStats(&status->pps_rises,&status->pps_falls,
                     &status->pin_high_seen,&status->pin_low_seen,&status->pps_missed);
}
