/* ypkg: gestionar de pachete .yokai (vezi docs/PACKAGE_FORMAT.md) */
#ifndef YPKG_H
#define YPKG_H

#include <stddef.h>
#include <stdint.h>

/* ---- SHA-256 ---- */
typedef struct {
    uint32_t h[8];
    uint64_t len;
    unsigned char buf[64];
    size_t n;
} Sha256;

void sha256_init(Sha256 *s);
void sha256_update(Sha256 *s, const void *data, size_t len);
void sha256_final(Sha256 *s, char hex[65]);

/* ---- manifest.toml ---- */
typedef struct {
    char *path;          /* relativ la radacina, fara '/' la inceput */
    char sha256[65];     /* hex minuscule */
    unsigned mode;       /* permisiuni (& 0777) */
    long long size;
} PkgFile;

typedef struct {
    long format;
    char *name, *version, *description, *entry, *signature;
    char **deps;  int ndeps;
    char **perms; int nperms;
    PkgFile *files; int nfiles;   /* sortat dupa path */
} Manifest;

/* 0 = ok; -1 = eroare (mesaj in err). Textul nu trebuie sa fie terminat cu 0 */
int manifest_parse(const char *text, size_t len, Manifest *m, char *err, size_t errsz);
void manifest_free(Manifest *m);
const PkgFile *manifest_find(const Manifest *m, const char *path);

/* name [op version] -> nume (in name), op ("" daca lipseste), versiune. 0 = ok */
int dep_split(const char *spec, char *name, size_t nsz, char *op, char *ver, size_t vsz);
int version_cmp(const char *a, const char *b);
int version_ok(const char *have, const char *op, const char *want);
int valid_name(const char *s);
int valid_version(const char *s);

/* ---- tar (ustar) ---- */
typedef struct {
    char name[512];
    char type;           /* '0' fisier, '5' director, altele = nesuportate */
    unsigned mode;
    long long size;
} TarEnt;

/* 1 = intrare, 0 = sfarsit de arhiva, -1 = eroare (err). Lasa fd la inceputul datelor */
int tar_next(int fd, TarEnt *e, char *err, size_t errsz);
/* sare peste date + padding (cand datele nu au fost citite) */
int tar_skip_data(int fd, const TarEnt *e);
/* citeste e->size octeti: hash (daca sha) si/sau scriere in outfd (daca >= 0), apoi sare padding-ul. 0 = ok */
int tar_stream(int fd, const TarEnt *e, Sha256 *sha, int outfd);
/* curata numele (./, / final) si valideaza calea; 0 = ok */
int path_normalize(char *p);

#endif
