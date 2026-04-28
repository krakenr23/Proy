#ifndef APFS_H
#define APFS_H

#include <stdint.h>

// Tipos de Apple
// https://developer.apple.com/support/downloads/Apple-File-System-Reference.pdf

// Tipos de proposito general

// Domicilio fisico de un bloque de disco
typedef int64_t paddr_t;

// Rango de direcciones
struct prange
{
    paddr_t pr_start_paddr;
    uint64_t pr_block_count;
};
typedef struct prange prange_t;

// Tipos objeto (o) y transaccion (x)
typedef uint64_t oid_t;
typedef uint64_t xid_t;

// UUID si mismo que GUID

typedef unsigned char uuid_t[16];

// Defines de varias cosas
#define MAX_CKSUM_SIZE 8
#define NX_MAGIC 'BSXN'
#define NX_MAX_FILE_SYSTEMS 100
#define NX_EPH_INFO_COUNT 4
#define NX_EPH_MIN_BLOCK_COUNT 8
#define NX_MAX_FILE_SYSTEM_EPH_STRUCTS 4
#define NX_TX_MIN_CHECKPOINT_COUNT 4
#define NX_EPH_INFO_VERSION_1 1

// Tipos de Objetos
#define OBJECT_TYPE_NX_SUPERBLOCK 0x00000001
#define OBJECT_TYPE_BTREE 0x00000002
#define OBJECT_TYPE_BTREE_NODE 0x00000003
#define OBJECT_TYPE_SPACEMAN 0x00000005
#define OBJECT_TYPE_SPACEMAN_CAB 0x00000006
#define OBJECT_TYPE_SPACEMAN_CIB 0x00000007
#define OBJECT_TYPE_SPACEMAN_BITMAP 0x00000008
#define OBJECT_TYPE_SPACEMAN_FREE_QUEUE 0x00000009
#define OBJECT_TYPE_EXTENT_LIST_TREE 0x0000000a
#define OBJECT_TYPE_OMAP 0x0000000b
#define OBJECT_TYPE_CHECKPOINT_MAP 0x0000000c
#define OBJECT_TYPE_FS 0x0000000d
#define OBJECT_TYPE_FSTREE 0x0000000e
#define OBJECT_TYPE_BLOCKREFTREE 0x0000000f
#define OBJECT_TYPE_SNAPMETATREE 0x00000010
#define OBJECT_TYPE_NX_REAPER 0x00000011
#define OBJECT_TYPE_NX_REAP_LIST 0x00000012
#define OBJECT_TYPE_OMAP_SNAPSHOT 0x00000013
#define OBJECT_TYPE_EFI_JUMPSTART 0x00000014
#define OBJECT_TYPE_FUSION_MIDDLE_TREE 0x00000015
#define OBJECT_TYPE_NX_FUSION_WBC 0x00000016
#define OBJECT_TYPE_NX_FUSION_WBC_LIST 0x00000017
#define OBJECT_TYPE_ER_STATE 0x00000018
#define OBJECT_TYPE_GBITMAP 0x00000019
#define OBJECT_TYPE_GBITMAP_TREE 0x0000001a
#define OBJECT_TYPE_GBITMAP_BLOCK 0x0000001b
#define OBJECT_TYPE_ER_RECOVERY_BLOCK 0x0000001c
#define OBJECT_TYPE_SNAP_META_EXT 0x0000001d
#define OBJECT_TYPE_INTEGRITY_META 0x0000001e
#define OBJECT_TYPE_FEXT_TREE 0x0000001f
#define OBJECT_TYPE_RESERVED_20 0x00000020
#define OBJECT_TYPE_INVALID 0x00000000
#define OBJECT_TYPE_TEST 0x000000ff
#define OBJECT_TYPE_CONTAINER_KEYBAG 'keys'
#define OBJECT_TYPE_VOLUME_KEYBAG 'recs'
#define OBJECT_TYPE_MEDIA_KEYBAG 'mkey'

// Banderas de objetos

#define OBJ_VIRTUAL 0x00000000
#define OBJ_EPHEMERAL 0x80000000
#define OBJ_PHYSICAL 0x40000000
#define OBJ_NOHEADER 0x20000000
#define OBJ_ENCRYPTED 0x10000000
#define OBJ_NONPERSISTENT 0x08000000

// Enumeraciones

typedef enum
{
    NX_CNTR_OBJ_CKSUM_SET = 0,
    NX_CNTR_OBJ_CKSUM_FAIL = 1,
    NX_NUM_COUNTERS = 32
} nx_counter_id_t;

struct obj_phys
{
    uint8_t o_cksum[MAX_CKSUM_SIZE];
    oid_t o_oid;
    xid_t o_xid;
    uint32_t o_type;
    uint32_t o_subtype;
};
typedef struct obj_phys obj_phys_t;

// Contenedor

struct nx_superblock
{
    obj_phys_t nx_o;
    uint32_t nx_magic;
    uint32_t nx_block_size;
    uint64_t nx_block_count;
    uint64_t nx_features;
    uint64_t nx_readonly_compatible_features;
    uint64_t nx_incompatible_features;
    uuid_t nx_uuid;
    oid_t nx_next_oid;
    xid_t nx_next_xid;
    uint32_t nx_xp_desc_blocks;
    uint32_t nx_xp_data_blocks;
    paddr_t nx_xp_desc_base;
    paddr_t nx_xp_data_base;
    uint32_t nx_xp_desc_next;
    uint32_t nx_xp_data_next;
    uint32_t nx_xp_desc_index;
    uint32_t nx_xp_desc_len;
    uint32_t nx_xp_data_index;
    uint32_t nx_xp_data_len;
    oid_t nx_spaceman_oid;
    oid_t nx_omap_oid;
    oid_t nx_reaper_oid;
    uint32_t nx_test_type;
    uint32_t nx_max_file_systems;
    oid_t nx_fs_oid[NX_MAX_FILE_SYSTEMS];
    uint64_t nx_counters[NX_NUM_COUNTERS];
    prange_t nx_blocked_out_prange;
    oid_t nx_evict_mapping_tree_oid;
    uint64_t nx_flags;
    paddr_t nx_efi_jumpstart;
    uuid_t nx_fusion_uuid;
    prange_t nx_keylocker;
    uint64_t nx_ephemeral_info[NX_EPH_INFO_COUNT];
    oid_t nx_test_oid;
    oid_t nx_fusion_mt_oid;
    oid_t nx_fusion_wbc_oid;
    prange_t nx_fusion_wbc;
    uint64_t nx_newest_mounted_version;
    prange_t nx_mkb_locker;
};
typedef struct nx_superblock nx_superblock_t;

// Estructuras para checar

struct checkpoint_mapping
{
    uint32_t cpm_type;
    uint32_t cpm_subtype;
    uint32_t cpm_size;
    uint32_t cpm_pad;
    oid_t cpm_fs_oid;
    oid_t cpm_oid;
    oid_t cpm_paddr;
};
typedef struct checkpoint_mapping checkpoint_mapping_t;

struct checkpoint_map_phys
{
    obj_phys_t cpm_o;
    uint32_t cpm_flags;
    uint32_t cpm_count;
    checkpoint_mapping_t cpm_map[];
};
typedef struct checkpoint_map_phys checkpoint_map_phys_t;

// Volumenes

// El bloque donde APFS guarda todo lo que necesita un volumen para existir.
// Solo marcamos los campos que usamos; el resto va declarado para que los
// offsets del on-disk cuadren exactos.

#define APFS_MAGIC 'BSPA'            // 'APSB' leido en little-endian
#define APFS_MODIFIED_NAMELEN 32
#define APFS_MAX_HIST 8
#define APFS_VOLNAME_LEN 256

#define APFS_INCOMPAT_CASE_INSENSITIVE          0x00000001
#define APFS_INCOMPAT_NORMALIZATION_INSENSITIVE 0x00000008

#pragma pack(push,1)

struct wrapped_meta_crypto_state
{
    uint16_t major_version;
    uint16_t minor_version;
    uint32_t cpflags;
    uint32_t persistent_class;
    uint32_t key_os_version;
    uint16_t key_revision;
    uint16_t unused;
};
typedef struct wrapped_meta_crypto_state wrapped_meta_crypto_state_t;

struct apfs_modified_by
{
    uint8_t id[APFS_MODIFIED_NAMELEN];
    uint64_t timestamp;
    xid_t last_xid;
};
typedef struct apfs_modified_by apfs_modified_by_t;

struct apfs_superblock
{
    obj_phys_t apfs_o;
    uint32_t apfs_magic;
    uint32_t apfs_fs_index;
    uint64_t apfs_features;
    uint64_t apfs_readonly_compatible_features;
    uint64_t apfs_incompatible_features;       // Bandera case-insensitive vive aqui
    uint64_t apfs_unmount_time;
    uint64_t apfs_fs_reserve_block_count;
    uint64_t apfs_fs_quota_block_count;
    uint64_t apfs_fs_alloc_count;
    wrapped_meta_crypto_state_t apfs_meta_crypto;
    uint32_t apfs_root_tree_type;
    uint32_t apfs_extentref_tree_type;
    uint32_t apfs_snap_meta_tree_type;
    oid_t apfs_omap_oid;                       // OMAP del volumen — fisico
    oid_t apfs_root_tree_oid;                  // Raiz del FS — virtual, via OMAP
    oid_t apfs_extentref_tree_oid;
    oid_t apfs_snap_meta_tree_oid;
    xid_t apfs_revert_to_xid;
    oid_t apfs_revert_to_sblock_oid;
    uint64_t apfs_next_obj_id;
    uint64_t apfs_num_files;
    uint64_t apfs_num_directories;
    uint64_t apfs_num_symlinks;
    uint64_t apfs_num_other_fsobjects;
    uint64_t apfs_num_snapshots;
    uint64_t apfs_total_blocks_alloced;
    uint64_t apfs_total_blocks_freed;
    uuid_t apfs_vol_uuid;
    uint64_t apfs_last_mod_time;
    uint64_t apfs_fs_flags;
    apfs_modified_by_t apfs_formatted_by;
    apfs_modified_by_t apfs_modified_by[APFS_MAX_HIST];
    uint8_t apfs_volname[APFS_VOLNAME_LEN];
    uint32_t apfs_next_doc_id;
    uint16_t apfs_role;
    uint16_t reserved;
    xid_t apfs_root_to_xid;
    oid_t apfs_er_state_oid;
};
typedef struct apfs_superblock apfs_superblock_t;

// Mapa de objetos — traduce oid virtual a direccion fisica

struct omap_phys
{
    obj_phys_t om_o;
    uint32_t om_flags;
    uint32_t om_snap_count;
    uint32_t om_tree_type;
    uint32_t om_snapshot_tree_type;
    oid_t om_tree_oid;                 // Paddr de la raiz del B-arbol OMAP
    oid_t om_snapshot_tree_oid;
    xid_t om_most_recent_snap;
    xid_t om_pending_revert_min;
    xid_t om_pending_revert_max;
};
typedef struct omap_phys omap_phys_t;

struct omap_key
{
    oid_t ok_oid;                      // OID virtual a traducir
    xid_t ok_xid;                      // Version deseada
};
typedef struct omap_key omap_key_t;

struct omap_val
{
    uint32_t ov_flags;
    uint32_t ov_size;
    paddr_t ov_paddr;                  // Donde realmente vive el objeto
};
typedef struct omap_val omap_val_t;

// Registros del sistema de archivos — todo comienza con j_key_t.
// Los 4 bits altos del campo dicen el tipo, los 60 bajos el id.

#define OBJ_ID_MASK    0x0fffffffffffffffULL
#define OBJ_TYPE_MASK  0xf000000000000000ULL
#define OBJ_TYPE_SHIFT 60

#define APFS_TYPE_ANY           0
#define APFS_TYPE_SNAP_METADATA 1
#define APFS_TYPE_EXTENT        2
#define APFS_TYPE_INODE         3
#define APFS_TYPE_XATTR         4
#define APFS_TYPE_SIBLING_LINK  5
#define APFS_TYPE_DSTREAM_ID    6
#define APFS_TYPE_CRYPTO_STATE  7
#define APFS_TYPE_FILE_EXTENT   8
#define APFS_TYPE_DIR_REC       9
#define APFS_TYPE_DIR_STATS     10

#define ROOT_DIR_INO_NUM 2                 // El inodo raiz de todo volumen APFS

/* Llave base: los 4 bits altos son el tipo, los 60 bajos el oid. */
struct j_key
{
    uint64_t obj_id_and_type;
};
typedef struct j_key j_key_t;

/* Valor del inodo — ojo con uncompressed_size: solo en APFS moderno. */
struct j_inode_val
{
    uint64_t parent_id;
    uint64_t private_id;           // Lo usamos para buscar extents cuando hace falta
    uint64_t create_time;
    uint64_t mod_time;
    uint64_t change_time;
    uint64_t access_time;
    uint64_t internal_flags;
    union
    {
        int32_t nchildren;         // Directorio
        int32_t nlink;             // Archivo regular
    };
    uint32_t default_protection_class;
    uint32_t write_generation_counter;
    uint32_t bsd_flags;
    uint32_t owner;
    uint32_t group;
    uint16_t mode;
    uint16_t pad1;
    uint64_t uncompressed_size;
    uint8_t xfields[];
};
typedef struct j_inode_val j_inode_val_t;

/* Entrada de directorio: la variante hash vive donde los nombres no distinguen mayus. */
#define J_DREC_LEN_MASK   0x000003ff
#define J_DREC_HASH_MASK  0xfffffc00
#define J_DREC_HASH_SHIFT 10

struct j_drec_hashed_key
{
    j_key_t hdr;
    uint32_t name_len_and_hash;
    uint8_t name[0];               // Nombre UTF-8 con NUL al final
};
typedef struct j_drec_hashed_key j_drec_hashed_key_t;

struct j_drec_key
{
    j_key_t hdr;
    uint16_t name_len;
    uint8_t name[0];
};
typedef struct j_drec_key j_drec_key_t;

#define DREC_TYPE_MASK 0x000f
#define DT_DIR  4
#define DT_REG  8
#define DT_LNK  10

struct j_drec_val
{
    uint64_t file_id;              // OID del inodo al que apunta la entrada
    uint64_t date_added;
    uint16_t flags;                // Los 4 bits bajos = DT_DIR / DT_REG / ...
    uint8_t xfields[];
};
typedef struct j_drec_val j_drec_val_t;

/* Extent — aqui vive el tamano real del archivo (campo len_and_flags). */
#define J_FILE_EXTENT_LEN_MASK   0x00ffffffffffffffULL
#define J_FILE_EXTENT_FLAG_MASK  0xff00000000000000ULL
#define J_FILE_EXTENT_FLAG_SHIFT 56

struct j_file_extent_key
{
    j_key_t hdr;
    uint64_t logical_addr;         // Offset dentro del archivo
};
typedef struct j_file_extent_key j_file_extent_key_t;

struct j_file_extent_val
{
    uint64_t len_and_flags;        // Talla del blob — lo que el hex viewer necesita
    uint64_t phys_block_num;       // Donde vive en disco
    uint64_t crypto_id;
};
typedef struct j_file_extent_val j_file_extent_val_t;

#pragma pack(pop)

#endif
