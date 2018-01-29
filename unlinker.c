/* J.Nider 27/07/2017
I tried for so long to get BFD to work, but it is just not built well enough
to be used for other tasks. There are too many format-specific flags and
behaviours that just make life difficult, which is why I am writing my own
backend from scratch. */

/* The idea of the program is simple - read in a fully linked executable,
and write out a set of unlinked .o files that can be relinked later.*/

#include <stdio.h>
#include <string.h>
#include <getopt.h>
#include <udis86.h>
#include "backend.h"

enum error_codes
{
   ERR_NONE,
   ERR_BAD_FILE,
   ERR_BAD_FORMAT,
   ERR_NO_SYMS,
   ERR_NO_SYMS_AFTER_RECONSTRUCT,
   ERR_NO_TEXT_SECTION
};

static struct option options[] =
{
  {"output-target", required_argument, 0, 'O'},
  {"reconstruct-symbols", no_argument, 0, 'R'},
  {0, no_argument, 0, 0}
};

struct config
{
   int reconstruct_symbols;
} config;

static void
usage(void)
{
   fprintf(stderr, "Unlinker performs the opposite action to 'ld'. It accepts a binary executable as input, and\n");
   fprintf(stderr, "creates a set of .o files that can be relinked.\n");
   fprintf(stderr, "unlinker <input file>\n");
}

static int reconstruct_symbols(backend_object* obj)
{
	char name[10];

	printf("reconstructing symbols from text section\n");
   /* find the text section */
   backend_section* sec_text = backend_get_section_by_name(obj, ".text");
   if (!sec_text)
      return -ERR_NO_TEXT_SECTION;

	// add a fake symbol for the filename
	backend_add_symbol(obj, "source.c", 0, SYMBOL_TYPE_FILE, 0, sec_text);

	// decode (disassemble) the executable section, and assume that any instruction following a 'ret'
   // is the beginning of a new function. Create a symbol entry at that address, and add it to the list.
	// We must also handle 'jmp' instructions in the middle of nowhere (jump tables?) in the same way.
	int sawret = 0;
	ud_t ud_obj;

	ud_init(&ud_obj);
	ud_set_mode(&ud_obj, 32); // decode in 32 bit mode
	ud_set_input_buffer(&ud_obj, sec_text->data, sec_text->size);
	//ud_set_syntax(&ud_obj, NULL); // #5 no disassemble!
	while (ud_disassemble(&ud_obj))
	{
		enum ud_mnemonic_code mnem;
		mnem = ud_insn_mnemonic(&ud_obj);
		unsigned int addr = ud_insn_off(&ud_obj);
		//printf("* %s\n", ud_insn_hex(&ud_obj));

		if (mnem == UD_Iret)
		{
			sawret = 1;
			continue;
		}

		if (mnem == UD_Ijmp)
		{
			if (sawret)
			{
				sprintf(name, "fn%06X", addr);
				backend_add_symbol(obj, name, addr, SYMBOL_TYPE_FUNCTION, 0, sec_text);
			}
			sawret = 1;
			continue;
		}

		// skip 'null' instructions until we hit the next 'valid' instruction
		if (sawret && mnem != UD_Iint3)
		{
			sawret = 0;
			sprintf(name, "fn%06X", addr);
			backend_add_symbol(obj, name, addr, SYMBOL_TYPE_FUNCTION, 0, sec_text);
		}
	}

   return 0;
}

static int
unlink_file(const char* input_filename, const char* output_target)
{
   backend_object* obj = backend_read(input_filename);

	if (!obj)
		return -ERR_BAD_FORMAT;

   // check for symbols, and rebuild if necessary
   if (backend_symbol_count(obj) == 0)
   {
      if (config.reconstruct_symbols == 0)
         return -ERR_NO_SYMS;
      else
      {
         reconstruct_symbols(obj);
         if (backend_symbol_count(obj) == 0)
            return -ERR_NO_SYMS_AFTER_RECONSTRUCT;
      }
   }

   // if the output target is not specified, use the input target
   // get the filenames from the input symbol table
   /* iterate over all symbols in the input table */
	backend_section* sec_text = NULL;
   backend_object* oo = NULL;
   backend_symbol* sym = backend_get_first_symbol(obj);
   char output_filename[24];
   while (sym)
   {
      // start by finding a file symbol
      int len;
      switch (sym->type)
      {
      case SYMBOL_TYPE_FILE:
         // if the symbol name ends in .c open a corresponding .o for it
         //printf("symbol name: %s\n", sym->name);
         len = strlen(sym->name);
         if (sym->name[len-2] != '.' || sym->name[len-1] != 'c')
         {
            sym = backend_get_next_symbol(obj);
            continue;
         }

         // write data to file
         if (oo)
         {
            printf("Writing file %s\n", output_filename);
            backend_write(oo, output_filename);
            backend_destructor(oo);
            oo = NULL;
         }

         // start a new one
         strcpy(output_filename, sym->name);
         output_filename[len-1] = 'o';
         oo = backend_create();
         if (!oo)
            return -10; 
         printf("=== Opening file %s\n", output_filename);
         backend_set_type(oo, OBJECT_TYPE_PE32);
         break;

      case SYMBOL_TYPE_SECTION:
         // create the sections and copy the symbols
         printf("Got section %s\n", sym->name);
         backend_section* bs = backend_get_section_by_name(obj, sym->name);
         printf("Found matching input section\n");
         backend_add_section(oo, 0, strdup(bs->name), 0, bs->address, NULL, bs->alignment, bs->flags);
         break;

      case SYMBOL_TYPE_FUNCTION:
			if (!sec_text)
			{
				printf("no text section found - searching\n");
         	sec_text = backend_get_section_by_name(oo, ".text");
				if (!sec_text)
				{
					printf("no text section found - adding\n");
         		sec_text = backend_add_section(oo, 0, strdup(".text"), 0, 0, NULL, 2, SECTION_FLAG_CODE);
					if (!sec_text)
					{
						printf("no text section found - major disaster\n");
						continue;
					}
				}
			}
         //printf("Got function %s\n", sym->name);
         // set the base address of all instructions referencing memory to 0
         // add function symbols to the output symbol table
			backend_add_symbol(oo, sym->name, sym->val, SYMBOL_TYPE_FUNCTION, 0, sec_text);
         break;
      }
   

      sym = backend_get_next_symbol(obj);
   }
   // write data to file
   if (oo)
   {
   	printf("Writing file %s\n", output_filename);
      backend_write(oo, output_filename);
      //backend_destructor(oo);
      oo = NULL;
   }
}

int
main (int argc, char *argv[])
{
   int status = 0;
   char *input_filename = NULL;
   char *output_target = NULL;

   if (argc < 2)
   {
      usage();
      return -1;
   }

   char c;
   while ((c = getopt_long (argc, argv, "O:R",
          options, (int *) 0)) != EOF)
   {
      switch (c)
      {
      case 'O':
         output_target = optarg;
         break;
      case 'R':
         config.reconstruct_symbols = 1;
         break;
      default:
         usage();
         return -1;
      }
   }

   if (argc <= optind)
   {
      printf("Missing input file name\n");
      usage();
      return -1;
   }

   input_filename = argv[optind];

   backend_init();

   int ret = unlink_file(input_filename, output_target);
   switch (ret)
   {
   case -ERR_BAD_FILE:
      printf("Can't open input file %s\n", input_filename);
      break;
   case -ERR_BAD_FORMAT:
      printf("Unhandled input file format\n");
      break;
   case -ERR_NO_SYMS:
      printf("No symbols found - try again with --reconstruct-symbols\n");
      break;
   case -ERR_NO_SYMS_AFTER_RECONSTRUCT:
      printf("No symbols found even after attempting to recreate them - maybe the code section is empty?\n");
      break;
   case -ERR_NO_TEXT_SECTION:
      printf("Can't find .text section!\n");
      break;
   }

   return status;
}
