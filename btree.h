#ifndef BTREE_H
#define BTREE_H

#include <stdint.h>
#include "APFS.h"

// Cada nodo B-arbol de APFS es un bloque autocontenido.
// Apple amontona todo en btn_data[] y te deja calcular offsets a mano:
//   [ TOC ][ libre ][ llaves → ][ ← valores ][ btree_info (solo raiz) ]
//
// Las llaves crecen hacia adelante desde el final de la TOC,
// los valores crecen hacia atras desde el final del nodo.

#pragma pack(push,1)

/* Banderas del nodo */
#define BTNODE_ROOT              0x0001
#define BTNODE_LEAF              0x0002
#define BTNODE_FIXED_KV_SIZE     0x0004   // KV con tamano fijo — TOC de kvoff_t
#define BTNODE_HASHED            0x0008
#define BTNODE_NOHEADER          0x0010
#define BTNODE_CHECK_KOFF_INVAL  0x8000

#define BTOFF_INVALID 0xffff

/* Par offset/longitud dentro del nodo — en bytes. */
struct nloc
{
    uint16_t off;
    uint16_t len;
};
typedef struct nloc nloc_t;

/* TOC de tamano variable — guarda offset y largo de cada campo. */
struct kvloc
{
    nloc_t k;
    nloc_t v;
};
typedef struct kvloc kvloc_t;

/* TOC de tamano fijo — solo offsets, los tamanos viven en btree_info. */
struct kvoff
{
    uint16_t k;
    uint16_t v;
};
typedef struct kvoff kvoff_t;

/* Un nodo B-arbol crudo, tal como vive en disco. */
struct btree_node_phys
{
    obj_phys_t btn_o;            // Cabecera: tipo, oid, xid, checksum
    uint16_t   btn_flags;        // ROOT / LEAF / FIXED_KV_SIZE ...
    uint16_t   btn_level;        // 0 = hoja, sube al subir el arbol
    uint32_t   btn_nkeys;        // Cuantas entradas vivas hay
    nloc_t     btn_table_space;  // Rango ocupado por la TOC
    nloc_t     btn_free_space;   // Hueco entre llaves y valores
    nloc_t     btn_key_free_list;
    nloc_t     btn_val_free_list;
    uint64_t   btn_data[];       // Aqui vive todo — la memoria real empieza aqui
};
typedef struct btree_node_phys btree_node_phys_t;

/* Info del arbol — vive al final del nodo raiz, nunca en los demas. */
struct btree_info_fixed
{
    uint32_t bt_flags;
    uint32_t bt_node_size;
    uint32_t bt_key_size;        // Solo valido en arboles FIXED_KV_SIZE
    uint32_t bt_val_size;
};
typedef struct btree_info_fixed btree_info_fixed_t;

struct btree_info
{
    btree_info_fixed_t bt_fixed;
    uint32_t bt_longest_key;
    uint32_t bt_longest_val;
    uint64_t bt_key_count;
    uint64_t bt_node_count;
};
typedef struct btree_info btree_info_t;

#pragma pack(pop)

/* Ayudantes de geometria — puro aritmetico, por eso viven inline. */

/* Donde empieza la zona de llaves (justo despues de la TOC). */
static inline uint8_t *bt_zona_llaves(btree_node_phys_t *n)
{
    return ((uint8_t *)n->btn_data) + n->btn_table_space.off + n->btn_table_space.len;
}

/* Donde termina la zona de valores — referencia para los offsets "v".
 * En nodos raiz, los ultimos 40 bytes son el btree_info, asi que se resta. */
static inline uint8_t *bt_fin_valores(btree_node_phys_t *n, uint32_t bsize)
{
    uint8_t *fin = (uint8_t *)n + bsize;
    if (n->btn_flags & BTNODE_ROOT) fin -= sizeof(btree_info_t);
    return fin;
}

static inline int bt_es_hoja(btree_node_phys_t *n) { return (n->btn_flags & BTNODE_LEAF) != 0; }
static inline int bt_es_fijo(btree_node_phys_t *n) { return (n->btn_flags & BTNODE_FIXED_KV_SIZE) != 0; }
static inline int bt_es_raiz(btree_node_phys_t *n) { return (n->btn_flags & BTNODE_ROOT) != 0; }

/* La TOC viene en dos sabores — fijo (kvoff_t, 4 bytes) o variable (kvloc_t, 8 bytes).
 * Estos helpers te devuelven los offsets correctos sin que tengas que andar casteando. */
static inline uint16_t bt_k_off(btree_node_phys_t *n, uint32_t i)
{
    uint8_t *t = (uint8_t *)n->btn_data + n->btn_table_space.off;
    return bt_es_fijo(n) ? ((kvoff_t *)t)[i].k : ((kvloc_t *)t)[i].k.off;
}
static inline uint16_t bt_v_off(btree_node_phys_t *n, uint32_t i)
{
    uint8_t *t = (uint8_t *)n->btn_data + n->btn_table_space.off;
    return bt_es_fijo(n) ? ((kvoff_t *)t)[i].v : ((kvloc_t *)t)[i].v.off;
}
/* Longitudes — solo tienen sentido en el TOC variable. */
static inline uint16_t bt_k_len(btree_node_phys_t *n, uint32_t i)
{
    uint8_t *t = (uint8_t *)n->btn_data + n->btn_table_space.off;
    return bt_es_fijo(n) ? 0 : ((kvloc_t *)t)[i].k.len;
}
static inline uint16_t bt_v_len(btree_node_phys_t *n, uint32_t i)
{
    uint8_t *t = (uint8_t *)n->btn_data + n->btn_table_space.off;
    return bt_es_fijo(n) ? 0 : ((kvloc_t *)t)[i].v.len;
}

#endif
