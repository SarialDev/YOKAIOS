#include "fs/jbd2/jbd2.h"
#include "block/bcache.h"
#include "fs/vfs/inode.h"
#include "mm/kmalloc.h"
#include "lib/string.h"
#include "lib/kprintf.h"
#include "lib/crc.h"
#include "uapi/syscall_nr.h"

void (*jbd2_crash_hook)(int stage) = nullptr;

namespace {

const uint32_t MAGIC = 0xC03B3998u;
enum { T_DESC = 1, T_COMMIT = 2, T_SB1 = 3, T_SB2 = 4, T_REVOKE = 5 };
const uint32_t F_ESCAPE = 1, F_SAME_UUID = 2, F_LAST = 8;
const uint32_t INC_REVOKE = 1, INC_64BIT = 2, INC_CSUM3 = 0x10;
const uint32_t TX_CAP = 160;                    // blocuri de metadate intr-o tranzactie
const uint32_t REV_CAP = 512;
const uint32_t HASH_SIZE = 512;
const uint32_t SB_BYTES = 1024;
const uint32_t SB_CSUM_OFF = 0xFC;

enum { PASS_SCAN, PASS_REVOKE, PASS_REPLAY };

struct Slot {
    uint64_t blk;
    Buf* buf;
};

}

struct Journal {
    BlockDevice* dev;
    Jbd2Map map;
    uint32_t bs;
    uint32_t maxlen;
    uint32_t first;
    uint32_t tid;                               // numarul tranzactiei care ruleaza
    uint32_t start;                             // s_start (0 = jurnal gol)
    uint32_t incompat;
    bool v3;
    bool b64;
    uint32_t tagsz;
    uint32_t seed;
    uint8_t uuid[16];
    Slot slots[TX_CAP];
    uint32_t nslots;
    int16_t hash[HASH_SIZE];                    // index in slots + 1, 0 = liber
    uint64_t revokes[REV_CAP];
    uint32_t nrev;
    Jbd2Stats st;
};

namespace {

uint32_t be32(const uint8_t* p){
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

uint16_t be16(const uint8_t* p){
    return (uint16_t)(p[0] << 8 | p[1]);
}

void put32(uint8_t* p, uint32_t v){
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

void put16(uint8_t* p, uint16_t v){
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

uint32_t hash_of(uint64_t blk){
    return (uint32_t)((blk * 0x9E3779B97F4A7C15UL) >> 40) & (HASH_SIZE - 1);
}

void rehash(Journal* j){
    memset(j->hash, 0, sizeof(j->hash));
    for(uint32_t i = 0; i < j->nslots; i++){
        uint32_t h = hash_of(j->slots[i].blk);
        while(j->hash[h]){
            h = (h + 1) & (HASH_SIZE - 1);
        }
        j->hash[h] = (int16_t)(i + 1);
    }
}

Slot* find_slot(Journal* j, uint64_t blk){
    uint32_t h = hash_of(blk);
    while(j->hash[h]){
        Slot* s = &j->slots[j->hash[h] - 1];
        if(s->blk == blk){
            return s;
        }
        h = (h + 1) & (HASH_SIZE - 1);
    }
    return nullptr;
}

uint32_t wrap(Journal* j, uint32_t b){
    return b >= j->maxlen ? b - (j->maxlen - j->first) : b;
}

// Blocul jurnalului jblk, citit (pinned) sau nou si zerouit
Buf* jbuf(Journal* j, uint32_t jblk, bool read){
    uint64_t phys;
    if(jblk >= j->maxlen || j->map.map(j->map.ctx, jblk, phys) || phys == 0){
        return nullptr;
    }
    Buf* b = read ? bread(j->dev, phys) : bget(j->dev, phys);
    if(b && !read){
        memset(b->data, 0, j->bs);
        b->valid = true;
    }
    return b;
}

void tail_set(Journal* j, uint8_t* p){
    if(j->v3){
        put32(p + j->bs - 4, 0);
        put32(p + j->bs - 4, crc32c(j->seed, p, j->bs));
    }
}

bool tail_ok(Journal* j, const uint8_t* p){
    if(!j->v3){
        return true;
    }
    uint32_t want = be32(p + j->bs - 4);
    uint8_t* q = (uint8_t*)kmalloc(j->bs);
    if(!q){
        return false;
    }
    memcpy(q, p, j->bs);
    put32(q + j->bs - 4, 0);
    bool ok = crc32c(j->seed, q, j->bs) == want;
    kfree(q);
    return ok;
}

uint32_t tag_csum(Journal* j, uint32_t seq, const uint8_t* data){
    uint8_t s[4];
    put32(s, seq);
    return crc32c(crc32c(j->seed, s, 4), data, j->bs);
}

void header(uint8_t* p, uint32_t type, uint32_t seq){
    put32(p, MAGIC);
    put32(p + 4, type);
    put32(p + 8, seq);
}

int jsb_update(Journal* j, uint32_t start, uint32_t seq){
    Buf* b = jbuf(j, 0, true);
    if(!b){
        return -EIO;
    }
    put32(b->data + 24, seq);
    put32(b->data + 28, start);
    if(j->v3){
        put32(b->data + SB_CSUM_OFF, 0);
        put32(b->data + SB_CSUM_OFF, crc32c(~0u, b->data, SB_BYTES));
    }
    bdirty(b);
    brelse(b);
    j->start = start;
    return 0;
}

// ---- tag-uri ----

struct Tag {
    uint64_t blk;
    uint32_t flags;
    uint32_t csum;
};

// Tag-ul de la off in descriptorul p; intoarce lungimea tag-ului (cu uuid) sau 0 daca nu mai incape
uint32_t tag_read(Journal* j, const uint8_t* p, uint32_t off, uint32_t limit, Tag& t, bool first_after_uuid){
    (void)first_after_uuid;
    if(off + j->tagsz > limit){
        return 0;
    }
    const uint8_t* q = p + off;
    t.blk = be32(q);
    if(j->v3){
        t.flags = be32(q + 4);
        t.csum = be32(q + 12);
        t.blk |= (uint64_t)be32(q + 8) << 32;
    } else {
        t.csum = be16(q + 4);
        t.flags = be16(q + 6);
        if(j->b64){
            t.blk |= (uint64_t)be32(q + 8) << 32;
        }
    }
    uint32_t len = j->tagsz;
    if(!(t.flags & F_SAME_UUID)){
        len += 16;
    }
    return off + len > limit ? 0 : len;
}

void tag_write(Journal* j, uint8_t* q, uint64_t blk, uint32_t flags, uint32_t csum){
    put32(q, (uint32_t)blk);
    if(j->v3){
        put32(q + 4, flags);
        put32(q + 8, (uint32_t)(blk >> 32));
        put32(q + 12, csum);
    } else {
        put16(q + 4, 0);
        put16(q + 6, (uint16_t)flags);
        if(j->b64){
            put32(q + 8, (uint32_t)(blk >> 32));
        }
    }
}

void tag_add_flags(Journal* j, uint8_t* q, uint32_t f){
    if(j->v3){
        put32(q + 4, be32(q + 4) | f);
    } else {
        put16(q + 6, (uint16_t)(be16(q + 6) | f));
    }
}

// ---- tabela de revoke-uri la recuperare ----

struct RevTab {
    uint64_t* key;                              // bloc + 1, 0 = liber
    uint32_t* seq;
    uint32_t cap;
    uint32_t n;
};

bool rev_init(RevTab& t){
    t.cap = 256;
    t.n = 0;
    t.key = (uint64_t*)kzalloc(sizeof(uint64_t) * t.cap);
    t.seq = (uint32_t*)kzalloc(sizeof(uint32_t) * t.cap);
    return t.key && t.seq;
}

void rev_free(RevTab& t){
    kfree(t.key);
    kfree(t.seq);
}

uint32_t rev_slot(const RevTab& t, uint64_t blk){
    uint32_t h = (uint32_t)((blk * 0x9E3779B97F4A7C15UL) >> 36) & (t.cap - 1);
    while(t.key[h] && t.key[h] != blk + 1){
        h = (h + 1) & (t.cap - 1);
    }
    return h;
}

bool rev_add(RevTab& t, uint64_t blk, uint32_t seq){
    if((t.n + 1) * 2 > t.cap){
        RevTab nt;
        nt.cap = t.cap * 2;
        nt.n = 0;
        nt.key = (uint64_t*)kzalloc(sizeof(uint64_t) * nt.cap);
        nt.seq = (uint32_t*)kzalloc(sizeof(uint32_t) * nt.cap);
        if(!nt.key || !nt.seq){
            rev_free(nt);
            return false;
        }
        for(uint32_t i = 0; i < t.cap; i++){
            if(t.key[i]){
                uint32_t h = rev_slot(nt, t.key[i] - 1);
                nt.key[h] = t.key[i];
                nt.seq[h] = t.seq[i];
                nt.n++;
            }
        }
        rev_free(t);
        t = nt;
    }
    uint32_t h = rev_slot(t, blk);
    if(!t.key[h]){
        t.key[h] = blk + 1;
        t.seq[h] = seq;
        t.n++;
    } else if((int32_t)(seq - t.seq[h]) > 0){
        t.seq[h] = seq;
    }
    return true;
}

// Cel mai mare numar de tranzactie care revoca blocul, sau false
bool rev_find(const RevTab& t, uint64_t blk, uint32_t& seq){
    uint32_t h = rev_slot(t, blk);
    if(!t.key[h]){
        return false;
    }
    seq = t.seq[h];
    return true;
}

// Un pas peste jurnal: SCAN gaseste sfarsitul (end_seq = prima tranzactie necomisa), REVOKE aduna revoke-urile,
// REPLAY scrie blocurile. 0 sau -errno
int do_pass(Journal* j, int pass, uint32_t first_seq, uint32_t& end_seq, RevTab& rt){
    uint32_t blk = j->start;
    uint32_t next = first_seq;
    int rc = 0;
    for(;;){
        if(pass != PASS_SCAN && (int32_t)(next - end_seq) >= 0){
            break;
        }
        Buf* b = jbuf(j, blk, true);
        if(!b){
            rc = pass == PASS_SCAN ? 0 : -EIO;
            break;
        }
        const uint8_t* p = b->data;
        if(be32(p) != MAGIC || be32(p + 8) != next){
            brelse(b);
            rc = pass == PASS_SCAN ? 0 : -EIO;
            break;
        }
        uint32_t type = be32(p + 4);
        uint32_t limit = j->bs - (j->v3 ? 4 : 0);
        if(type == T_COMMIT){
            bool ok = true;
            if(j->v3){
                uint8_t* q = (uint8_t*)kmalloc(j->bs);
                ok = q != nullptr;
                if(q){
                    memcpy(q, p, j->bs);
                    put32(q + 16, 0);
                    ok = crc32c(j->seed, q, j->bs) == be32(p + 16);
                    kfree(q);
                }
            }
            brelse(b);
            if(!ok){
                rc = pass == PASS_SCAN ? 0 : -EIO;
                break;
            }
            next++;
            blk = wrap(j, blk + 1);
        } else if(type == T_REVOKE){
            bool ok = tail_ok(j, p);
            if(ok && pass == PASS_REVOKE){
                uint32_t used = be32(p + 12);
                uint32_t rs = j->b64 ? 8 : 4;
                if(used > limit){
                    used = limit;
                }
                for(uint32_t off = 16; off + rs <= used; off += rs){
                    uint64_t r = j->b64 ? (uint64_t)be32(p + off) << 32 | be32(p + off + 4) : be32(p + off);
                    if(!rev_add(rt, r, next)){
                        brelse(b);
                        return -ENOMEM;
                    }
                }
            }
            brelse(b);
            if(!ok){
                rc = pass == PASS_SCAN ? 0 : -EIO;
                break;
            }
            blk = wrap(j, blk + 1);
        } else if(type == T_DESC){
            if(!tail_ok(j, p)){
                brelse(b);
                rc = pass == PASS_SCAN ? 0 : -EIO;
                break;
            }
            // Descriptorul si blocurile lui de date raman pinned doar cat le procesam
            uint32_t off = 12;
            uint32_t k = 0;
            bool bad = false;
            for(;;){
                Tag t;
                uint32_t len = tag_read(j, p, off, limit, t, false);
                if(!len){
                    bad = true;                     // fara LAST_TAG in bloc: descriptor corupt
                    break;
                }
                if(pass != PASS_REVOKE){
                    Buf* d = jbuf(j, wrap(j, blk + 1 + k), true);
                    if(!d){
                        bad = true;
                        break;
                    }
                    if(j->v3 && tag_csum(j, next, d->data) != t.csum){
                        brelse(d);
                        bad = true;
                        break;
                    }
                    if(pass == PASS_REPLAY){
                        uint32_t rseq;
                        if(rev_find(rt, t.blk, rseq) && (int32_t)(rseq - next) >= 0){
                            j->st.revoked_skipped++;
                        } else {
                            Buf* w = bget(j->dev, t.blk);
                            if(!w){
                                brelse(d);
                                brelse(b);
                                return -EIO;
                            }
                            memcpy(w->data, d->data, j->bs);
                            if(t.flags & F_ESCAPE){
                                put32(w->data, MAGIC);
                            }
                            bdirty(w);
                            brelse(w);
                            j->st.replayed_blocks++;
                        }
                    }
                    brelse(d);
                }
                k++;
                off += len;
                if(t.flags & F_LAST){
                    break;
                }
            }
            brelse(b);
            if(bad){
                rc = pass == PASS_SCAN ? 0 : -EIO;
                break;
            }
            blk = wrap(j, blk + 1 + k);
        } else {
            brelse(b);
            rc = pass == PASS_SCAN ? 0 : -EIO;
            break;
        }
    }
    if(pass == PASS_SCAN){
        end_seq = next;
    }
    return rc;
}

// Descriptorul urmatorului grup de tag-uri nu mai incape sau s-au terminat blocurile: numarul de blocuri revoke
uint32_t revoke_blocks(Journal* j){
    if(!j->nrev){
        return 0;
    }
    uint32_t rs = j->b64 ? 8 : 4;
    uint32_t per = (j->bs - (j->v3 ? 4 : 0) - 16) / rs;
    return (j->nrev + per - 1) / per;
}

}

Journal* jbd2_open(BlockDevice* dev, const Jbd2Map& m, uint32_t fs_bs, int& err){
    Journal* j = (Journal*)kzalloc(sizeof(Journal));
    if(!j){
        err = -ENOMEM;
        return nullptr;
    }
    j->dev = dev;
    j->map = m;
    j->bs = fs_bs;
    j->maxlen = 0xFFFFFFFFu;                    // pana citim s_maxlen
    Buf* b = jbuf(j, 0, true);
    if(!b){
        kfree(j);
        err = -EIO;
        return nullptr;
    }
    const uint8_t* p = b->data;
    err = -EINVAL;
    uint32_t type = be32(p + 4);
    uint32_t inc = be32(p + 40);
    bool ok = be32(p) == MAGIC && type == T_SB2 && be32(p + 12) == fs_bs && be32(p + 64) <= 1;
    if(ok && (inc & ~(INC_REVOKE | INC_64BIT | INC_CSUM3))){
        kprintf("jbd2: trasaturi de jurnal nesuportate (incompat %x)\n", inc & ~(INC_REVOKE | INC_64BIT | INC_CSUM3));
        ok = false;
    }
    if(ok && (be32(p + 36) || be32(p + 44))){
        kprintf("jbd2: trasaturi compat / ro_compat nesuportate\n");     // ex. COMPAT_CHECKSUM (v1)
        ok = false;
    }
    if(ok){
        j->maxlen = be32(p + 16);
        j->first = be32(p + 20);
        j->tid = be32(p + 24);
        j->start = be32(p + 28);
        j->incompat = inc;
        j->v3 = (inc & INC_CSUM3) != 0;
        j->b64 = (inc & INC_64BIT) != 0;
        j->tagsz = j->v3 ? 16 : (j->b64 ? 12 : 8);
        memcpy(j->uuid, p + 48, 16);
        j->seed = crc32c(~0u, j->uuid, 16);
        ok = j->maxlen >= 64 && j->first >= 1 && j->first < j->maxlen && j->maxlen < 0x7FFFFFFFu;
        if(ok && j->v3){
            uint8_t* q = (uint8_t*)kmalloc(SB_BYTES);
            ok = q != nullptr;
            if(q){
                memcpy(q, p, SB_BYTES);
                put32(q + SB_CSUM_OFF, 0);
                ok = crc32c(~0u, q, SB_BYTES) == be32(p + SB_CSUM_OFF);
                kfree(q);
                if(!ok){
                    kprintf("jbd2: checksum-ul superblock-ului jurnalului e gresit\n");
                }
            }
        }
    }
    brelse(b);
    if(!ok){
        kfree(j);
        return nullptr;
    }
    err = 0;
    return j;
}

void jbd2_free(Journal* j){
    if(!j){
        return;
    }
    for(uint32_t i = 0; i < j->nslots; i++){
        brelse(j->slots[i].buf);
    }
    kfree(j);
}

bool jbd2_has_log(Journal* j){
    return j->start != 0;
}

int jbd2_recover(Journal* j){
    if(!j->start){
        return 0;
    }
    uint32_t first_seq = j->tid;
    uint32_t end_seq = first_seq;
    RevTab rt;
    if(!rev_init(rt)){
        rev_free(rt);
        return -ENOMEM;
    }
    int rc = do_pass(j, PASS_SCAN, first_seq, end_seq, rt);
    if(!rc){
        rc = do_pass(j, PASS_REVOKE, first_seq, end_seq, rt);
    }
    if(!rc){
        j->st.replayed_txns = end_seq - first_seq;
        rc = do_pass(j, PASS_REPLAY, first_seq, end_seq, rt);
    }
    rev_free(rt);
    if(!rc){
        rc = bsync(j->dev);
    }
    if(!rc){
        j->tid = end_seq + 1;
        rc = jsb_update(j, 0, j->tid);
        if(!rc){
            rc = bsync(j->dev);
        }
    }
    return rc;
}

uint8_t* jbd2_get(Journal* j, uint64_t blk, bool read){
    Slot* s = find_slot(j, blk);
    if(s){
        return s->buf->data;
    }
    if(j->nslots >= TX_CAP){
        return nullptr;
    }
    Buf* b = read ? bread(j->dev, blk) : bget(j->dev, blk);
    if(!b){
        return nullptr;
    }
    if(!read){
        memset(b->data, 0, j->bs);
        b->valid = true;
    }
    j->slots[j->nslots] = Slot{blk, b};
    j->nslots++;
    rehash(j);
    return b->data;
}

void jbd2_forget(Journal* j, uint64_t blk, bool revoke){
    Slot* s = find_slot(j, blk);
    if(s){
        s->buf->valid = false;                  // continutul modificat nu trebuie sa ramana in cache
        brelse(s->buf);
        *s = j->slots[--j->nslots];
        rehash(j);
    }
    if(revoke && j->nrev < REV_CAP){
        for(uint32_t i = 0; i < j->nrev; i++){
            if(j->revokes[i] == blk){
                return;
            }
        }
        j->revokes[j->nrev++] = blk;
    }
}

uint32_t jbd2_pending(Journal* j){
    return j->nslots;
}

bool jbd2_dirty(Journal* j){
    return j->nslots != 0 || j->nrev != 0;
}

uint32_t jbd2_capacity(Journal* j){
    (void)j;
    return TX_CAP;
}

uint32_t jbd2_sequence(Journal* j){
    return j->tid;
}

void jbd2_stats(Journal* j, Jbd2Stats& out){
    out = j->st;
    out.csum_v3 = j->v3;
}

int jbd2_commit(Journal* j){
    if(!jbd2_dirty(j)){
        return 0;
    }
    uint32_t bs = j->bs;
    uint32_t limit = bs - (j->v3 ? 4 : 0);
    uint32_t nrevb = revoke_blocks(j);
    uint32_t ndesc = (j->nslots * (j->tagsz + 1) + (limit - 12 - 16) - 1) / (limit - 12 - 16);
    if(j->nslots && !ndesc){
        ndesc = 1;
    }
    ndesc++;                                    // marja: primul tag are si uuid
    if(j->first + 1 + nrevb + ndesc + j->nslots > j->maxlen){
        kprintf("jbd2: tranzactia (%u blocuri) nu incape in jurnal\n", j->nslots);
        return -ENOSPC;
    }
    if(jbd2_crash_hook){
        jbd2_crash_hook(0);
    }
    // Ordered: datele (blocuri murdare, inclusiv ale fisierelor modificate in tranzactie) ajung pe disc inainte
    int rc = bsync(j->dev);
    if(rc){
        return rc;
    }
    uint32_t tid = j->tid;
    uint32_t pos = j->first;
    rc = jsb_update(j, j->first, tid);
    if(rc){
        return rc;
    }
    // Revoke
    uint32_t ri = 0;
    while(ri < j->nrev){
        Buf* rb = jbuf(j, pos++, false);
        if(!rb){
            return -EIO;
        }
        header(rb->data, T_REVOKE, tid);
        uint32_t off = 16;
        uint32_t rs = j->b64 ? 8 : 4;
        while(ri < j->nrev && off + rs <= limit){
            if(j->b64){
                put32(rb->data + off, (uint32_t)(j->revokes[ri] >> 32));
                put32(rb->data + off + 4, (uint32_t)j->revokes[ri]);
            } else {
                put32(rb->data + off, (uint32_t)j->revokes[ri]);
            }
            off += rs;
            ri++;
        }
        put32(rb->data + 12, off);
        tail_set(j, rb->data);
        bdirty(rb);
        brelse(rb);
    }
    // Descriptori + copiile metadatelor
    uint32_t i = 0;
    while(i < j->nslots){
        Buf* db = jbuf(j, pos++, false);
        if(!db){
            return -EIO;
        }
        uint8_t* p = db->data;
        header(p, T_DESC, tid);
        uint32_t off = 12;
        uint32_t last_tag = 0;
        bool first = true;
        while(i < j->nslots && off + j->tagsz + (first ? 16 : 0) <= limit){
            Slot* s = &j->slots[i];
            Buf* wb = jbuf(j, pos++, false);
            if(!wb){
                brelse(db);
                return -EIO;
            }
            memcpy(wb->data, s->buf->data, bs);
            uint32_t flags = first ? 0 : F_SAME_UUID;
            if(be32(wb->data) == MAGIC){
                put32(wb->data, 0);
                flags |= F_ESCAPE;
            }
            tag_write(j, p + off, s->blk, flags, j->v3 ? tag_csum(j, tid, wb->data) : 0);
            last_tag = off;
            off += j->tagsz;
            if(first){
                memcpy(p + off, j->uuid, 16);
                off += 16;
                first = false;
            }
            bdirty(wb);
            brelse(wb);
            i++;
        }
        tag_add_flags(j, p + last_tag, F_LAST);
        tail_set(j, p);
        bdirty(db);
        brelse(db);
    }
    rc = bsync(j->dev);
    if(rc){
        return rc;
    }
    j->st.blocks += j->nslots;
    j->st.revokes += j->nrev;
    if(jbd2_crash_hook){
        jbd2_crash_hook(1);
    }
    // Commit
    Buf* cb = jbuf(j, pos++, false);
    if(!cb){
        return -EIO;
    }
    header(cb->data, T_COMMIT, tid);
    Timespec now = vfs_now();
    put32(cb->data + 48, (uint32_t)((uint64_t)now.sec >> 32));
    put32(cb->data + 52, (uint32_t)now.sec);
    put32(cb->data + 56, (uint32_t)now.nsec);
    if(j->v3){
        cb->data[12] = 4;                       // JBD2_CRC32C_CHKSUM
        cb->data[13] = 4;
        put32(cb->data + 16, 0);
        put32(cb->data + 16, crc32c(j->seed, cb->data, bs));
    }
    bdirty(cb);
    brelse(cb);
    rc = bsync(j->dev);
    if(rc){
        return rc;
    }
    if(jbd2_crash_hook){
        jbd2_crash_hook(2);
    }
    // Checkpoint: blocurile reale, in doua jumatati (hook 3 = crash pe la jumatate)
    uint32_t half = j->nslots / 2;
    for(uint32_t k = 0; k < half; k++){
        bdirty(j->slots[k].buf);
    }
    rc = bsync(j->dev);
    if(jbd2_crash_hook){
        jbd2_crash_hook(3);
    }
    for(uint32_t k = half; k < j->nslots; k++){
        bdirty(j->slots[k].buf);
    }
    int rc2 = bsync(j->dev);
    rc = rc ? rc : rc2;
    for(uint32_t k = 0; k < j->nslots; k++){
        brelse(j->slots[k].buf);
    }
    j->nslots = 0;
    j->nrev = 0;
    rehash(j);
    if(jbd2_crash_hook){
        jbd2_crash_hook(4);
    }
    j->tid = tid + 1;
    j->st.commits++;
    if(rc){
        return rc;
    }
    rc = jsb_update(j, 0, j->tid);
    if(!rc){
        rc = bsync(j->dev);
    }
    return rc;
}
