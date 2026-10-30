#include "fs/ext4/ext4_priv.h"
#include "lib/string.h"
#include "lib/kprintf.h"
#include "uapi/syscall_nr.h"

// Scrierea arborelui de extente (T6.8). Radacina e in Node::blk (60 de octeti, 4 intrari), nodurile interioare in
// blocuri fs modificate prin tranzactie. Dupa orice modificare apelantul public scrie inode-ul (inode_store).

namespace ext4 {

namespace {

const uint16_t EXT_MAGIC = 0xF30A;
const int MAX_DEPTH = 5;
const uint32_t INIT_MAX = 32768;                // lungimea maxima a unui extent initializat

uint32_t eh_entries(const uint8_t* p){ return le16(p + 2); }
uint32_t eh_max(const uint8_t* p){ return le16(p + 4); }
uint32_t eh_depth(const uint8_t* p){ return le16(p + 6); }
uint8_t* ent(uint8_t* p, uint32_t i){ return p + 12 + 12 * i; }

uint32_t ee_start(const uint8_t* e){ return le32(e); }
uint32_t ee_rawlen(const uint8_t* e){ return le16(e + 4); }
uint32_t ee_len(const uint8_t* e){
    uint32_t l = ee_rawlen(e);
    return l > INIT_MAX ? l - INIT_MAX : l;
}
bool ee_uninit(const uint8_t* e){ return ee_rawlen(e) > INIT_MAX; }
uint64_t ee_phys(const uint8_t* e){ return (uint64_t)le16(e + 6) << 32 | le32(e + 8); }
void ee_set(uint8_t* e, uint32_t start, uint32_t rawlen, uint64_t phys){
    put32(e, start);
    put16(e + 4, rawlen);
    put16(e + 6, (uint32_t)(phys >> 32));
    put32(e + 8, (uint32_t)phys);
}
uint64_t ei_leaf(const uint8_t* e){ return (uint64_t)le16(e + 8) << 32 | le32(e + 4); }
void ei_set(uint8_t* e, uint32_t key, uint64_t leaf){
    put32(e, key);
    put32(e + 4, (uint32_t)leaf);
    put16(e + 8, (uint32_t)(leaf >> 32));
    put16(e + 10, 0);
}

void hdr_init(uint8_t* p, uint32_t entries, uint32_t max, uint32_t depth){
    put16(p, EXT_MAGIC);
    put16(p + 2, entries);
    put16(p + 4, max);
    put16(p + 6, depth);
    put32(p + 8, 0);
}

struct Path {
    uint8_t* node[MAX_DEPTH + 1];
    uint64_t blk[MAX_DEPTH + 1];            // 0 = radacina (in inode)
    int idx[MAX_DEPTH + 1];
    int depth;
};

uint32_t node_cap(Volume* v){
    return (v->bs - 12) / 12;
}

void node_done(Node* n, Path& p, int level){
    if(level > 0){
        extblock_csum_set(n, p.node[level]);
    }
}

// Coboara spre frunza pentru lblk (last = true: cea mai din dreapta). idx la frunza: ultima intrare cu start <= lblk
// (-1 daca nu exista)
int descend(Node* n, uint32_t lblk, bool last, Path& p){
    Volume* v = n->vol;
    uint8_t* cur = (uint8_t*)n->blk;
    uint64_t blk = 0;
    for(int level = 0; level <= MAX_DEPTH; level++){
        if(le16(cur) != EXT_MAGIC){
            return -EIO;
        }
        uint32_t entries = eh_entries(cur);
        uint32_t depth = eh_depth(cur);
        if(entries > eh_max(cur) || (level == 0 && eh_max(cur) > 4) || (level > 0 && eh_max(cur) > node_cap(v))){
            return -EIO;
        }
        if(level == 0){
            p.depth = (int)depth;
            if(depth > (uint32_t)MAX_DEPTH){
                return -EIO;
            }
        } else if(depth != (uint32_t)(p.depth - level)){
            return -EIO;
        }
        p.node[level] = cur;
        p.blk[level] = blk;
        int idx = -1;
        if(last){
            idx = (int)entries - 1;
        } else {
            for(uint32_t i = 0; i < entries && le32(ent(cur, i)) <= lblk; i++){
                idx = (int)i;
            }
        }
        if(level == p.depth){
            p.idx[level] = idx;
            return 0;
        }
        if(entries == 0){
            return -EIO;
        }
        if(idx < 0){
            idx = 0;
        }
        p.idx[level] = idx;
        uint64_t child = ei_leaf(ent(cur, (uint32_t)idx));
        cur = tx_get(v, child, true);
        if(!cur){
            return -ENOSPC;
        }
        blk = child;
    }
    return -EIO;
}

// Bloc nou pentru un nod al arborelui; header-ul se completeaza de apelant
int new_node(Node* n, uint8_t*& out, uint64_t& blk){
    Volume* v = n->vol;
    uint32_t got;
    int rc = balloc(v, (uint64_t)((n->ino->ino - 1) / v->ipg) * v->bpg + v->first_data_block, 1, blk, got);
    if(rc){
        return rc;
    }
    out = tx_get(v, blk, false);
    if(!out){
        bfree(v, blk, 1, true);
        return -ENOSPC;
    }
    n->blocks512 += v->bs >> 9;
    return 0;
}

// Cheia primei intrari a nodului de la `level` s-a schimbat: actualizeaza parintii
void fix_keys(Node* n, Path& p, int level, uint32_t key){
    for(int l = level; l > 0; l--){
        uint8_t* e = ent(p.node[l - 1], (uint32_t)p.idx[l - 1]);
        put32(e, key);
        node_done(n, p, l - 1);
        if(p.idx[l - 1] != 0){
            break;
        }
    }
}

int grow_root(Node* n){
    Volume* v = n->vol;
    uint8_t* root = (uint8_t*)n->blk;
    uint32_t depth = eh_depth(root);
    if(depth >= (uint32_t)MAX_DEPTH){
        return -ENOSPC;
    }
    uint8_t* b;
    uint64_t blk;
    int rc = new_node(n, b, blk);
    if(rc){
        return rc;
    }
    uint32_t entries = eh_entries(root);
    hdr_init(b, entries, node_cap(v), depth);
    memcpy(ent(b, 0), ent(root, 0), entries * 12);
    extblock_csum_set(n, b);
    uint32_t key = le32(ent(b, 0));
    hdr_init(root, 1, 4, depth + 1);
    memset(ent(root, 0), 0, 48);
    ei_set(ent(root, 0), key, blk);
    return 0;
}

int split_node(Node* n, Path& p, int l, uint32_t lblk){
    Volume* v = n->vol;
    uint8_t* node = p.node[l];
    uint8_t* parent = p.node[l - 1];
    uint32_t count = eh_entries(node);
    uint32_t mid = count / 2;
    if(l == p.depth && p.idx[l] == (int)count - 1){
        mid = count;                            // adaugare la sfarsit: frunza noua goala
    }
    uint8_t* b;
    uint64_t blk;
    int rc = new_node(n, b, blk);
    if(rc){
        return rc;
    }
    hdr_init(b, count - mid, node_cap(v), eh_depth(node));
    memcpy(ent(b, 0), ent(node, mid), (count - mid) * 12);
    put16(node + 2, mid);
    uint32_t key = mid < count ? le32(ent(b, 0)) : lblk;
    extblock_csum_set(n, b);
    node_done(n, p, l);
    // intrarea noua in parinte, dupa cea a nodului
    uint32_t pc = eh_entries(parent);
    uint32_t at = (uint32_t)p.idx[l - 1] + 1;
    memmove(ent(parent, at + 1), ent(parent, at), (pc - at) * 12);
    ei_set(ent(parent, at), key, blk);
    put16(parent + 2, pc + 1);
    node_done(n, p, l - 1);
    return 0;
}

int make_room(Node* n, Path& p, uint32_t lblk){
    int l = p.depth;
    while(l > 0 && eh_entries(p.node[l - 1]) >= eh_max(p.node[l - 1])){
        l--;
    }
    if(l == 0){
        return grow_root(n);
    }
    return split_node(n, p, l, lblk);
}

// Elimina intrarea idx din nodul de la `level`; daca nodul ramane gol si nu e radacina, se elibereaza si el
int remove_entry(Node* n, Path& p, int level, uint32_t idx){
    Volume* v = n->vol;
    uint8_t* node = p.node[level];
    uint32_t count = eh_entries(node);
    memmove(ent(node, idx), ent(node, idx + 1), (count - idx - 1) * 12);
    memset(ent(node, count - 1), 0, 12);
    put16(node + 2, count - 1);
    node_done(n, p, level);
    while(level > 0 && eh_entries(p.node[level]) == 0){
        uint64_t blk = p.blk[level];
        int rc = bfree(v, blk, 1, true);
        if(rc){
            return rc;
        }
        n->blocks512 -= v->bs >> 9;
        level--;
        uint8_t* par = p.node[level];
        uint32_t pi = (uint32_t)p.idx[level];
        uint32_t pc = eh_entries(par);
        memmove(ent(par, pi), ent(par, pi + 1), (pc - pi - 1) * 12);
        memset(ent(par, pc - 1), 0, 12);
        put16(par + 2, pc - 1);
        node_done(n, p, level);
    }
    if(level == 0 && eh_entries(p.node[0]) == 0){
        hdr_init(p.node[0], 0, 4, 0);           // arbore gol: frunza in radacina
    }
    return 0;
}

// Radacina cu un singur copil care incape in ea: copilul se aduce in inode
int collapse_root(Node* n){
    Volume* v = n->vol;
    uint8_t* root = (uint8_t*)n->blk;
    while(eh_depth(root) > 0 && eh_entries(root) == 1){
        uint64_t child = ei_leaf(ent(root, 0));
        uint8_t* c = tx_get(v, child, true);
        if(!c){
            return -ENOSPC;
        }
        uint32_t ce = eh_entries(c);
        if(le16(c) != EXT_MAGIC || ce > 4){
            break;
        }
        uint32_t cd = eh_depth(c);
        memset(ent(root, 0), 0, 48);
        memcpy(ent(root, 0), ent(c, 0), ce * 12);
        hdr_init(root, ce, 4, cd);
        int rc = bfree(v, child, 1, true);
        if(rc){
            return rc;
        }
        n->blocks512 -= v->bs >> 9;
    }
    return 0;
}

}

void ext_init_root(uint32_t* blk15){
    memset(blk15, 0, 60);
    hdr_init((uint8_t*)blk15, 0, 4, 0);
}

int ext_insert(Node* n, uint32_t lblk, uint64_t phys, uint32_t len){
    for(int attempt = 0; attempt < 32; attempt++){
        Path p;
        int rc = descend(n, lblk, false, p);
        if(rc){
            return rc;
        }
        int D = p.depth;
        uint8_t* leaf = p.node[D];
        uint32_t entries = eh_entries(leaf);
        int i = p.idx[D];
        if(i >= 0){
            uint8_t* e = ent(leaf, (uint32_t)i);
            uint64_t end = (uint64_t)ee_start(e) + ee_len(e);
            if(lblk < end){
                return -EOPNOTSUPP;                         // suprapunere (extent neinitializat?)
            }
            if(!ee_uninit(e) && end == lblk && ee_phys(e) + ee_len(e) == phys && ee_len(e) + len <= INIT_MAX){
                put16(e + 4, ee_len(e) + len);
                node_done(n, p, D);
                return inode_store(n);
            }
        }
        if(i + 1 < (int)entries){
            uint8_t* nx = ent(leaf, (uint32_t)i + 1);
            if((uint64_t)lblk + len > ee_start(nx)){
                return -EOPNOTSUPP;
            }
            if(!ee_uninit(nx) && ee_start(nx) == lblk + len && ee_phys(nx) == phys + len &&
               ee_len(nx) + len <= INIT_MAX){
                ee_set(nx, lblk, ee_len(nx) + len, phys);
                if(i + 1 == 0){
                    fix_keys(n, p, D, lblk);
                }
                node_done(n, p, D);
                return inode_store(n);
            }
        }
        if(entries < eh_max(leaf)){
            uint32_t at = (uint32_t)i + 1;
            memmove(ent(leaf, at + 1), ent(leaf, at), (entries - at) * 12);
            ee_set(ent(leaf, at), lblk, len, phys);
            put16(leaf + 2, entries + 1);
            if(at == 0 && D > 0){
                fix_keys(n, p, D, lblk);
            }
            node_done(n, p, D);
            return inode_store(n);
        }
        rc = make_room(n, p, lblk);
        if(rc){
            return rc;
        }
    }
    return -EIO;
}

int ext_truncate(Node* n, uint32_t from, uint32_t budget, bool& more){
    Volume* v = n->vol;
    more = false;
    int rc = 0;
    uint32_t steps = 0;
    bool md = S_ISDIR(n->ino->mode) || S_ISLNK(n->ino->mode);       // blocurile directoarelor / symlink-urilor sunt metadate
    for(;;){
        Path p;
        rc = descend(n, 0, true, p);
        if(rc){
            return rc;
        }
        int D = p.depth;
        uint8_t* leaf = p.node[D];
        uint32_t entries = eh_entries(leaf);
        if(entries == 0){
            if(D == 0){
                break;
            }
            if(steps >= budget){
                more = true;
                break;
            }
            rc = remove_entry(n, p, D, 0);          // frunza goala (ramasa dintr-un pas anterior)
            steps++;
            if(rc){
                return rc;
            }
            continue;
        }
        uint8_t* e = ent(leaf, entries - 1);
        uint64_t start = ee_start(e);
        uint32_t len = ee_len(e);
        if(start + len <= from){
            break;
        }
        if(steps >= budget){
            more = true;
            break;
        }
        steps++;
        uint64_t phys = ee_phys(e);
        if(start >= from){
            rc = bfree(v, phys, len, md);
            if(rc){
                return rc;
            }
            n->blocks512 -= (uint64_t)len * (v->bs >> 9);
            rc = remove_entry(n, p, D, entries - 1);
            if(rc){
                return rc;
            }
        } else {
            uint32_t keep = from - (uint32_t)start;
            rc = bfree(v, phys + keep, len - keep, md);
            if(rc){
                return rc;
            }
            n->blocks512 -= (uint64_t)(len - keep) * (v->bs >> 9);
            put16(e + 4, keep + (ee_uninit(e) ? INIT_MAX : 0));
            node_done(n, p, D);
        }
    }
    if(!more){
        rc = collapse_root(n);
        if(rc){
            return rc;
        }
    }
    return inode_store(n);
}

int file_alloc_range(Node* n, uint32_t lblk, uint32_t want, uint64_t& first, uint32_t& got){
    Volume* v = n->vol;
    if(want > INIT_MAX){
        want = INIT_MAX;
    }
    uint64_t goal = 0;
    if(lblk > 0){
        uint64_t phys, run;
        if(map_block(n, lblk - 1, 1, phys, run) == 0 && phys){
            goal = phys + 1;
        }
    }
    if(!goal){
        goal = (uint64_t)((n->ino->ino - 1) / v->ipg) * v->bpg + v->first_data_block;
    }
    int rc = balloc(v, goal, want, first, got);
    if(rc){
        return rc;
    }
    rc = ext_insert(n, lblk, first, got);
    if(rc){
        bfree(v, first, got, false);
        return rc;
    }
    n->blocks512 += (uint64_t)got * (v->bs >> 9);
    return inode_store(n);
}

namespace {

int free_indirect(Node* n, uint32_t blk, int level){
    Volume* v = n->vol;
    Buf* b;
    uint8_t* t = fsblk_get(v, blk, b);
    if(!t){
        return -EIO;
    }
    int rc = 0;
    for(uint32_t i = 0; i < v->bs / 4 && !rc; i++){
        uint32_t e = le32(t + 4 * i);
        if(!e){
            continue;
        }
        if(level == 1){
            rc = bfree(v, e, 1, false);
            n->blocks512 -= v->bs >> 9;
        } else {
            rc = free_indirect(n, e, level - 1);
        }
    }
    brelse(b);
    if(!rc){
        rc = bfree(v, blk, 1, true);
        n->blocks512 -= v->bs >> 9;
    }
    return rc;
}

}

int bmap_free_all(Node* n){
    Volume* v = n->vol;
    int rc = 0;
    for(uint32_t i = 0; i < 15 && !rc; i++){
        uint32_t e = n->blk[i];
        if(!e){
            continue;
        }
        if(i < 12){
            rc = bfree(v, e, 1, false);
            n->blocks512 -= v->bs >> 9;
        } else {
            rc = free_indirect(n, e, (int)(i - 11));
        }
        n->blk[i] = 0;
    }
    return rc ? rc : inode_store(n);
}

}
