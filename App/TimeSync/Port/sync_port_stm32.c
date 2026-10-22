#include "sync_port.h"
#include "sync_pps.h"
#include "sync_hw.h"
#include "sync_log.h"
#include "lan8742.h"
#include <stddef.h>
#include <string.h>

/* The linker reserves non-cacheable SRAM2 for every ETH descriptor and buffer. */
#define DMA_MEMORY __attribute__((section(".eth_dma"),aligned(32)))
typedef struct RxBuffer {
    struct RxBuffer *next;
    uint16_t length;
    bool used;
    uint8_t data[1536] __attribute__((aligned(32)));
} RxBuffer;
static RxBuffer rx_pool[8] DMA_MEMORY;
static uint8_t tx_frame[96] DMA_MEMORY;
static uint8_t local_mac[6];
static lan8742_Object_t phy;
static bool running, tx_pending, ptp_ready;
static volatile bool phase_update;
static volatile uint32_t phase_generation;
static uint32_t tx_index, phy_poll_ms, base_addend;
static int32_t link_state, correction_ppb;

static int32_t phy_init(void) { return 0; }
static int32_t phy_deinit(void) { return 0; }
static int32_t phy_read(uint32_t dev,uint32_t reg,uint32_t *value) {
    return HAL_ETH_ReadPHYRegister(&heth,dev,reg,value)==HAL_OK ? 0 : -1;
}
static int32_t phy_write(uint32_t dev,uint32_t reg,uint32_t value) {
    return HAL_ETH_WritePHYRegister(&heth,dev,reg,value)==HAL_OK ? 0 : -1;
}
static int32_t phy_tick(void) { return (int32_t)HAL_GetTick(); }

static bool ptp_wait(uint32_t bits) {
    uint32_t start=HAL_GetTick();
    while(ETH->PTPTSCR & bits) if(HAL_GetTick()-start>10U) return false;
    return true;
}
uint64_t SyncPort_Time(void) {
    uint32_t s1,s2,ns;
    /* Unlike the HAL helper, retry across the seconds rollover. */
    do { s1=ETH->PTPTSHR; ns=ETH->PTPTSLR; s2=ETH->PTPTSHR; } while(s1!=s2);
    return (uint64_t)s1*1000000000ULL+ns;
}
bool SyncPort_CaptureClock(uint64_t *time_ns, uint32_t *generation) {
    if (!ptp_ready || phase_update) return false;
    for (unsigned retry = 0; retry < 3; retry++) {
        uint32_t epoch = phase_generation;
        uint32_t s1 = ETH->PTPTSHR, ns = ETH->PTPTSLR, s2 = ETH->PTPTSHR;
        if (s1 == s2 && ns < 1000000000U && !phase_update &&
            epoch == phase_generation &&
            !(ETH->PTPTSCR & (ETH_PTPTSCR_TSSTU | ETH_PTPTSCR_TSSTI))) {
            *time_ns = (uint64_t)s1 * 1000000000ULL + ns;
            *generation = epoch;
            return true;
        }
    }
    return false;
}
static bool ptp_init(void) {
    base_addend=(uint32_t)(((1ULL<<32)*50000000ULL)/HAL_RCC_GetHCLKFreq());
    ETH->PTPTSCR=ETH_PTPTSCR_TSE|ETH_PTPTSCR_TSFCU|ETH_PTPTSCR_TSSSR|ETH_PTPTSCR_TSSARFE;
    ETH->PTPSSIR=20U;
    ETH->PTPTSAR=base_addend;
    ETH->PTPTSCR|=ETH_PTPTSCR_TSARU;
    if(!ptp_wait(ETH_PTPTSCR_TSARU)) return false;
    ETH->PTPTSHUR=0; ETH->PTPTSLUR=0;
    ETH->PTPTSCR|=ETH_PTPTSCR_TSSTI;
    if(!ptp_wait(ETH_PTPTSCR_TSSTI)) return false;
    ETH->MACIMR|=ETH_MACIMR_TSTIM;
    heth.IsPtpConfigured=HAL_ETH_PTP_CONFIGURED;
    return true;
}

bool SyncPort_Correct(int64_t phase_ns,int32_t frequency_ppb) {
    if(!ptp_ready) return true;
    if(frequency_ppb>SYNC_MAX_FREQ_PPB || frequency_ppb<-SYNC_MAX_FREQ_PPB) return false;
    if(phase_ns) {
        phase_update=true;
        phase_generation++;
        __DMB();
        uint64_t amount=(uint64_t)(phase_ns<0 ? -phase_ns : phase_ns);
        uint32_t seconds=(uint32_t)(amount/1000000000ULL), ns=(uint32_t)(amount%1000000000ULL);
        if(!ptp_wait(ETH_PTPTSCR_TSSTU|ETH_PTPTSCR_TSSTI)) return false;
        SyncPps_Suspend(); /* The old target must not fire across a phase jump. */
        if(phase_ns<0) {
            /* F746's GMAC3 uses a positive seconds magnitude with ADDSUB.
               Negating seconds (the GMAC4 convention) doubles a positive offset. */
            ETH->PTPTSHUR=seconds;
            ETH->PTPTSLUR=ETH_PTPTSLUR_TSUPNS|(1000000000U-ns);
        } else { ETH->PTPTSHUR=seconds; ETH->PTPTSLUR=ns; }
        ETH->PTPTSCR|=ETH_PTPTSCR_TSSTU;
        if(!ptp_wait(ETH_PTPTSCR_TSSTU)) return false;
    }
    if(frequency_ppb!=correction_ppb) {
        int64_t adjustment=(int64_t)base_addend*frequency_ppb/1000000000LL;
        ETH->PTPTSAR=(uint32_t)((int64_t)base_addend+adjustment);
        ETH->PTPTSCR|=ETH_PTPTSCR_TSARU;
        if(!ptp_wait(ETH_PTPTSCR_TSARU)) return false;
        correction_ppb=frequency_ppb;
    }
    if(phase_ns) {
        SyncPps_Rearm();
        __DMB();
        phase_update=false;
    }
    return true;
}
void SyncPort_PpsPoll(void) {
    if(ptp_ready) SyncPps_Poll();
}
void SyncPort_PpsStats(uint32_t *rises,uint32_t *falls,uint32_t *high_seen,uint32_t *low_seen,
                       uint32_t *missed) {
    SyncPps_Stats(rises,falls,high_seen,low_seen,missed);
}

void HAL_ETH_RxAllocateCallback(uint8_t **buffer) {
    *buffer=NULL;
    for(unsigned i=0;i<8;i++) if(!rx_pool[i].used) {
        rx_pool[i].used=true; rx_pool[i].next=NULL; rx_pool[i].length=0;
        *buffer=rx_pool[i].data; break;
    }
}
void HAL_ETH_RxLinkCallback(void **start,void **end,uint8_t *buffer,uint16_t length) {
    RxBuffer *b=(RxBuffer *)(buffer - offsetof(RxBuffer,data));
    b->next=NULL; b->length=length;
    if(!*start) *start=b; else ((RxBuffer *)*end)->next=b;
    *end=b;
}
void HAL_ETH_TxFreeCallback(uint32_t *buffer) { (void)buffer; }

bool SyncPort_Init(uint8_t uid[12],uint8_t mac[6]) {
    /* .eth_dma is NOLOAD and is deliberately outside the startup .bss clear. */
    memset(rx_pool,0,sizeof(rx_pool));
    memset(tx_frame,0,sizeof(tx_frame));
    uint32_t words[3]={HAL_GetUIDw0(),HAL_GetUIDw1(),HAL_GetUIDw2()};
    for(unsigned w=0;w<3;w++) for(unsigned b=0;b<4;b++) uid[w*4+b]=(uint8_t)(words[w]>>(24-8*b));
    /* Derive a locally administered unicast MAC; role election uses the full 96-bit UID. */
    uint64_t hash=0xcbf29ce484222325ULL;
    for(unsigned i=0;i<12;i++) { hash^=uid[i]; hash*=0x100000001b3ULL; }
    local_mac[0]=2;
    for(unsigned i=1;i<6;i++) local_mac[i]=(uint8_t)(hash>>(8U*(i-1)));
    memcpy(mac,local_mac,6); memcpy(heth.Init.MACAddr,local_mac,6);
    /* MX_ETH_Init ran with its placeholder MAC; install the unique address in hardware. */
    ETH->MACA0HR=((uint32_t)local_mac[5]<<8)|local_mac[4];
    ETH->MACA0LR=((uint32_t)local_mac[3]<<24)|((uint32_t)local_mac[2]<<16)|
                 ((uint32_t)local_mac[1]<<8)|local_mac[0];
    lan8742_IOCtx_t io={phy_init,phy_deinit,phy_write,phy_read,phy_tick};
    if(LAN8742_RegisterBusIO(&phy,&io)!=LAN8742_STATUS_OK || LAN8742_Init(&phy)!=LAN8742_STATUS_OK)
        return false;
    if(!ptp_init()) return false;
    ptp_ready=true; link_state=LAN8742_STATUS_LINK_DOWN;
    phy_poll_ms=HAL_GetTick()-250U;
    return SyncPps_Init();
}
bool SyncPort_Link(void) {
    if(HAL_GetTick()-phy_poll_ms<250U) return running;
    phy_poll_ms=HAL_GetTick(); int32_t state=LAN8742_GetLinkState(&phy);
    if(state<=LAN8742_STATUS_LINK_DOWN) {
        if(running) HAL_ETH_Stop(&heth);
        running=false; link_state=state; return false;
    }
    if(!running || state!=link_state) {
        if(running) HAL_ETH_Stop(&heth);
        ETH_MACConfigTypeDef config;
        if(HAL_ETH_GetMACConfig(&heth,&config)!=HAL_OK) return false;
        config.Speed=(state==LAN8742_STATUS_100MBITS_FULLDUPLEX ||
                      state==LAN8742_STATUS_100MBITS_HALFDUPLEX) ? ETH_SPEED_100M : ETH_SPEED_10M;
        config.DuplexMode=(state==LAN8742_STATUS_100MBITS_FULLDUPLEX ||
                           state==LAN8742_STATUS_10MBITS_FULLDUPLEX) ? ETH_FULLDUPLEX_MODE : ETH_HALFDUPLEX_MODE;
        if(HAL_ETH_SetMACConfig(&heth,&config)!=HAL_OK || HAL_ETH_Start(&heth)!=HAL_OK) {
            running=false; return false;
        }
        running=true; link_state=state;
    }
    return running;
}
bool SyncPort_Receive(SyncRxPacket *packet) {
    if(!running) return false;
    RxBuffer *first=NULL;
    if(HAL_ETH_ReadData(&heth,(void **)&first)!=HAL_OK || !first) return false;
    uint8_t frame[96]; size_t copied=0,total=0;
    for(RxBuffer *b=first;b;) {
        size_t length=b->length;
        if(length>sizeof(b->data)) length=sizeof(b->data);
        size_t take=length<sizeof(frame)-copied ? length : sizeof(frame)-copied;
        memcpy(frame+copied,b->data,take); copied+=take; total+=length;
        RxBuffer *next=b->next; b->used=false; b=next;
    }
    uint32_t errors;
    HAL_ETH_GetRxDataErrorCode(&heth,&errors);
    uint32_t status=heth.RxDescList.pRxLastRxDesc;
    /* RM0385 RDES0[7] is TSV in enhanced mode. The HAL's legacy error mask
       calls it IPV4HCE and incorrectly rejects every timestamped frame. */
    errors&=~ETH_DMARXDESC_IPV4HCE;
    errors|=status&ETH_DMARXDESC_ES;
    if(errors || total<14U+SYNC_WIRE_SIZE || frame[12]!=(SYNC_ETHERTYPE>>8) ||
       frame[13]!=(SYNC_ETHERTYPE&255U) || (memcmp(frame,local_mac,6) && memcmp(frame,"\xff\xff\xff\xff\xff\xff",6)))
        return false;
    memcpy(packet->source_mac,frame+6,6); memcpy(packet->payload,frame+14,SYNC_WIRE_SIZE);
    ETH_TimeStampTypeDef stamp=heth.RxDescList.TimeStamp;
    packet->timestamp_valid=(status&ETH_DMARXDESC_IPV4HCE)!=0 &&
                            stamp.TimeStampHigh!=UINT32_MAX && stamp.TimeStampLow<1000000000U;
    packet->timestamp_ns=(uint64_t)stamp.TimeStampHigh*1000000000ULL+stamp.TimeStampLow;
    return true;
}
static bool finish_transmit(void) {
    if(!tx_pending) return true;
    uint32_t start=HAL_GetTick();
    while(heth.Init.TxDesc[tx_index].DESC0&ETH_DMATXDESC_OWN) {
        if(HAL_GetTick()-start>=10U) return false; /* Keep buffer alive until DMA releases it. */
    }
    __DMB();
    if(HAL_ETH_ReleaseTxPacket(&heth)!=HAL_OK) return false;
    tx_pending=false;
    return true;
}
bool SyncPort_Send(const uint8_t destination[6],const uint8_t payload[SYNC_WIRE_SIZE],
                   bool timestamp,uint64_t *transmit_ns) {
    if(!running || !finish_transmit()) return false;
    memcpy(tx_frame,destination,6); memcpy(tx_frame+6,local_mac,6);
    tx_frame[12]=(uint8_t)(SYNC_ETHERTYPE>>8); tx_frame[13]=(uint8_t)SYNC_ETHERTYPE;
    memcpy(tx_frame+14,payload,SYNC_WIRE_SIZE);
    ETH_BufferTypeDef buffer={tx_frame,14U+SYNC_WIRE_SIZE,NULL};
    ETH_TxPacketConfigTypeDef config={0};
    config.Attributes=ETH_TX_PACKETS_FEATURES_CRCPAD;
    config.CRCPadCtrl=ETH_CRC_PAD_INSERT; config.TxBuffer=&buffer; config.Length=buffer.len;
    config.pData=tx_frame;
    tx_index=heth.TxDescList.CurTxDesc;
    ETH_DMADescTypeDef *d=&heth.Init.TxDesc[tx_index];
    /* Clear stale status, preserve descriptor chaining, request hardware TX timestamp. */
    d->DESC0=ETH_DMATXDESC_TCH|(timestamp ? ETH_DMATXDESC_TTSE : 0U);
    d->DESC6=UINT32_MAX; d->DESC7=UINT32_MAX; __DMB();
    if(HAL_ETH_Transmit_IT(&heth,&config)!=HAL_OK) return false;
    tx_pending=true;
    uint32_t start=HAL_GetTick();
    while(d->DESC0&ETH_DMATXDESC_OWN) if(HAL_GetTick()-start>=10U) return false;
    __DMB();
    bool valid=(d->DESC0&ETH_DMATXDESC_ES)==0;
    if(timestamp) {
        valid=valid && (d->DESC0&ETH_DMATXDESC_TTSS) && d->DESC6<1000000000U && d->DESC7!=UINT32_MAX;
        if(valid && transmit_ns) *transmit_ns=(uint64_t)d->DESC7*1000000000ULL+d->DESC6;
    }
    /* Read DESC6/7 directly: this HAL revision's GetTxTimestamp reads DESC0/1. */
    if(!finish_transmit()) return false;
    return valid;
}
uint32_t SyncPort_Millis(void) { return HAL_GetTick(); }
uint32_t SyncPort_Session(void) {
    uint32_t session=0;
    if(HAL_RNG_GenerateRandomNumber(&hrng,&session)!=HAL_OK)
        session=(uint32_t)SyncPort_Time()^HAL_GetUIDw0()^HAL_GetTick();
    return session;
}
void SyncPort_Log(const char *text) {
    SyncLog_Write(text);
}
void SyncPort_Led(bool locked) {
    /* LED1 belongs to the TouchGFX button. Keep this optional status hook from
       overwriting the user's toggle; sync status is available via the API/log. */
    (void)locked;
}
