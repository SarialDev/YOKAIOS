#include "fs/fat32/fat32_priv.h"
#include "drivers/rtc.h"
#include "lib/string.h"
#include "uapi/syscall_nr.h"

namespace fat32 {

// ---- nume: UTF-8 <-> UTF-16, comparatie fara diferenta de majuscule ----

namespace {

uint16_t fold(uint16_t c){
    if(c < 0x80){
        return c >= 'a' && c <= 'z' ? (uint16_t)(c - 32) : c;
    }
    if((c >= 0xE0 && c <= 0xFE && c != 0xF7) || (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) || (c >= 0x430 && c <= 0x44F)){
        return (uint16_t)(c - 32);
    }
    if(c >= 0x450 && c <= 0x45F){
        return (uint16_t)(c - 80);
    }
    // Latin extins A (ă â î ș ț ...): perechile majuscula / minuscula alterneaza
    if((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177) || (c >= 0x218 && c <= 0x21B)){
        return c & 1 ? (uint16_t)(c - 1) : c;
    }
    if((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)){
        return c & 1 ? c : (uint16_t)(c - 1);
    }
    return c;
}

bool short_char_ok(uint16_t c){
    if(c >= 'A' && c <= 'Z'){
        return true;
    }
    if(c >= '0' && c <= '9'){
        return true;
    }
    switch(c){
        case '!': case '#': case '$': case '%': case '&': case '\'': case '(': case ')': case '-': case '@':
        case '^': case '_': case '`': case '{': case '}': case '~':
            return true;
        default:
            return false;
    }
}

uint8_t lfn_checksum(const uint8_t* name11){
    uint8_t sum = 0;
    for(int i = 0; i < 11; i++){
        sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + name11[i]);
    }
    return sum;
}

uint16_t le16(const uint8_t* p){
    return (uint16_t)(p[0] | p[1] << 8);
}

void put16(uint8_t* p, uint16_t v){
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

// Pozitiile celor 13 caractere dintr-o intrare de nume lung
const uint8_t LFN_POS[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};

// Numele 8.3 afisat: baza + "." + extensia, cu minuscule dupa bitii NT (0x08 baza, 0x10 extensia)
uint32_t short_display(const uint8_t* raw, uint16_t* out){
    uint32_t n = 0;
    uint8_t nt = raw[12];
    for(int i = 0; i < 8 && raw[i] != ' '; i++){
        uint16_t c = raw[i];
        if(i == 0 && c == 0x05){
            c = 0xE5;
        }
        if((nt & 0x08) && c >= 'A' && c <= 'Z'){
            c = (uint16_t)(c + 32);
        }
        out[n++] = c;
    }
    if(raw[8] != ' '){
        out[n++] = '.';
        for(int i = 8; i < 11 && raw[i] != ' '; i++){
            uint16_t c = raw[i];
            if((nt & 0x10) && c >= 'A' && c <= 'Z'){
                c = (uint16_t)(c + 32);
            }
            out[n++] = c;
        }
    }
    return n;
}

// Numele se potriveste exact intr-un 8.3 (cu cele doua bitui de minuscule NT)? Completeaza name11 si nt
bool short_exact(const uint16_t* name, uint32_t n, uint8_t* name11, uint8_t& nt){
    int dot = -1;
    for(uint32_t i = 0; i < n; i++){
        if(name[i] == '.'){
            dot = (int)i;
        }
    }
    if(dot == 0 || dot == (int)n - 1){
        return false;
    }
    uint32_t base_len = dot < 0 ? n : (uint32_t)dot;
    uint32_t ext_len = dot < 0 ? 0 : n - (uint32_t)dot - 1;
    if(base_len == 0 || base_len > 8 || ext_len > 3){
        return false;
    }
    bool lower[2] = {false, false};
    bool upper[2] = {false, false};
    memset(name11, ' ', 11);
    for(uint32_t i = 0; i < n; i++){
        if((int)i == dot){
            continue;
        }
        bool in_ext = dot >= 0 && (int)i > dot;
        uint16_t c = name[i];
        if(c >= 'a' && c <= 'z'){
            lower[in_ext] = true;
            c = (uint16_t)(c - 32);
        } else if(c >= 'A' && c <= 'Z'){
            upper[in_ext] = true;
        }
        if(!short_char_ok(c)){
            return false;
        }
        uint32_t pos = in_ext ? 8 + (i - (uint32_t)dot - 1) : i;
        name11[pos] = (uint8_t)c;
    }
    if((lower[0] && upper[0]) || (lower[1] && upper[1])){
        return false;
    }
    nt = (uint8_t)((lower[0] ? 0x08 : 0) | (lower[1] ? 0x10 : 0));
    return true;
}

// Numele scurt generat pentru un nume lung: baza (cel mult 8 - tail) + "~N" + extensia. lossy = diferit de nume
// in afara de majuscule
void short_basis(const uint16_t* name, uint32_t n, uint8_t* base8, uint32_t& base_len, uint8_t* ext3, bool& lossy){
    lossy = false;
    uint32_t start = 0;
    while(start < n && name[start] == '.'){
        start++;
        lossy = true;
    }
    int dot = -1;
    for(uint32_t i = start; i < n; i++){
        if(name[i] == '.'){
            dot = (int)i;
        }
    }
    uint32_t base_end = dot < 0 ? n : (uint32_t)dot;
    base_len = 0;
    memset(ext3, ' ', 3);
    for(uint32_t i = start; i < base_end; i++){
        uint16_t c = name[i];
        if(c == ' ' || c == '.'){
            lossy = true;
            continue;
        }
        if(c >= 'a' && c <= 'z'){
            c = (uint16_t)(c - 32);
        }
        if(!short_char_ok(c)){
            c = '_';
            lossy = true;
        }
        if(base_len == 8){
            lossy = true;
            continue;
        }
        base8[base_len++] = (uint8_t)c;
    }
    if(dot >= 0){
        uint32_t e = 0;
        for(uint32_t i = (uint32_t)dot + 1; i < n; i++){
            uint16_t c = name[i];
            if(c == ' '){
                lossy = true;
                continue;
            }
            if(c >= 'a' && c <= 'z'){
                c = (uint16_t)(c - 32);
            }
            if(!short_char_ok(c)){
                c = '_';
                lossy = true;
            }
            if(e == 3){
                lossy = true;
                continue;
            }
            ext3[e++] = (uint8_t)c;
        }
    }
    if(base_len == 0){
        base8[base_len++] = '_';
        lossy = true;
    }
}


}

int utf8_to_utf16(const char* s, uint32_t len, uint16_t* out, uint32_t cap, uint32_t& n){
    n = 0;
    for(uint32_t i = 0; i < len;){
        uint8_t b = (uint8_t)s[i];
        uint32_t cp;
        uint32_t extra;
        if(b < 0x80){
            cp = b;
            extra = 0;
        } else if((b & 0xE0) == 0xC0){
            cp = b & 0x1F;
            extra = 1;
        } else if((b & 0xF0) == 0xE0){
            cp = b & 0x0F;
            extra = 2;
        } else if((b & 0xF8) == 0xF0){
            cp = b & 0x07;
            extra = 3;
        } else {
            return -EINVAL;
        }
        for(uint32_t k = 1; k <= extra; k++){
            if(i + k >= len || ((uint8_t)s[i + k] & 0xC0) != 0x80){
                return -EINVAL;
            }
            cp = cp << 6 | ((uint8_t)s[i + k] & 0x3F);
        }
        i += extra + 1;
        // Codificari prea lungi, surogate izolate si valori in afara Unicode
        if((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) || (extra == 3 && cp < 0x10000) ||
           (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF){
            return -EINVAL;
        }
        if(cp >= 0x10000){
            if(n + 2 > cap){
                return -ENAMETOOLONG;
            }
            cp -= 0x10000;
            out[n++] = (uint16_t)(0xD800 | (cp >> 10));
            out[n++] = (uint16_t)(0xDC00 | (cp & 0x3FF));
        } else {
            if(n + 1 > cap){
                return -ENAMETOOLONG;
            }
            out[n++] = (uint16_t)cp;
        }
    }
    return 0;
}

// Lungimea in octeti, sau 0xFFFFFFFF daca nu incape in cap sau numele nu e UTF-16 valid
uint32_t utf16_to_utf8(const uint16_t* s, uint32_t n, char* out, uint32_t cap){
    uint32_t o = 0;
    for(uint32_t i = 0; i < n; i++){
        uint32_t cp = s[i];
        if(cp >= 0xD800 && cp <= 0xDBFF){
            if(i + 1 >= n || s[i + 1] < 0xDC00 || s[i + 1] > 0xDFFF){
                return 0xFFFFFFFFu;
            }
            cp = 0x10000 + ((cp - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            i++;
        } else if(cp >= 0xDC00 && cp <= 0xDFFF){
            return 0xFFFFFFFFu;
        }
        uint32_t need = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
        if(o + need > cap){
            return 0xFFFFFFFFu;
        }
        if(need == 1){
            out[o++] = (char)cp;
        } else if(need == 2){
            out[o++] = (char)(0xC0 | (cp >> 6));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        } else if(need == 3){
            out[o++] = (char)(0xE0 | (cp >> 12));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        } else {
            out[o++] = (char)(0xF0 | (cp >> 18));
            out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    return o;
}

// Nume permis la creare: nevid, fara caractere interzise, fara punct sau spatiu la sfarsit, nu "." / ".."
bool name_valid(const uint16_t* name, uint32_t n){
    if(n == 0 || n > NAME16_MAX){
        return false;
    }
    if(name[n - 1] == '.' || name[n - 1] == ' '){
        return false;
    }
    for(uint32_t i = 0; i < n; i++){
        uint16_t c = name[i];
        if(c < 0x20 || c == '"' || c == '*' || c == '/' || c == ':' || c == '<' || c == '>' || c == '?' ||
           c == '\\' || c == '|'){
            return false;
        }
    }
    return true;
}

bool name_equal(const uint16_t* a, uint32_t na, const uint16_t* b, uint32_t nb){
    if(na != nb){
        return false;
    }
    for(uint32_t i = 0; i < na; i++){
        if(a[i] != b[i] && fold(a[i]) != fold(b[i])){
            return false;
        }
    }
    return true;
}

// ---- timp: date = (an - 1980) << 9 | luna << 5 | zi; time = ora << 11 | minut << 5 | secunda / 2 ----

void fat_time_encode(int64_t epoch, uint16_t& date, uint16_t& time){
    RtcTime t = rtc_from_epoch(epoch < 0 ? 0 : (uint64_t)epoch);
    if(t.year < 1980){
        date = 1 << 5 | 1;
        time = 0;
        return;
    }
    if(t.year > 2107){
        t.year = 2107;
    }
    date = (uint16_t)((t.year - 1980) << 9 | t.month << 5 | t.day);
    time = (uint16_t)(t.hour << 11 | t.minute << 5 | t.second / 2);
}

Timespec fat_time_decode(uint16_t date, uint16_t time){
    RtcTime t = {};
    t.year = (uint16_t)(1980 + (date >> 9));
    t.month = (uint8_t)((date >> 5) & 15);
    t.day = (uint8_t)(date & 31);
    t.hour = (uint8_t)(time >> 11);
    t.minute = (uint8_t)((time >> 5) & 63);
    t.second = (uint8_t)((time & 31) * 2);
    if(t.month < 1 || t.month > 12 || t.day < 1 || t.hour > 23 || t.minute > 59 || t.second > 59){
        return Timespec{0, 0};
    }
    return Timespec{(int64_t)rtc_to_epoch(t), 0};
}

// ---- fluxul de intrari al unui director ----

void it_init(DirIter& it, Volume* v, uint32_t first){
    it.v = v;
    it.first = first;
    it.idx = 0;
    it.clu = 0;
}

namespace {

// Offset-ul de volum al intrarii de la off; 1 daca lantul se termina inainte
int it_phys(DirIter& it, uint32_t off, uint64_t& phys){
    Volume* v = it.v;
    if(off >= DIR_MAX_BYTES){
        return 1;
    }
    uint32_t ci = off / v->cluster_bytes;
    if(it.clu == 0 || it.idx > ci){
        if(!clu_valid(v, it.first)){
            return -EIO;
        }
        it.idx = 0;
        it.clu = it.first;
    }
    while(it.idx < ci){
        uint32_t nx;
        int err = fat_next(v, it.clu, nx);
        if(err){
            return err;
        }
        if(!nx){
            return 1;
        }
        it.clu = nx;
        it.idx++;
    }
    phys = clu_offset(v, it.clu) + off % v->cluster_bytes;
    return 0;
}

}

int dir_read_raw(DirIter& it, uint32_t off, uint8_t* e){
    uint64_t phys;
    int rc = it_phys(it, off, phys);
    if(rc){
        return rc;
    }
    return vol_rw(it.v, phys, e, ENT_SIZE, false);
}

int dir_write_raw(DirIter& it, uint32_t off, const uint8_t* e){
    uint64_t phys;
    int rc = it_phys(it, off, phys);
    if(rc > 0){
        return -EIO;
    }
    if(rc < 0){
        return rc;
    }
    return vol_rw(it.v, phys, (void*)e, ENT_SIZE, true);
}

int dir_next(DirIter& it, uint32_t& off, Entry& e){
    uint16_t lbuf[20 * 13];
    bool lvalid = false;
    uint8_t expect = 0;
    uint8_t chk = 0;
    uint32_t lstart = 0;
    uint32_t ltotal = 0;
    for(;;){
        uint8_t r[ENT_SIZE];
        int rc = dir_read_raw(it, off, r);
        if(rc < 0){
            return rc;
        }
        if(rc == 1 || r[0] == 0x00){
            return 0;
        }
        uint32_t here = off;
        off += ENT_SIZE;
        if(r[0] == 0xE5){
            lvalid = false;
            continue;
        }
        if(r[11] == ATTR_LFN){
            uint8_t n = r[0] & 0x3F;
            if(r[0] & 0x40){
                if(n < 1 || n > 20){
                    lvalid = false;
                    continue;
                }
                lvalid = true;
                ltotal = n;
                expect = n;
                chk = r[13];
                lstart = here;
                for(uint32_t i = 0; i < 20 * 13; i++){
                    lbuf[i] = 0xFFFF;
                }
            }
            if(!lvalid || n != expect || r[13] != chk){
                lvalid = false;
                continue;
            }
            for(uint32_t k = 0; k < 13; k++){
                lbuf[(n - 1) * 13 + k] = le16(r + LFN_POS[k]);
            }
            expect = (uint8_t)(n - 1);
            continue;
        }
        if(r[11] & ATTR_VOLID){
            lvalid = false;
            continue;
        }
        memcpy(e.raw, r, ENT_SIZE);
        e.alen = short_display(r, e.alias);
        e.has_lfn = lvalid && expect == 0 && lfn_checksum(r) == chk;
        if(e.has_lfn){
            uint32_t n = 0;
            while(n < ltotal * 13 && lbuf[n] != 0){
                n++;
            }
            if(n > NAME16_MAX){
                n = NAME16_MAX;
            }
            memcpy(e.name, lbuf, n * 2);
            e.nlen = n;
            e.lfn_off = lstart;
        } else {
            memcpy(e.name, e.alias, e.alen * 2);
            e.nlen = e.alen;
            e.lfn_off = here;
        }
        e.ent_off = here;
        return 1;
    }
}

int dir_find(Volume* v, uint32_t dir_first, const uint16_t* name, uint32_t n, Entry& e){
    DirIter it;
    it_init(it, v, dir_first);
    uint32_t off = 0;
    int rc;
    while((rc = dir_next(it, off, e)) == 1){
        if(name_equal(e.name, e.nlen, name, n) || (e.has_lfn && name_equal(e.alias, e.alen, name, n))){
            return 0;
        }
    }
    return rc < 0 ? rc : -ENOENT;
}

int dir_empty(Volume* v, uint32_t dir_first){
    DirIter it;
    it_init(it, v, dir_first);
    uint32_t off = 0;
    Entry e;
    int rc;
    while((rc = dir_next(it, off, e)) == 1){
        bool dot = e.raw[0] == '.' && !e.has_lfn;
        if(!dot){
            return 0;
        }
    }
    return rc < 0 ? rc : 1;
}

namespace {

bool short_in_use(Volume* v, uint32_t dir_first, const uint8_t* name11){
    DirIter it;
    it_init(it, v, dir_first);
    uint32_t off = 0;
    uint8_t r[ENT_SIZE];
    for(;;){
        int rc = dir_read_raw(it, off, r);
        if(rc != 0 || r[0] == 0x00){
            return false;
        }
        if(r[0] != 0xE5 && r[11] != ATTR_LFN && memcmp(r, name11, 11) == 0){
            return true;
        }
        off += ENT_SIZE;
    }
}

}

int dir_add(Volume* v, uint32_t dir_first, const uint16_t* name, uint32_t n, uint8_t* raw, uint32_t& lfn_off,
            uint32_t& ent_off){
    uint8_t name11[11];
    uint8_t nt = 0;
    bool need_lfn = !short_exact(name, n, name11, nt);
    if(need_lfn){
        uint8_t base[8];
        uint32_t base_len;
        uint8_t ext[3];
        bool lossy;
        short_basis(name, n, base, base_len, ext, lossy);
        bool found = false;
        for(uint32_t tail = lossy ? 1 : 0; tail < 1000000 && !found; tail++){
            char tl_buf[8];
            uint32_t tl = 0;
            if(tail){
                char tmp[8];
                uint32_t d = 0;
                for(uint32_t t = tail; t; t /= 10){
                    tmp[d++] = (char)('0' + t % 10);
                }
                tl_buf[tl++] = '~';
                while(d){
                    tl_buf[tl++] = tmp[--d];
                }
            }
            uint32_t keep = base_len;
            if(keep + tl > 8){
                keep = 8 - tl;
            }
            memset(name11, ' ', 11);
            memcpy(name11, base, keep);
            memcpy(name11 + keep, tl_buf, tl);
            memcpy(name11 + 8, ext, 3);
            found = !short_in_use(v, dir_first, name11);
        }
        if(!found){
            return -EEXIST;
        }
        nt = 0;
    }
    if(name11[0] == 0xE5){
        name11[0] = 0x05;
    }
    uint32_t slots = (need_lfn ? (n + 12) / 13 : 0) + 1;

    // Prima serie de `slots` intrari libere consecutive; dupa prima intrare 0x00 tot restul directorului e liber
    DirIter it;
    it_init(it, v, dir_first);
    uint32_t off = 0;
    uint32_t run_start = 0;
    uint32_t run_len = 0;
    for(;;){
        uint8_t r[ENT_SIZE];
        int rc = dir_read_raw(it, off, r);
        if(rc < 0){
            return rc;
        }
        if(rc == 1){
            if(!run_len){
                run_start = off;
            }
            break;
        }
        if(r[0] == 0x00){
            if(!run_len){
                run_start = off;
            }
            break;
        }
        if(r[0] == 0xE5){
            if(!run_len){
                run_start = off;
            }
            run_len++;
            if(run_len == slots){
                break;
            }
        } else {
            run_len = 0;
        }
        off += ENT_SIZE;
    }
    uint32_t end = run_start + slots * ENT_SIZE;
    if(end > DIR_MAX_BYTES){
        return -ENOSPC;
    }
    // Directorul se extinde cu clustere zerorizate pana cand ultima intrare incape
    for(;;){
        uint64_t phys;
        int rc = it_phys(it, end - ENT_SIZE, phys);
        if(rc < 0){
            return rc;
        }
        if(rc == 0){
            break;
        }
        uint32_t nc;
        int err = clu_alloc(v, it.clu, nc);
        if(err){
            return err;
        }
        it.clu = nc;
        it.idx++;
    }

    memcpy(raw, name11, 11);
    raw[12] = nt;
    uint32_t pos = run_start;
    if(need_lfn){
        uint8_t chk = lfn_checksum(name11);
        uint32_t cnt = slots - 1;
        for(uint32_t ord = cnt; ord >= 1; ord--){
            uint8_t l[ENT_SIZE];
            memset(l, 0, sizeof(l));
            l[0] = (uint8_t)(ord | (ord == cnt ? 0x40 : 0));
            l[11] = ATTR_LFN;
            l[13] = chk;
            for(uint32_t k = 0; k < 13; k++){
                uint32_t p = (ord - 1) * 13 + k;
                put16(l + LFN_POS[k], p < n ? name[p] : p == n ? 0x0000 : 0xFFFF);
            }
            int err = dir_write_raw(it, pos, l);
            if(err){
                return err;
            }
            pos += ENT_SIZE;
        }
    }
    int err = dir_write_raw(it, pos, raw);
    if(err){
        return err;
    }
    lfn_off = run_start;
    ent_off = pos;
    return 0;
}

int dir_save_marks(Volume* v, uint32_t dir_first, uint32_t lfn_off, uint32_t ent_off, uint8_t* saved){
    DirIter it;
    it_init(it, v, dir_first);
    for(uint32_t off = lfn_off, i = 0; off <= ent_off; off += ENT_SIZE, i++){
        uint8_t r[ENT_SIZE];
        int rc = dir_read_raw(it, off, r);
        if(rc != 0){
            return rc ? rc : -EIO;
        }
        saved[i] = r[0];
    }
    return 0;
}

int dir_delete(Volume* v, uint32_t dir_first, uint32_t lfn_off, uint32_t ent_off){
    DirIter it;
    it_init(it, v, dir_first);
    for(uint32_t off = lfn_off; off <= ent_off; off += ENT_SIZE){
        uint8_t r[ENT_SIZE];
        int rc = dir_read_raw(it, off, r);
        if(rc != 0){
            return rc ? rc : -EIO;
        }
        r[0] = 0xE5;
        rc = dir_write_raw(it, off, r);
        if(rc){
            return rc;
        }
    }
    return 0;
}

int dir_undelete(Volume* v, uint32_t dir_first, uint32_t lfn_off, uint32_t ent_off, const uint8_t* saved){
    DirIter it;
    it_init(it, v, dir_first);
    for(uint32_t off = lfn_off, i = 0; off <= ent_off; off += ENT_SIZE, i++){
        uint8_t r[ENT_SIZE];
        int rc = dir_read_raw(it, off, r);
        if(rc != 0){
            return rc ? rc : -EIO;
        }
        r[0] = saved[i];
        rc = dir_write_raw(it, off, r);
        if(rc){
            return rc;
        }
    }
    return 0;
}

}
