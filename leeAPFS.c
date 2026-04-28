/*
 * leeAPFS — Navegador APFS (disco → contenedor → volumen → directorios)
 * Sistemas Operativos
 *
 * Compilar: make leeAPFS
 * Uso:      ./leeAPFS [imagen.dmg]
 *
 * Niveles:
 *   1. DISCO       — tabla GPT con cada particion (tipo, LBA, tamano).
 *   2. CONTENEDOR  — cabecera NX superblock + lista de volumenes APFS.
 *   3. DIRECTORIO  — entradas del FS tree con ".." al frente.
 *   4. ARCHIVO     — entregado a leeArchivo, que arma el blob y lanza navvis.
 *
 * CONTROLES: ↑ ↓ = mover, Enter/→ = entrar, u/← = subir, q/ESC = salir.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <curses.h>

#include "mbr.h"
#include "gpt.h"
#include "APFS.h"
#include "btree.h"

#define SECTOR       512
#define MAX_ENTRADAS 4096
#define RUTA_DISCO   "DiscoAPFS.dmg"

#define COL_HEADER 1
#define COL_DIR    2
#define COL_FILE   3
#define COL_SELECT 4
#define COL_STATUS 5

/* GUIDs conocidos — primer uint32_t en little-endian basta para reconocerlos. */
#define GUID_TAG_APFS  0x7C3457EF
#define GUID_TAG_EFI   0xC12A7328
#define GUID_TAG_HFS   0x48465300

typedef struct
{
    char    *base;
    long     fs;
    char    *part;         // Se llena al entrar al contenedor
    uint32_t bsize;
    char     ruta_disco[512];
} ctx_t;

typedef struct
{
    char     nombre[256];
    uint64_t file_id;
    uint8_t  tipo;
    uint64_t talla;
    uint64_t btime;        // create_time del inodo (ns desde epoch)
} DirEntry;

typedef void (*fs_cb_t)(void *llave, uint32_t klen,
                        void *valor, uint32_t vlen, void *ctx);

/* ════════════════════════════════════════════════════════════
 *  B-ARBOL — sobretiro con multi-rama
 * ════════════════════════════════════════════════════════════
 * OMAP: busqueda exacta (la rama buena es la ultima con key <= oid).
 * FS:   listado por obj_id. Como los records de un oid pueden repartirse
 *       entre varias hojas, descendemos por todas las ramas cuya franja
 *       pudiera contener el objetivo (no solo la ultima).
 */

static inline void *bloque(ctx_t *c, paddr_t p)
{
    return c->part + (long long)p * c->bsize;
}

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

    // Interno: ultima rama con key.oid <= oid — sobretiro puro.
    int r = -1;
    for (uint32_t i = 0; i < n->btn_nkeys; i++)
    {
        omap_key_t *k = (omap_key_t *)(kz + bt_k_off(n, i));
        if (k->ok_oid > oid) break;
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
            else if (oid > target) return;     // las llaves siguen ordenadas
        }
        return;
    }

    // Interno: visitamos toda rama i donde key[i].oid <= target Y (key[i+1].oid >= target
    // o no hay siguiente). Asi no se nos escapan hojas cuando los records cruzan fronteras.
    for (uint32_t i = 0; i < n->btn_nkeys; i++)
    {
        j_key_t *k = (j_key_t *)(kz + bt_k_off(n, i));
        uint64_t oid = k->obj_id_and_type & OBJ_ID_MASK;
        if (oid > target) break;                // rama y las que siguen, fuera de rango

        uint64_t next_oid = (uint64_t)-1;
        if (i + 1 < n->btn_nkeys)
        {
            j_key_t *kn = (j_key_t *)(kz + bt_k_off(n, i+1));
            next_oid = kn->obj_id_and_type & OBJ_ID_MASK;
        }
        if (next_oid < target) continue;        // rama entera por debajo

        uint64_t hijo_oid = *(uint64_t *)(vf - bt_v_off(n, i));
        paddr_t hijo = lee_omap(c, vomap, hijo_oid);
        if (hijo) lee_fs(c, hijo, vomap, target, cb, ctx);
    }
}

/* ════════════════════════════════════════════════════════════
 *  CALLBACKS DEL FS TREE
 * ════════════════════════════════════════════════════════════ */

typedef struct
{
    DirEntry *arr;
    int       n;
    int       hash;
} dirctx_t;

static void cb_dir(void *k, uint32_t kl, void *v, uint32_t vl, void *u)
{
    (void)kl; (void)vl;
    j_key_t  *jk = k;
    dirctx_t *d  = u;
    uint8_t tipo = (jk->obj_id_and_type & OBJ_TYPE_MASK) >> OBJ_TYPE_SHIFT;
    if (tipo != APFS_TYPE_DIR_REC || d->n >= MAX_ENTRADAS) return;

    uint8_t *nombre; int nlen;
    if (d->hash) {
        j_drec_hashed_key_t *dk = k;
        nlen = dk->name_len_and_hash & J_DREC_LEN_MASK;
        nombre = dk->name;
    } else {
        j_drec_key_t *dk = k;
        nlen = dk->name_len;
        nombre = dk->name;
    }

    j_drec_val_t *dv = v;
    DirEntry *e = &d->arr[d->n++];
    int cp = nlen < (int)sizeof(e->nombre) - 1 ? nlen : (int)sizeof(e->nombre) - 1;
    memcpy(e->nombre, nombre, cp);
    e->nombre[cp] = 0;
    while (cp > 0 && e->nombre[cp-1] == 0) e->nombre[--cp] = 0;

    e->file_id = dv->file_id;
    e->tipo    = dv->flags & DREC_TYPE_MASK;
    e->talla   = 0;
}

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

typedef struct { uint64_t btime; } btimectx_t;

static void cb_btime(void *k, uint32_t kl, void *v, uint32_t vl, void *u)
{
    (void)kl; (void)vl;
    j_key_t *jk = k;
    if (((jk->obj_id_and_type & OBJ_TYPE_MASK) >> OBJ_TYPE_SHIFT) != APFS_TYPE_INODE) return;
    j_inode_val_t *iv = v;
    ((btimectx_t *)u)->btime = iv->create_time;
}

/* ════════════════════════════════════════════════════════════
 *  UTILIDADES
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

static void format_size(uint64_t sz, char *buf, size_t n)
{
    if      (sz < 1024)             snprintf(buf, n, "%llu B",  (unsigned long long)sz);
    else if (sz < 1024*1024)        snprintf(buf, n, "%.1fK",   sz/1024.0);
    else if (sz < 1024LL*1024*1024) snprintf(buf, n, "%.1fM",   sz/(1024.0*1024));
    else                            snprintf(buf, n, "%.1fG",   sz/(1024.0*1024*1024));
}

/* Timestamp APFS: nanosegundos desde el epoch. */
static void format_apfs_time(uint64_t ns, char *buf, size_t n)
{
    if (ns == 0) { snprintf(buf, n, "(nunca)"); return; }
    time_t s = ns / 1000000000ULL;
    struct tm *t = localtime(&s);
    strftime(buf, n, "%Y-%m-%d %H:%M", t);
}

/* Formatea un GUID GPT (16 bytes) como XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX.
   Los primeros tres grupos van en little-endian en disco; los ultimos dos en big-endian. */
static void format_guid(const uint8_t *g, char *buf, size_t n)
{
    snprintf(buf, n,
             "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
             g[3], g[2], g[1], g[0],
             g[5], g[4],
             g[7], g[6],
             g[8], g[9],
             g[10], g[11], g[12], g[13], g[14], g[15]);
}

/* ════════════════════════════════════════════════════════════
 *  DECORADORES DE PANTALLA
 * ════════════════════════════════════════════════════════════ */

static void draw_header(const char *izq, const char *der)
{
    int r, cols; getmaxyx(stdscr, r, cols); (void)r;
    attron(COLOR_PAIR(COL_HEADER) | A_BOLD);
    mvhline(0, 0, ' ', cols);
    mvprintw(0, 1, "%s", izq);
    if (der) mvprintw(0, cols - (int)strlen(der) - 1, "%s", der);
    attroff(COLOR_PAIR(COL_HEADER) | A_BOLD);
}

static void draw_status(const char *msg)
{
    int rows, cols; getmaxyx(stdscr, rows, cols);
    attron(COLOR_PAIR(COL_STATUS) | A_BOLD);
    mvhline(rows - 1, 0, ' ', cols);
    mvprintw(rows - 1, 1, "%s", msg);
    attroff(COLOR_PAIR(COL_STATUS) | A_BOLD);
}

/* ════════════════════════════════════════════════════════════
 *  UI — DIRECTORIO
 * ════════════════════════════════════════════════════════════ */

static void ui_directorio(ctx_t *c, int vol_idx, paddr_t vomap, paddr_t fs_root, int hash,
                          uint64_t dir_oid, uint64_t parent_oid, const char *ruta)
{
    DirEntry *ents = calloc(MAX_ENTRADAS, sizeof(DirEntry));
    if (!ents) return;

    // ".." guarda el oid del padre — navegable, no solo cosmetico.
    strcpy(ents[0].nombre, "..");
    ents[0].file_id = parent_oid;
    ents[0].tipo    = DT_DIR;
    int base = 1;

    dirctx_t dctx = { .arr = ents + base, .n = 0, .hash = hash };
    lee_fs(c, fs_root, vomap, dir_oid, cb_dir, &dctx);
    int total = base + dctx.n;

    for (int i = base; i < total; i++)
    {
        if (ents[i].tipo == DT_REG) {
            tallactx_t tc = { .talla = 0 };
            lee_fs(c, fs_root, vomap, ents[i].file_id, cb_talla, &tc);
            ents[i].talla = tc.talla;
        }
        btimectx_t bc = { .btime = 0 };
        lee_fs(c, fs_root, vomap, ents[i].file_id, cb_btime, &bc);
        ents[i].btime = bc.btime;
    }

    int sel = 0, off = 0;

    for (;;)
    {
        int rows, cols; getmaxyx(stdscr, rows, cols);
        int list_rows = rows - 3;
        if (sel < off) off = sel;
        if (sel >= off + list_rows) off = sel - list_rows + 1;

        clear();
        char der[64]; snprintf(der, sizeof(der), "%d entradas  oid=%llu",
                               total, (unsigned long long)dir_oid);
        draw_header(ruta, der);

        /* Si la terminal es estrecha se omite la columna Fecha. */
        int show_btime = cols >= 54;
        int reserved   = 1 + 9 + 2;                     // tamano + separadores
        if (show_btime) reserved += 16 + 2;
        int nw = cols - reserved > 12 ? cols - reserved : 12;

        attron(COLOR_PAIR(COL_HEADER));
        mvhline(1, 0, ' ', cols);
        if (show_btime)
            mvprintw(1, 1, "%-*s %9s  %-16s", nw, "Nombre", "Tamano", "Fecha");
        else
            mvprintw(1, 1, "%-*s %9s", nw, "Nombre", "Tamano");
        attroff(COLOR_PAIR(COL_HEADER));

        for (int i = 0; i < list_rows && (i + off) < total; i++)
        {
            int idx = i + off;
            DirEntry *e = &ents[idx];
            char sz[16];
            if (e->tipo == DT_DIR) snprintf(sz, sizeof(sz), "    DIR");
            else                    format_size(e->talla, sz, sizeof(sz));
            char nm[300];
            snprintf(nm, sizeof(nm), "%s%s", e->tipo == DT_DIR ? "/" : " ", e->nombre);
            char fecha[20] = "";
            if (e->btime != 0) format_apfs_time(e->btime, fecha, sizeof(fecha));

            if (idx == sel)             attron(COLOR_PAIR(COL_SELECT) | A_BOLD);
            else if (e->tipo == DT_DIR) attron(COLOR_PAIR(COL_DIR));
            else                        attron(COLOR_PAIR(COL_FILE));
            mvhline(2 + i, 0, ' ', cols);
            if (show_btime)
                mvprintw(2 + i, 1, "%-*s %9s  %-16s", nw, nm, sz, fecha);
            else
                mvprintw(2 + i, 1, "%-*s %9s", nw, nm, sz);
            if (idx == sel)             attroff(COLOR_PAIR(COL_SELECT) | A_BOLD);
            else if (e->tipo == DT_DIR) attroff(COLOR_PAIR(COL_DIR));
            else                        attroff(COLOR_PAIR(COL_FILE));
        }

        char st[512];
        snprintf(st, sizeof(st), "%d/%d  %s  oid=%llu  [Enter] abrir  [u] subir  [q] salir",
                 sel+1, total, ents[sel].nombre,
                 (unsigned long long)ents[sel].file_id);
        draw_status(st);
        refresh();

        int key = getch();
        switch (key)
        {
            case KEY_UP:   if (sel > 0) sel--; break;
            case KEY_DOWN: if (sel < total - 1) sel++; break;
            case KEY_PPAGE: sel -= list_rows; if (sel < 0) sel = 0; break;
            case KEY_NPAGE: sel += list_rows; if (sel >= total) sel = total - 1; break;
            case '\n': case KEY_RIGHT:
            {
                DirEntry *e = &ents[sel];
                if (strcmp(e->nombre, "..") == 0) { free(ents); return; }
                if (e->tipo == DT_DIR)
                {
                    char nueva[1024];
                    snprintf(nueva, sizeof(nueva), "%s/%s", ruta, e->nombre);
                    ui_directorio(c, vol_idx, vomap, fs_root, hash,
                                  e->file_id, dir_oid, nueva);
                }
                else if (e->tipo == DT_REG)
                {
                    def_prog_mode();
                    endwin();
                    char cmd[1024];
                    snprintf(cmd, sizeof(cmd), "./leeArchivo %s %d %llu",
                             c->ruta_disco, vol_idx, (unsigned long long)e->file_id);
                    int rc = system(cmd); (void)rc;
                    reset_prog_mode();
                    refresh();
                }
                break;
            }
            case 'u': case KEY_LEFT: case KEY_BACKSPACE: case 127:
            case 'q': case 'Q': case 27:
                free(ents);
                return;
        }
    }
}

/* ════════════════════════════════════════════════════════════
 *  UI — CONTENEDOR (NX superblock + volumenes)
 * ════════════════════════════════════════════════════════════ */

static void ui_contenedor(ctx_t *c)
{
    nx_superblock_t *nxsb = (nx_superblock_t *)c->part;
    if (nxsb->nx_magic != NX_MAGIC) {
        clear();
        draw_header("Contenedor invalido", NULL);
        mvprintw(3, 2, "Esta particion no es APFS (magic: %08x).", nxsb->nx_magic);
        draw_status("Cualquier tecla vuelve");
        refresh(); getch();
        return;
    }
    c->bsize = nxsb->nx_block_size;

    /* Resolver volumenes via OMAP del contenedor. */
    struct { int idx; paddr_t p; } V[NX_MAX_FILE_SYSTEMS];
    int nv = 0;

    omap_phys_t *cmap  = bloque(c, nxsb->nx_omap_oid);
    paddr_t      ctree = cmap->om_tree_oid;

    for (uint32_t i = 0; i < nxsb->nx_max_file_systems && nv < NX_MAX_FILE_SYSTEMS; i++)
    {
        oid_t vo = nxsb->nx_fs_oid[i];
        if (!vo) continue;
        paddr_t vp = lee_omap(c, ctree, vo);
        if (!vp) continue;
        apfs_superblock_t *vsb = bloque(c, vp);
        if (vsb->apfs_magic != APFS_MAGIC) continue;
        V[nv].idx = i;
        V[nv].p   = vp;
        nv++;
    }

    int sel = 0;
    const int cab = 7;                                 // Altura de la cabecera informativa

    for (;;)
    {
        int rows, cols; getmaxyx(stdscr, rows, cols); (void)rows;
        clear();

        char der[48]; snprintf(der, sizeof(der), "%d volumenes", nv);
        draw_header("Contenedor APFS", der);

        /* Info del NX superblock */
        mvprintw(1, 2, "Block size:   %u", nxsb->nx_block_size);
        mvprintw(2, 2, "Block count:  %llu  (~", (unsigned long long)nxsb->nx_block_count);
        char sz[16]; format_size(nxsb->nx_block_count * (uint64_t)nxsb->nx_block_size, sz, sizeof(sz));
        printw("%s)", sz);
        mvprintw(3, 2, "Next OID:     %llu", (unsigned long long)nxsb->nx_next_oid);
        mvprintw(4, 2, "Next XID:     %llu", (unsigned long long)nxsb->nx_next_xid);
        mvprintw(5, 2, "Max vol:      %u", nxsb->nx_max_file_systems);

        /* Encabezado de la tabla de volumenes */
        attron(COLOR_PAIR(COL_HEADER));
        mvhline(cab, 0, ' ', cols);
        mvprintw(cab, 1, "%-3s %-28s %-10s %-10s %-16s", "#", "Nombre", "Archivos", "Dirs", "Ultima mod.");
        attroff(COLOR_PAIR(COL_HEADER));

        for (int i = 0; i < nv; i++)
        {
            apfs_superblock_t *vsb = bloque(c, V[i].p);
            char nombre[APFS_VOLNAME_LEN];
            strncpy(nombre, (char *)vsb->apfs_volname, APFS_VOLNAME_LEN - 1);
            nombre[APFS_VOLNAME_LEN - 1] = 0;
            char tiempo[32]; format_apfs_time(vsb->apfs_last_mod_time, tiempo, sizeof(tiempo));

            if (i == sel) attron(COLOR_PAIR(COL_SELECT) | A_BOLD);
            else          attron(COLOR_PAIR(COL_DIR));
            mvhline(cab + 1 + i, 0, ' ', cols);
            mvprintw(cab + 1 + i, 1, "%-3d %-28s %-10llu %-10llu %-16s",
                     V[i].idx, nombre,
                     (unsigned long long)vsb->apfs_num_files,
                     (unsigned long long)vsb->apfs_num_directories,
                     tiempo);
            if (i == sel) attroff(COLOR_PAIR(COL_SELECT) | A_BOLD);
            else          attroff(COLOR_PAIR(COL_DIR));
        }

        char st[256];
        snprintf(st, sizeof(st), "%d/%d  [Enter] entrar volumen  [u] subir  [q] salir",
                 nv > 0 ? sel+1 : 0, nv);
        draw_status(st);
        refresh();

        int key = getch();
        switch (key)
        {
            case KEY_UP:   if (sel > 0) sel--; break;
            case KEY_DOWN: if (sel < nv - 1) sel++; break;
            case '\n': case KEY_RIGHT:
            {
                if (nv == 0) break;
                apfs_superblock_t *vsb = bloque(c, V[sel].p);
                omap_phys_t      *vph = bloque(c, vsb->apfs_omap_oid);
                paddr_t vomap   = vph->om_tree_oid;
                paddr_t fs_root = lee_omap(c, vomap, vsb->apfs_root_tree_oid);
                int hash = (vsb->apfs_incompatible_features &
                            (APFS_INCOMPAT_CASE_INSENSITIVE |
                             APFS_INCOMPAT_NORMALIZATION_INSENSITIVE)) != 0;

                char ruta[512];
                snprintf(ruta, sizeof(ruta), "/%s", (char *)vsb->apfs_volname);
                ui_directorio(c, V[sel].idx, vomap, fs_root, hash,
                              ROOT_DIR_INO_NUM, ROOT_DIR_INO_NUM, ruta);
                break;
            }
            case 'u': case KEY_LEFT: case KEY_BACKSPACE: case 127:
            case 'q': case 'Q': case 27:
                return;
        }
    }
}

/* ════════════════════════════════════════════════════════════
 *  UI — DISCO (tabla GPT)
 * ════════════════════════════════════════════════════════════ */

typedef struct
{
    efi_partition_entry e;
    uint32_t            tag;        // Primer uint32_t del TypeGUID (atajo)
    char                guid[40];   // TypeGUID formateado: 8-4-4-4-12 hex
    int                 es_apfs;    // ¿Magic NXSB al inicio?
} PartInfo;

static void ui_disco(ctx_t *c)
{
    /* Cabecera GPT vive en LBA 1. */
    struct gpt_header gpt;
    memcpy(&gpt, c->base + SECTOR, sizeof(gpt));

    uint32_t n_ent  = gpt.npartition_entries;
    uint32_t sz_ent = gpt.sizeof_partition_entry;
    if (n_ent > 128) n_ent = 128;                      // acotamos por si acaso

    PartInfo *P = calloc(n_ent, sizeof(PartInfo));
    int np = 0;

    for (uint32_t i = 0; i < n_ent; i++)
    {
        efi_partition_entry e;
        memcpy(&e, c->base + gpt.partition_entry_lba * SECTOR + (long long)i * sz_ent, sizeof(e));
        /* Slot vacio: TypeGUID todo ceros. */
        int vacio = 1;
        for (int k = 0; k < 16; k++) if (e.PartitionTypeGUID.uuid[k]) { vacio = 0; break; }
        if (vacio) continue;

        P[np].e   = e;
        P[np].tag = *(uint32_t *)&e.PartitionTypeGUID.uuid[0];
        format_guid(e.PartitionTypeGUID.uuid, P[np].guid, sizeof(P[np].guid));

        /* ¿Apunta a un NX superblock? — test definitivo. */
        nx_superblock_t *nxsb = (nx_superblock_t *)(c->base + e.start * SECTOR);
        P[np].es_apfs = (nxsb->nx_magic == NX_MAGIC);
        np++;
    }

    int sel = 0;
    for (;;)
    {
        int rows, cols; getmaxyx(stdscr, rows, cols); (void)rows;
        clear();

        char der[64]; snprintf(der, sizeof(der), "%d particiones  (%ld bytes)",
                               np, c->fs);
        draw_header(c->ruta_disco, der);

        /* Info del GPT */
        mvprintw(1, 2, "Revision GPT: 0x%08x   Entradas: %u   LBA tabla: %llu",
                 gpt.revision, gpt.npartition_entries,
                 (unsigned long long)gpt.partition_entry_lba);

        /* Tabla de particiones */
        attron(COLOR_PAIR(COL_HEADER));
        mvhline(3, 0, ' ', cols);
        mvprintw(3, 1, "%-3s %-36s %-12s %-12s %-10s",
                 "#", "GUID Tipo", "LBA inicio", "LBA fin", "Tamano");
        attroff(COLOR_PAIR(COL_HEADER));

        for (int i = 0; i < np; i++)
        {
            PartInfo *p = &P[i];
            uint64_t bytes = (p->e.end - p->e.start + 1) * SECTOR;
            char sz[16]; format_size(bytes, sz, sizeof(sz));

            if (i == sel)      attron(COLOR_PAIR(COL_SELECT) | A_BOLD);
            else if (p->es_apfs) attron(COLOR_PAIR(COL_DIR));
            else               attron(COLOR_PAIR(COL_FILE));
            mvhline(4 + i, 0, ' ', cols);
            mvprintw(4 + i, 1, "%-3d %-36s %-12llu %-12llu %-10s",
                     i, p->guid,
                     (unsigned long long)p->e.start,
                     (unsigned long long)p->e.end, sz);
            if (i == sel)      attroff(COLOR_PAIR(COL_SELECT) | A_BOLD);
            else if (p->es_apfs) attroff(COLOR_PAIR(COL_DIR));
            else               attroff(COLOR_PAIR(COL_FILE));
        }

        char st[256];
        snprintf(st, sizeof(st), "%d/%d  [Enter] entrar contenedor  [q] salir",
                 np > 0 ? sel+1 : 0, np);
        draw_status(st);
        refresh();

        int key = getch();
        switch (key)
        {
            case KEY_UP:   if (sel > 0) sel--; break;
            case KEY_DOWN: if (sel < np - 1) sel++; break;
            case '\n': case KEY_RIGHT:
            {
                if (np == 0) break;
                if (!P[sel].es_apfs) {
                    draw_status("Esa particion no es APFS — Enter para regresar");
                    refresh(); getch(); break;
                }
                c->part = c->base + P[sel].e.start * SECTOR;
                ui_contenedor(c);
                c->part = NULL;
                break;
            }
            case 'q': case 'Q': case 27:
                free(P);
                return;
        }
    }
}

/* ════════════════════════════════════════════════════════════
 *  MAIN
 * ════════════════════════════════════════════════════════════ */

int main(int argc, char *argv[])
{
    const char *ruta = argc >= 2 ? argv[1] : RUTA_DISCO;

    ctx_t c = {0};
    strncpy(c.ruta_disco, ruta, sizeof(c.ruta_disco) - 1);
    c.base = mapea(ruta, &c.fs);
    if (!c.base) return 1;

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    if (has_colors()) {
        start_color(); use_default_colors();
        init_pair(COL_HEADER, COLOR_BLACK, COLOR_CYAN);
        init_pair(COL_DIR,    COLOR_CYAN,  -1);
        init_pair(COL_FILE,   COLOR_WHITE, -1);
        init_pair(COL_SELECT, COLOR_BLACK, COLOR_WHITE);
        init_pair(COL_STATUS, COLOR_BLACK, COLOR_CYAN);
    }

    ui_disco(&c);

    endwin();
    munmap(c.base, c.fs);
    return 0;
}
