#include "net/netif.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/tcpip.h"
#include "lwip/pbuf.h"
#include "netif/ethernet.h"
#include "sync/spinlock.h"
#include "lib/string.h"
#include "lib/kprintf.h"

namespace {

const size_t MAX_FRAME = 1514;

// Starea unei interfete; o singura interfata (prima placa) in T9.2
struct State {
    NetDevice* dev;
    struct netif nif;
    Spinlock tx_lock;
    uint8_t tx_buf[MAX_FRAME];
    YkNetifStats st;
};

State state = {};

// Apelat pentru fiecare cadru din inelul RX, din IRQ sau din task-ul de interogare al placii: nu blocheaza
void on_rx(NetDevice*, const void* frame, size_t len, void* ctx){
    State* s = (State*)ctx;
    s->st.rx_frames++;
    if(len > 0xFFFF){
        return;
    }
    struct pbuf* p = pbuf_alloc(PBUF_RAW, (u16_t)len, PBUF_POOL);
    if(!p){
        s->st.rx_nomem++;
        return;
    }
    if(pbuf_take(p, frame, (u16_t)len) != ERR_OK || s->nif.input(p, &s->nif) != ERR_OK){
        s->st.rx_nomem++;
        pbuf_free(p);
    }
}

err_t low_level_output(struct netif* n, struct pbuf* p){
    State* s = (State*)n->state;
    if(p->tot_len > MAX_FRAME){
        s->st.tx_errors++;
        return ERR_BUF;
    }
    uint64_t fl = s->tx_lock.lock_irqsave();
    pbuf_copy_partial(p, s->tx_buf, p->tot_len, 0);
    int rc = s->dev->send(s->tx_buf, p->tot_len);
    if(rc == 0){
        s->st.tx_frames++;
    } else {
        s->st.tx_errors++;
    }
    s->tx_lock.unlock_irqrestore(fl);
    return rc == 0 ? ERR_OK : ERR_IF;
}

err_t netif_setup(struct netif* n){
    State* s = (State*)n->state;
    n->name[0] = 'e';
    n->name[1] = 't';
    n->output = etharp_output;
    n->linkoutput = low_level_output;
    n->hwaddr_len = ETH_HWADDR_LEN;
    s->dev->mac(n->hwaddr);
    uint32_t mtu = s->dev->mtu();
    n->mtu = (u16_t)(mtu ? mtu : 1500);
    n->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    return ERR_OK;
}

}

struct netif* yk_netif_create(NetDevice* dev, uint32_t addr, uint32_t mask, uint32_t gw){
    if(!dev || state.dev){
        return nullptr;
    }
    state.dev = dev;
    ip4_addr_t a, m, g;
    a.addr = addr;
    m.addr = mask;
    g.addr = gw;
    if(!netif_add(&state.nif, &a, &m, &g, &state, netif_setup, tcpip_input)){
        state.dev = nullptr;
        return nullptr;
    }
    netif_set_default(&state.nif);
    netif_set_up(&state.nif);
    if(dev->link_up()){
        netif_set_link_up(&state.nif);
    }
    // Ultimul pas: de aici vin cadre in tcpip_input
    dev->set_rx_handler(on_rx, &state);
    return &state.nif;
}

void yk_netif_stats(YkNetifStats& out){
    out = state.st;
}
