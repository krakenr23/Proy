/*
 * NavVis - Navegador y Visor de Archivos
 * Sistemas Operativos
 *
 * Compilar: gcc -o navvis navvis.c -lncurses
 * Uso:      ./navvis [directorio]
 *
 * CONTROLES NAVEGADOR:
 *   Flechas Arriba/Abajo  — seleccionar archivo
 *   Enter                 — entrar a directorio / abrir visor
 *   Flecha Izquierda / u  — subir directorio padre
 *   q / ESC               — salir
 *
 * CONTROLES VISOR (texto y hex):
 *   Flechas Arriba/Abajo  — línea anterior / siguiente
 *   PgUp / PgDn           — página anterior / siguiente
 *   < o Home              — inicio del archivo
 *   > o End               — final del archivo
 *   g                     — ir a línea/offset específico
 *   t / h                 — alternar entre vista Texto y Hex
 *   q / ESC               — volver al navegador
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <time.h>
#include <curses.h>

/* ─── Constantes ──────────────────────────────────────────── */
#define MAX_ENTRIES   4096
#define MAX_PATH      4096
#define BYTES_PER_ROW 16
#define MAX_LINES     1000000

/* ─── Pares de color ──────────────────────────────────────── */
#define COL_HEADER 1
#define COL_DIR    2
#define COL_FILE   3
#define COL_SELECT 4
#define COL_STATUS 5
#define COL_KEYS   6

typedef struct {
    char  name[NAME_MAX + 1];
    int   is_dir;
    off_t size;
    time_t mtime;
    time_t btime;   /* fecha de creacion (st_birthtime en macOS/BSD, st_ctime como fallback en Linux) */
} Entry;

/* Devuelve la fecha de creacion del archivo si el sistema la expone;
 * en Linux la mayoria de FS no la guardan, asi que caemos a st_ctime. */
static time_t stat_btime(const struct stat *st) {
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
    return st->st_birthtime;
#else
    return st->st_ctime;
#endif
}

/* ─── Prototipos ──────────────────────────────────────────── */
void navigator(const char *start_dir);
void viewer(const char *filepath, int start_in_hex);
int  entry_cmp(const void *a, const void *b);
void format_size(off_t size, char *buf, size_t bufsz);
void draw_header(const char *left, const char *right);
void draw_status(const char *msg);
void draw_keys(const char *keys[], int n);

/* ════════════════════════════════════════════════════════════
 *  UTILIDADES
 * ════════════════════════════════════════════════════════════ */

int entry_cmp(const void *a, const void *b) {
    const Entry *ea = a, *eb = b;
    if (ea->is_dir && !eb->is_dir) return -1;
    if (!ea->is_dir && eb->is_dir) return  1;
    return strcmp(ea->name, eb->name);
}

void format_size(off_t sz, char *buf, size_t n) {
    if      (sz < 1024)           snprintf(buf, n, "%lld B",  (long long)sz);
    else if (sz < 1024*1024)      snprintf(buf, n, "%.1fK",   sz/1024.0);
    else if (sz < 1024LL*1024*1024) snprintf(buf, n, "%.1fM", sz/(1024.0*1024));
    else                          snprintf(buf, n, "%.1fG",   sz/(1024.0*1024*1024));
}

void draw_header(const char *left, const char *right) {
    int rows, cols; getmaxyx(stdscr, rows, cols); (void)rows;
    attron(COLOR_PAIR(COL_HEADER) | A_BOLD);
    mvhline(0, 0, ' ', cols);
    mvprintw(0, 1, "%s", left);
    if (right) mvprintw(0, cols - (int)strlen(right) - 1, "%s", right);
    attroff(COLOR_PAIR(COL_HEADER) | A_BOLD);
}

void draw_status(const char *msg) {
    int rows, cols; getmaxyx(stdscr, rows, cols);
    attron(COLOR_PAIR(COL_STATUS) | A_BOLD);
    mvhline(rows - 2, 0, ' ', cols);
    mvprintw(rows - 2, 1, "%s", msg);
    attroff(COLOR_PAIR(COL_STATUS) | A_BOLD);
}

void draw_keys(const char *keys[], int n) {
    int rows, cols; getmaxyx(stdscr, rows, cols);
    attron(COLOR_PAIR(COL_KEYS));
    mvhline(rows - 1, 0, ' ', cols);
    int x = 0;
    for (int i = 0; i < n && x + 2 < cols; i++) {
        attroff(COLOR_PAIR(COL_KEYS));
        attron(COLOR_PAIR(COL_SELECT) | A_BOLD);
        mvprintw(rows - 1, x, "%s", keys[i*2]);
        attroff(COLOR_PAIR(COL_SELECT) | A_BOLD);
        attron(COLOR_PAIR(COL_KEYS));
        x += strlen(keys[i*2]);
        mvprintw(rows - 1, x, "%-8s", keys[i*2+1]);
        x += 8;
    }
    attroff(COLOR_PAIR(COL_KEYS));
}

/* ════════════════════════════════════════════════════════════
 *  NAVEGADOR
 * ════════════════════════════════════════════════════════════ */

void navigator(const char *start_dir) {
    char cwd[MAX_PATH];
    strncpy(cwd, start_dir, MAX_PATH - 1);

    Entry *entries = malloc(sizeof(Entry) * MAX_ENTRIES);
    if (!entries) return;

    int n = 0, sel = 0, off = 0;

    /* Teclas: par (etiqueta, descripción) */
    const char *keys[] = {
        "Enter","Abrir",  "u/←","Subir",  "q","Salir"
    };

    for (;;) {
        /* ── Leer directorio ── */
        n = 0;
        DIR *dp = opendir(cwd);
        if (!dp) { draw_status("No se puede abrir el directorio"); getch(); break; }
        struct dirent *de;
        while ((de = readdir(dp)) && n < MAX_ENTRIES) {
            if (strcmp(de->d_name, ".") == 0) continue;
            char full[MAX_PATH];
            snprintf(full, sizeof(full) - 1, "%s/%s", cwd, de->d_name);
            struct stat st;
            if (stat(full, &st) < 0) continue;
            strncpy(entries[n].name, de->d_name, NAME_MAX);
            entries[n].is_dir = S_ISDIR(st.st_mode);
            entries[n].size   = st.st_size;
            entries[n].mtime  = st.st_mtime;
            entries[n].btime  = stat_btime(&st);
            n++;
        }
        closedir(dp);
        qsort(entries, n, sizeof(Entry), entry_cmp);

        if (sel >= n) sel = n > 0 ? n - 1 : 0;

        /* ── Dibujar ── */
        int rows, cols;
        getmaxyx(stdscr, rows, cols);
        int list_rows = rows - 4;   /* header(2) + status(1) + keys(1) */

        if (sel < off)              off = sel;
        if (sel >= off + list_rows) off = sel - list_rows + 1;

        clear();

        /* Título */
        char right[32]; snprintf(right, sizeof(right), "%d entradas", n);
        draw_header(cwd, right);

        /* Columnas: Nombre | Tamano | Fecha (creacion) | Modificacion.
         * Si la terminal es estrecha, se omite Modificacion y luego Fecha. */
        int show_mtime = cols >= 70;
        int show_btime = cols >= 54;
        int reserved   = 1 + 9 + 2;                 /* tamano + separadores */
        if (show_btime) reserved += 16 + 2;
        if (show_mtime) reserved += 16 + 2;
        int nw = cols - reserved > 12 ? cols - reserved : 12;

        attron(COLOR_PAIR(COL_HEADER));
        mvhline(1, 0, ' ', cols);
        if (show_mtime)
            mvprintw(1, 1, "%-*s %9s  %-16s  %-16s",
                     nw, "Nombre", "Tamano", "Fecha", "Modificacion");
        else if (show_btime)
            mvprintw(1, 1, "%-*s %9s  %-16s",
                     nw, "Nombre", "Tamano", "Fecha");
        else
            mvprintw(1, 1, "%-*s %9s",
                     nw, "Nombre", "Tamano");
        attroff(COLOR_PAIR(COL_HEADER));

        for (int i = 0; i < list_rows && (i + off) < n; i++) {
            int idx = i + off;
            Entry *e = &entries[idx];

            char sz[16];
            if (e->is_dir) snprintf(sz, sizeof(sz), "     DIR");
            else           format_size(e->size, sz, sizeof(sz));

            char fecha[20] = "", modif[20] = "";
            struct tm *tmb = localtime(&e->btime);
            if (tmb) strftime(fecha, sizeof(fecha), "%Y-%m-%d %H:%M", tmb);
            struct tm *tmm = localtime(&e->mtime);
            if (tmm) strftime(modif, sizeof(modif), "%Y-%m-%d %H:%M", tmm);

            char nm[NAME_MAX + 3];
            snprintf(nm, sizeof(nm), "%s%s", e->is_dir ? "/" : " ", e->name);

            if (idx == sel)    attron(COLOR_PAIR(COL_SELECT) | A_BOLD);
            else if (e->is_dir) attron(COLOR_PAIR(COL_DIR));
            else               attron(COLOR_PAIR(COL_FILE));

            mvhline(2 + i, 0, ' ', cols);
            if (show_mtime)
                mvprintw(2 + i, 1, "%-*s %9s  %-16s  %-16s",
                         nw, nm, sz, fecha, modif);
            else if (show_btime)
                mvprintw(2 + i, 1, "%-*s %9s  %-16s",
                         nw, nm, sz, fecha);
            else
                mvprintw(2 + i, 1, "%-*s %9s",
                         nw, nm, sz);

            if (idx == sel)    attroff(COLOR_PAIR(COL_SELECT) | A_BOLD);
            else if (e->is_dir) attroff(COLOR_PAIR(COL_DIR));
            else               attroff(COLOR_PAIR(COL_FILE));
        }

        char status[256];
        if (n > 0) {
            char sz[16]; format_size(entries[sel].size, sz, sizeof(sz));
            snprintf(status, sizeof(status), "%d/%d  %s  %s",
                     sel+1, n, entries[sel].is_dir ? "[DIR]" : sz, entries[sel].name);
        } else {
            snprintf(status, sizeof(status), "Directorio vacio");
        }
        draw_status(status);
        draw_keys(keys, 3);
        refresh();

        /* ── Input ── */
        int c = getch();
        switch (c) {
            case KEY_UP:
                if (sel > 0) sel--;
                break;
            case KEY_DOWN:
                if (sel < n - 1) sel++;
                break;
            case KEY_PPAGE:
                sel -= list_rows; if (sel < 0) sel = 0;
                break;
            case KEY_NPAGE:
                sel += list_rows; if (sel >= n) sel = n > 0 ? n-1 : 0;
                break;
            case KEY_HOME:
                sel = 0;
                break;
            case KEY_END:
                if (n > 0) sel = n - 1;
                break;
            case '\n': case KEY_RIGHT: {
                if (n <= 0) break;
                Entry *e = &entries[sel];
                char full[MAX_PATH];
                snprintf(full, sizeof(full) - 1, "%s/%s", cwd, e->name);
                if (e->is_dir) {
                    char resolved[MAX_PATH];
                    if (realpath(full, resolved)) strncpy(cwd, resolved, MAX_PATH-1);
                    sel = 0; off = 0;
                } else {
                    viewer(full, 0);   /* abre en texto por defecto */
                }
                break;
            }
            case 'u': case KEY_LEFT: case KEY_BACKSPACE: case 127: {
                char parent[MAX_PATH];
                snprintf(parent, sizeof(parent) - 1, "%s/..", cwd);
                char resolved[MAX_PATH];
                if (realpath(parent, resolved)) strncpy(cwd, resolved, MAX_PATH-1);
                sel = 0; off = 0;
                break;
            }
            case 'q': case 'Q': case 27:
                free(entries);
                return;
        }
    }
    free(entries);
}

/* ════════════════════════════════════════════════════════════
 *  VISOR  (texto + hex en la misma función, toggle con t/h)
 * ════════════════════════════════════════════════════════════ */

/* Construye una línea hexadecimal de 16 bytes */
static void hex_line(const unsigned char *base, off_t row_off, off_t fsize,
                     char *buf, size_t bufsz) {
    int o = 0;
    o += snprintf(buf+o, bufsz-o, "%08llx ", (unsigned long long)row_off);
    for (int g = 0; g < 4; g++) {
        for (int b = 0; b < 4; b++) {
            off_t p = row_off + g*4 + b;
            if (p < fsize) o += snprintf(buf+o, bufsz-o, "%02x ", (unsigned char)base[p]);
            else           o += snprintf(buf+o, bufsz-o, "   ");
        }
        if (o < (int)bufsz) buf[o++] = ' ';
    }
    for (int i = 0; i < BYTES_PER_ROW; i++) {
        off_t p = row_off + i;
        unsigned char ch = p < fsize ? (unsigned char)base[p] : ' ';
        if (o < (int)bufsz) buf[o++] = isprint(ch) ? ch : '.';
    }
    if (o < (int)bufsz) buf[o] = '\0';
}

void viewer(const char *filepath, int start_in_hex) {
    /* ── Abrir y mapear ── */
    int fd = open(filepath, O_RDONLY);
    if (fd < 0) { draw_status("No se puede abrir el archivo"); getch(); return; }

    struct stat st;
    fstat(fd, &st);
    off_t fsize = st.st_size;

    unsigned char *map = NULL;
    if (fsize > 0) {
        map = mmap(NULL, fsize, PROT_READ, MAP_SHARED, fd, 0);
        if (map == MAP_FAILED) {
            close(fd); draw_status("Error al mapear el archivo"); getch(); return;
        }
    }

    /* ── Índice de líneas para modo texto ── */
    off_t *line_off = malloc(sizeof(off_t) * MAX_LINES);
    if (!line_off) { if (map) munmap(map, fsize); close(fd); return; }

    long n_lines = 0;
    line_off[n_lines++] = 0;
    for (off_t i = 0; i < fsize && n_lines < MAX_LINES; i++)
        if (map && map[i] == '\n' && i + 1 < fsize)
            line_off[n_lines++] = i + 1;
    if (n_lines == 0) n_lines = 1;

    int hex_mode = start_in_hex;   /* 0 = texto, 1 = hex */
    long top     = 0;              /* primera fila visible (línea o fila hex) */

    /* Teclas del visor:
     *   ↑↓ PgUp PgDn — navegar
     *   < / Home     — inicio
     *   > / End      — final
     *   g            — ir a posición
     *   t            — modo texto
     *   h            — modo hex
     *   q / ESC      — volver
     */
    const char *keys_txt[] = {
        "↑↓PgUpDn","Navegar", "</>","Inicio/Fin", "g","Ir a...", "h","Hex", "q","Salir"
    };
    const char *keys_hex[] = {
        "↑↓PgUpDn","Navegar", "</>","Inicio/Fin", "g","Ir a...", "t","Texto", "q","Salir"
    };

    int rows, cols;
    for (;;) {
        getmaxyx(stdscr, rows, cols);
        int list_rows = rows - 3;   /* header(1) + status(1) + keys(1) */

        /* ── Calcular total de filas según modo ── */
        long total;
        if (hex_mode) {
            total = fsize > 0 ? (fsize + BYTES_PER_ROW - 1) / BYTES_PER_ROW : 1;
        } else {
            total = n_lines;
        }
        if (top >= total) top = total - 1;
        if (top < 0)      top = 0;

        clear();

        /* Cabecera */
        char right[48];
        double pct = total > 1 ? (double)top * 100.0 / (total - 1) : 100.0;
        snprintf(right, sizeof(right), "[%s]  %ld/%ld  %.0f%%",
                 hex_mode ? "HEX" : "TEXTO", top + 1, total, pct);
        draw_header(filepath, right);

        /* Contenido */
        if (hex_mode) {
            char linebuf[256];
            for (int i = 0; i < list_rows; i++) {
                off_t row_off = (top + i) * BYTES_PER_ROW;
                if (row_off >= fsize && fsize > 0) break;
                hex_line(map, row_off, fsize, linebuf, sizeof(linebuf));
                attron(COLOR_PAIR(COL_FILE));
                mvprintw(1 + i, 0, "%s", linebuf);
                attroff(COLOR_PAIR(COL_FILE));
            }
        } else {
            for (int i = 0; i < list_rows; i++) {
                long ln = top + i;
                if (ln >= n_lines) break;
                off_t start = line_off[ln];
                off_t end   = (ln + 1 < n_lines) ? line_off[ln+1] - 1 : fsize;
                int len = (int)(end - start);
                if (len < 0) len = 0;
                int show = len < cols ? len : cols - 1;
                attron(COLOR_PAIR(COL_FILE));
                for (int k = 0; k < show; k++) {
                    char ch = map[start + k];
                    if (ch == '\t') ch = ' ';
                    if (!isprint((unsigned char)ch) && ch != ' ') ch = '.';
                    mvaddch(1 + i, k, ch);
                }
                attroff(COLOR_PAIR(COL_FILE));
            }
        }

        /* Barra de estado */
        char status[256];
        if (hex_mode) {
            snprintf(status, sizeof(status),
                     "Offset: 0x%08llx  |  Tamano: %lld bytes  |  t:Texto  g:Ir a offset  q:Salir",
                     (unsigned long long)(top * BYTES_PER_ROW), (long long)fsize);
        } else {
            snprintf(status, sizeof(status),
                     "Linea: %ld/%ld  |  Tamano: %lld bytes  |  h:Hex  g:Ir a linea  q:Salir",
                     top + 1, n_lines, (long long)fsize);
        }
        draw_status(status);
        draw_keys(hex_mode ? keys_hex : keys_txt, 5);
        refresh();

        /* ── Input ── */
        int c = getch();
        switch (c) {
            /* Navegar línea a línea */
            case KEY_UP:
                if (top > 0) top--;
                break;
            case KEY_DOWN:
                if (top + 1 < total) top++;
                break;

            /* Página */
            case KEY_PPAGE:
                top -= list_rows; if (top < 0) top = 0;
                break;
            case KEY_NPAGE:
                top += list_rows;
                if (top >= total) top = total - 1;
                break;

            /* Inicio del archivo */
            case '<': case KEY_HOME:
                top = 0;
                break;

            /* Final del archivo */
            case '>': case KEY_END:
                top = total - list_rows;
                if (top < 0) top = 0;
                break;

            /* Ir a posición */
            case 'g': case 'G': {
                echo(); curs_set(1);
                if (hex_mode) {
                    draw_status("Ir a offset (hex, ej: 1A2F): ");
                } else {
                    draw_status("Ir a linea: ");
                }
                refresh();
                char input[32] = {0};
                mvgetnstr(rows - 2, hex_mode ? 30 : 13, input, sizeof(input) - 1);
                noecho(); curs_set(0);

                if (hex_mode) {
                    unsigned long long target = 0;
                    if (sscanf(input, "%llx", &target) == 1) {
                        long trow = (long)(target / BYTES_PER_ROW);
                        top = trow >= total ? total - 1 : trow;
                    }
                } else {
                    long target = 0;
                    if (sscanf(input, "%ld", &target) == 1 && target > 0) {
                        top = target - 1;
                        if (top >= n_lines) top = n_lines - 1;
                    }
                }
                break;
            }

            /* Alternar modos */
            case 't': case 'T':
                if (hex_mode) {
                    /* convertir fila hex → línea de texto aproximada */
                    off_t byte_pos = top * BYTES_PER_ROW;
                    long best = 0;
                    for (long i = 0; i < n_lines; i++)
                        if (line_off[i] <= byte_pos) best = i;
                    top = best;
                    hex_mode = 0;
                }
                break;
            case 'h': case 'H':
                if (!hex_mode) {
                    /* convertir línea de texto → fila hex */
                    off_t byte_pos = line_off[top];
                    top = byte_pos / BYTES_PER_ROW;
                    hex_mode = 1;
                }
                break;

            /* Salir */
            case 'q': case 'Q': case 27:
                goto done;
        }
    }
done:
    free(line_off);
    if (map && fsize > 0) munmap(map, fsize);
    close(fd);
}

/* ════════════════════════════════════════════════════════════
 *  MAIN
 * ════════════════════════════════════════════════════════════ */
int main(int argc, char *argv[]) {
    const char *start = argc >= 2 ? argv[1] : ".";

    struct stat st;
    int is_dir = stat(start, &st) == 0 && S_ISDIR(st.st_mode);
    int is_file = stat(start, &st) == 0 && S_ISREG(st.st_mode);

    if (!is_dir && !is_file) {
        fprintf(stderr, "Uso: %s [directorio|archivo]\n", argv[0]);
        return 1;
    }

    /* Inicializar ncurses */
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);

    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(COL_HEADER, COLOR_BLACK, COLOR_CYAN);
        init_pair(COL_DIR,    COLOR_CYAN,  -1);
        init_pair(COL_FILE,   COLOR_WHITE, -1);
        init_pair(COL_SELECT, COLOR_BLACK, COLOR_WHITE);
        init_pair(COL_STATUS, COLOR_BLACK, COLOR_CYAN);
        init_pair(COL_KEYS,   COLOR_BLACK, COLOR_WHITE);
    }

    if (is_file) {
        viewer(start, 0);
    } else {
        char resolved[MAX_PATH];
        if (!realpath(start, resolved)) strncpy(resolved, start, MAX_PATH - 1);
        navigator(resolved);
    }

    endwin();
    return 0;
}