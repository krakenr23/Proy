#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <string.h>
#include <curses.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include "mbr.h"
#include "gpt.h"
#include "APFS.h"

char *mapFile(char *filePath) {
    /* Abre archivo */
    int fd = open(filePath, O_RDONLY);
    if (fd == -1) {
    	perror("Error abriendo el archivo");
	    return(NULL);
    }

    /* Mapea archivo */
    struct stat st;
    fstat(fd,&st);
    long fs = st.st_size;

    char *map = mmap(0, fs, PROT_READ, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
    	close(fd);
	    perror("Error mapeando el archivo");
	    return(NULL);
    }

  return map;
}

void desp_part(int sel, MBR_PARTITION_RECORD p[]) {
    for (int j=0; j < 4; j++) {
         if (j == sel) {
           attron(A_REVERSE);
         }
         // Despliega información de la partición
         mvprintw(5+j,5,"%5x     %7d     %7d",
            p[j].OSIndicator,
            p[j].StartingLBA,
            p[j].SizeInLBA
        );
         attroff(A_REVERSE);
      }
      move(5+sel,5);
      refresh();
}

int main()
{
   char *lista[] = {"Uno", "Dos", "Tres", "Cuatro" };
   int i = 0;
   int c;
   
   // Mapea imagen del disco
   char *base = mapFile("DiscoAPFS.dmg");

   initscr();
   raw();
   keypad(stdscr, TRUE); /* Habilita las teclas de funcion y flechas */
   noecho(); /* No muestres el caracter leido */
   cbreak(); /* Haz que los caracteres se le pasen al usuario */
   
   // Leer MBR
   MASTER_BOOT_RECORD mbr;
   memcpy(&mbr,base,sizeof(mbr)); // Aqui se puede hacer un cast directo, pero lo hago con memcpy para evitar warnings de alignment

      // Leer encabezado GPT 
   struct gpt_header gpt;
   memcpy(&gpt, base + 512, sizeof(gpt));

   // Leer la primera entrada de partición GPT
   efi_partition_entry part;
   memcpy(&part,
          base + (gpt.partition_entry_lba * 512),
          sizeof(part));

   char  *s = base+part.start*512;

   nx_superblock_t sb;

  memcpy(&sb1, s + (sb.nx_xp_desc_base + 1) * sb.nx_block_size, sizeof(sb));

   
   if (sb.nx_magic != NX_MAGIC)
   {
         //Panico!!
         mvprintw(7,5,"Que pasa!!");
         mvprintw(8,5,"Magic leido: %08x", sb.nx_magic);
   }

   desp_part(0,mbr.Partition);

   do {
      c = getch();
      switch(c) {
         case KEY_UP:
            i = (i>0) ? i - 1 : 3;
            break;
         case KEY_DOWN:
            i = (i<3) ? i + 1 : 0;
            break;
         default:
            // Nothing 
            break;
      }
      move(10,5);
      printw("Estoy en %d: Lei %d",i,c);
   } while (c != 'q');
   endwin();
   return 0;
}
