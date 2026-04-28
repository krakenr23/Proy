/*
 * leeArchivo — Reconstruye el blob de un archivo APFS y lo entrega a navvis.
 * Sistemas Operativos
 *
 * Compilar: make leeArchivo
 * Uso:      ./leeArchivo <disco.dmg> <vol_idx> <file_id>
 *
 * Camina el mismo camino que leeAPFS hasta el FS tree del volumen pedido,
 * junta los extents del file_id indicado en un archivo temporal y lanza
 * navvis para que el usuario los inspeccione en hex/texto.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include "gpt.h"
#include "APFS.h"
#include "btree.h"

#define SECTOR   512
#define RUTA_TMP "/tmp/leeAPFS_blob.bin"

typedef struct
{
    char    *base;
    long     fs;
    char    *part;
    uint32_t bsize;
} ctx_t;

static inline void *bloque(ctx_t *c, paddr_t p) { return c->part + (long long)p * c->bsize; }

typedef void (*fs_cb_t)(void *llave, uint32_t klen,
                        void *valor, uint32_t vlen, void *ctx);

/* ════════════════════════════════════════════════════════════
 *  B-ARBOL — mismo sobretiro que en leeAPFS, por simetria.
 * ════════════════════════════════════════════════════════════ */

static paddr_t lee_omap(ctx_t *c, paddr_t raiz, oid_t oid)
{
    btree_node_phys_t *n = bloque(c, raiz);
    uint8_t *kz = bt_zona_llaves(n);
    uint8_t *vf = bt_fin_valores(n, c->bsize);

    if (bt_es_hoja(n))
    {
        for (uint32_t i = 0; i < n->btn_nkeys; i++)
        {
            omap_key_t *k = (omap_key_t *)(kz + bt_k_off(n, i));
            if (k->ok_oid == oid)
                return ((omap_val_t *)(vf - bt_v_off(n, i)))->ov_paddr;
        }
        return 0;
    }

    int r = -1;
    for (uint32_t i = 0; i < n->btn_nkeys; i++)
    {
        omap_key_t *k = (omap_key_t *)(kz + bt_k_off(n, i));
        if (k->ok_oid > oid) break;    // sobretiro
        r = i;
    }
    if (r < 0) return 0;
    paddr_t hijo = *(paddr_t *)(vf - bt_v_off(n, r));
    return lee_omap(c, hijo, oid);
}

static void lee_fs(ctx_t *c, paddr_t raiz, paddr_t vomap,
                   uint64_t target, fs_cb_t cb, void *ctx)
{
    btree_node_phys_t *n = bloque(c, raiz);
    uint8_t *kz = bt_zona_llaves(n);
    uint8_t *vf = bt_fin_valores(n, c->bsize);

    if (bt_es_hoja(n))
    {
        for (uint32_t i = 0; i < n->btn_nkeys; i++)
        {
            j_key_t *k = (j_key_t *)(kz + bt_k_off(n, i));
            uint64_t oid = k->obj_id_and_type & OBJ_ID_MASK;
            if (oid == target)
                cb(k, bt_k_len(n, i), vf - bt_v_off(n, i), bt_v_len(n, i), ctx);
            else if (oid > target) return;
        }
        return;
    }

    // Interno: descender por toda rama i donde key[i].oid <= target
    // y (key[i+1].oid >= target o i es la ultima) — los records de un oid
    // pueden repartirse entre hojas vecinas.
    for (uint32_t i = 0; i < n->btn_nkeys; i++)
    {
        j_key_t *k = (j_key_t *)(kz + bt_k_off(n, i));
        uint64_t oid = k->obj_id_and_type & OBJ_ID_MASK;
        if (oid > target) break;

        uint64_t next_oid = (uint64_t)-1;
        if (i + 1 < n->btn_nkeys) {
            j_key_t *kn = (j_key_t *)(kz + bt_k_off(n, i+1));
            next_oid = kn->obj_id_and_type & OBJ_ID_MASK;
        }
        if (next_oid < target) continue;

        uint64_t hijo_oid = *(uint64_t *)(vf - bt_v_off(n, i));
        paddr_t hijo = lee_omap(c, vomap, hijo_oid);
        if (hijo) lee_fs(c, hijo, vomap, target, cb, ctx);
    }
}

/* ════════════════════════════════════════════════════════════
 *  CALLBACKS: primero la talla, luego el volcado.
 * ════════════════════════════════════════════════════════════ */

typedef struct { uint64_t talla; } tallactx_t;

static void cb_talla(void *k, uint32_t kl, void *v, uint32_t vl, void *u)
{
    (void)kl; (void)vl;
    j_key_t *jk = k;
    if (((jk->obj_id_and_type & OBJ_TYPE_MASK) >> OBJ_TYPE_SHIFT) != APFS_TYPE_FILE_EXTENT) return;
    j_file_extent_key_t *ek = k;
    j_file_extent_val_t *ev = v;
    uint64_t fin = ek->logical_addr + (ev->len_and_flags & J_FILE_EXTENT_LEN_MASK);
    tallactx_t *t = u;
    if (fin > t->talla) t->talla = fin;
}

typedef struct { int fd; ctx_t *c; } dumpctx_t;

static void cb_dump(void *k, uint32_t kl, void *v, uint32_t vl, void *u)
{
    (void)kl; (void)vl;
    j_key_t *jk = k;
    if (((jk->obj_id_and_type & OBJ_TYPE_MASK) >> OBJ_TYPE_SHIFT) != APFS_TYPE_FILE_EXTENT) return;
    j_file_extent_key_t *ek = k;
    j_file_extent_val_t *ev = v;
    uint64_t len = ev->len_and_flags & J_FILE_EXTENT_LEN_MASK;
    if (ev->phys_block_num == 0) return;   // hueco — respetamos el silencio
    dumpctx_t *d = u;
    void *src = bloque(d->c, ev->phys_block_num);
    pwrite(d->fd, src, len, ek->logical_addr);
}

/* ════════════════════════════════════════════════════════════
 *  BASICOS
 * ════════════════════════════════════════════════════════════ */

static char *mapea(const char *ruta, long *fs)
{
    int fd = open(ruta, O_RDONLY);
    if (fd < 0) { perror("open"); return NULL; }
    struct stat st; fstat(fd, &st);
    *fs = st.st_size;
    char *m = mmap(0, *fs, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) { perror("mmap"); return NULL; }
    return m;
}

static void localiza_apfs(ctx_t *c)
{
    struct gpt_header gpt;
    memcpy(&gpt, c->base + SECTOR, sizeof(gpt));
    efi_partition_entry p;
    memcpy(&p, c->base + gpt.partition_entry_lba * SECTOR, sizeof(p));
    c->part = c->base + p.start * SECTOR;
}

/* ════════════════════════════════════════════════════════════
 *  MAIN
 * ════════════════════════════════════════════════════════════ */

int main(int argc, char *argv[])
{
    if (argc < 4) {
        fprintf(stderr, "Uso: %s <disco.dmg> <vol_idx> <file_id>\n", argv[0]);
        return 1;
    }
    const char *disco = argv[1];
    int vidx = atoi(argv[2]);
    uint64_t fid = strtoull(argv[3], NULL, 10);

    ctx_t c = {0};
    c.base = mapea(disco, &c.fs);
    if (!c.base) return 1;
    localiza_apfs(&c);

    nx_superblock_t *nxsb = (nx_superblock_t *)c.part;
    if (nxsb->nx_magic != NX_MAGIC) { fprintf(stderr, "NXSB invalido\n"); return 1; }
    c.bsize = nxsb->nx_block_size;

    /* Volumen pedido — mismo baile que en leeAPFS. */
    omap_phys_t *cmap  = bloque(&c, nxsb->nx_omap_oid);
    paddr_t      ctree = cmap->om_tree_oid;
    paddr_t vsb_p = lee_omap(&c, ctree, nxsb->nx_fs_oid[vidx]);
    if (!vsb_p) { fprintf(stderr, "No resolvi el volumen %d\n", vidx); return 1; }

    apfs_superblock_t *vsb = bloque(&c, vsb_p);
    omap_phys_t       *vph = bloque(&c, vsb->apfs_omap_oid);
    paddr_t vomap = vph->om_tree_oid;
    paddr_t fs_root = lee_omap(&c, vomap, vsb->apfs_root_tree_oid);
    if (!fs_root) { fprintf(stderr, "No resolvi la raiz del FS tree\n"); return 1; }

    /* Dos pasadas: primero cuadramos talla para el ftruncate, luego volcamos. */
    int fd = open(RUTA_TMP, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror("open blob"); return 1; }

    tallactx_t tc = { .talla = 0 };
    lee_fs(&c, fs_root, vomap, fid, cb_talla, &tc);
    if (tc.talla > 0) ftruncate(fd, tc.talla);

    dumpctx_t dc = { .fd = fd, .c = &c };
    lee_fs(&c, fs_root, vomap, fid, cb_dump, &dc);
    close(fd);

    /* El blob quedo listo — dejamos que navvis lo muestre. */
    execl("./navvis", "navvis", RUTA_TMP, (char *)NULL);
    perror("exec navvis");
    return 1;
}
