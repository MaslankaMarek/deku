/*
* Author: Marek Maślanka
* Project: DEKU
* URL: https://github.com/MarekMaslanka/deku
*/
#include <error.h>

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>

#include <gelf.h>

#define PACKAGE 1	/* requred by libbfd */
#include <dis-asm.h>

#if DISASSEMBLY_STYLE_SUPPORT == 0
	#undef DISASSEMBLY_STYLE_SUPPORT
#endif

#define MAX_DISASS_LINE_LEN 512

bool ShowDebugLog = 0;
#define LOG_ERR(fmt, ...)												\
	do																	\
	{																	\
		fprintf(stderr, "ERROR (%s:%d): " fmt "\n", __FILE__, __LINE__,	\
				##__VA_ARGS__);											\
		exit(1);														\
	} while (0)
#define LOG_INFO(fmt, ...)												\
	do																	\
	{																	\
		printf(fmt "\n", ##__VA_ARGS__);								\
	} while (0)
#define LOG_DEBUG(fmt, ...)												\
	do																	\
	{																	\
		if (ShowDebugLog)												\
			printf(fmt "\n", ##__VA_ARGS__);							\
	} while (0)

#define CHECK_ALLOC(m)	\
	if (m == NULL)		\
	LOG_ERR("Failed to alloc memory in %s (%s:%d)", __func__, __FILE__, __LINE__)

#define invalidSym(sym) ((sym).st_name == 0 && (sym).st_info == 0 && (sym).st_shndx == 0)

typedef struct
{
	char *name;
	size_t index;
	bool isFun;
	bool isVar;
	size_t copiedIndex;
	GElf_Sym sym;
	void *data;
} Symbol;

typedef struct
{
	Symbol **symbols;
	Elf_Scn **copiedScnMap;
	size_t sectionsCount;
	size_t symbolsCount;
	Elf *elf;
	Elf *elfOut;
} Context;

typedef struct
{
	GElf_Sym sym;
	GElf_Shdr shdr;
	uint8_t *symData;
	void *data;
	Context *ctx;
	size_t pc;
} DisasmData;

typedef enum
{
	DIFF_NO_DIFF,
	DIFF_NEW_VAR,
	DIFF_MOD_VAR,
	DIFF_NEW_FUN,
	DIFF_MOD_FUN,
} DiffResult;

static const unsigned int crc32Table[] =
{
  0x00000000, 0x04c11db7, 0x09823b6e, 0x0d4326d9,
  0x130476dc, 0x17c56b6b, 0x1a864db2, 0x1e475005,
  0x2608edb8, 0x22c9f00f, 0x2f8ad6d6, 0x2b4bcb61,
  0x350c9b64, 0x31cd86d3, 0x3c8ea00a, 0x384fbdbd,
  0x4c11db70, 0x48d0c6c7, 0x4593e01e, 0x4152fda9,
  0x5f15adac, 0x5bd4b01b, 0x569796c2, 0x52568b75,
  0x6a1936c8, 0x6ed82b7f, 0x639b0da6, 0x675a1011,
  0x791d4014, 0x7ddc5da3, 0x709f7b7a, 0x745e66cd,
  0x9823b6e0, 0x9ce2ab57, 0x91a18d8e, 0x95609039,
  0x8b27c03c, 0x8fe6dd8b, 0x82a5fb52, 0x8664e6e5,
  0xbe2b5b58, 0xbaea46ef, 0xb7a96036, 0xb3687d81,
  0xad2f2d84, 0xa9ee3033, 0xa4ad16ea, 0xa06c0b5d,
  0xd4326d90, 0xd0f37027, 0xddb056fe, 0xd9714b49,
  0xc7361b4c, 0xc3f706fb, 0xceb42022, 0xca753d95,
  0xf23a8028, 0xf6fb9d9f, 0xfbb8bb46, 0xff79a6f1,
  0xe13ef6f4, 0xe5ffeb43, 0xe8bccd9a, 0xec7dd02d,
  0x34867077, 0x30476dc0, 0x3d044b19, 0x39c556ae,
  0x278206ab, 0x23431b1c, 0x2e003dc5, 0x2ac12072,
  0x128e9dcf, 0x164f8078, 0x1b0ca6a1, 0x1fcdbb16,
  0x018aeb13, 0x054bf6a4, 0x0808d07d, 0x0cc9cdca,
  0x7897ab07, 0x7c56b6b0, 0x71159069, 0x75d48dde,
  0x6b93dddb, 0x6f52c06c, 0x6211e6b5, 0x66d0fb02,
  0x5e9f46bf, 0x5a5e5b08, 0x571d7dd1, 0x53dc6066,
  0x4d9b3063, 0x495a2dd4, 0x44190b0d, 0x40d816ba,
  0xaca5c697, 0xa864db20, 0xa527fdf9, 0xa1e6e04e,
  0xbfa1b04b, 0xbb60adfc, 0xb6238b25, 0xb2e29692,
  0x8aad2b2f, 0x8e6c3698, 0x832f1041, 0x87ee0df6,
  0x99a95df3, 0x9d684044, 0x902b669d, 0x94ea7b2a,
  0xe0b41de7, 0xe4750050, 0xe9362689, 0xedf73b3e,
  0xf3b06b3b, 0xf771768c, 0xfa325055, 0xfef34de2,
  0xc6bcf05f, 0xc27dede8, 0xcf3ecb31, 0xcbffd686,
  0xd5b88683, 0xd1799b34, 0xdc3abded, 0xd8fba05a,
  0x690ce0ee, 0x6dcdfd59, 0x608edb80, 0x644fc637,
  0x7a089632, 0x7ec98b85, 0x738aad5c, 0x774bb0eb,
  0x4f040d56, 0x4bc510e1, 0x46863638, 0x42472b8f,
  0x5c007b8a, 0x58c1663d, 0x558240e4, 0x51435d53,
  0x251d3b9e, 0x21dc2629, 0x2c9f00f0, 0x285e1d47,
  0x36194d42, 0x32d850f5, 0x3f9b762c, 0x3b5a6b9b,
  0x0315d626, 0x07d4cb91, 0x0a97ed48, 0x0e56f0ff,
  0x1011a0fa, 0x14d0bd4d, 0x19939b94, 0x1d528623,
  0xf12f560e, 0xf5ee4bb9, 0xf8ad6d60, 0xfc6c70d7,
  0xe22b20d2, 0xe6ea3d65, 0xeba91bbc, 0xef68060b,
  0xd727bbb6, 0xd3e6a601, 0xdea580d8, 0xda649d6f,
  0xc423cd6a, 0xc0e2d0dd, 0xcda1f604, 0xc960ebb3,
  0xbd3e8d7e, 0xb9ff90c9, 0xb4bcb610, 0xb07daba7,
  0xae3afba2, 0xaafbe615, 0xa7b8c0cc, 0xa379dd7b,
  0x9b3660c6, 0x9ff77d71, 0x92b45ba8, 0x9675461f,
  0x8832161a, 0x8cf30bad, 0x81b02d74, 0x857130c3,
  0x5d8a9099, 0x594b8d2e, 0x5408abf7, 0x50c9b640,
  0x4e8ee645, 0x4a4ffbf2, 0x470cdd2b, 0x43cdc09c,
  0x7b827d21, 0x7f436096, 0x7200464f, 0x76c15bf8,
  0x68860bfd, 0x6c47164a, 0x61043093, 0x65c52d24,
  0x119b4be9, 0x155a565e, 0x18197087, 0x1cd86d30,
  0x029f3d35, 0x065e2082, 0x0b1d065b, 0x0fdc1bec,
  0x3793a651, 0x3352bbe6, 0x3e119d3f, 0x3ad08088,
  0x2497d08d, 0x2056cd3a, 0x2d15ebe3, 0x29d4f654,
  0xc5a92679, 0xc1683bce, 0xcc2b1d17, 0xc8ea00a0,
  0xd6ad50a5, 0xd26c4d12, 0xdf2f6bcb, 0xdbee767c,
  0xe3a1cbc1, 0xe760d676, 0xea23f0af, 0xeee2ed18,
  0xf0a5bd1d, 0xf464a0aa, 0xf9278673, 0xfde69bc4,
  0x89b8fd09, 0x8d79e0be, 0x803ac667, 0x84fbdbd0,
  0x9abc8bd5, 0x9e7d9662, 0x933eb0bb, 0x97ffad0c,
  0xafb010b1, 0xab710d06, 0xa6322bdf, 0xa2f33668,
  0xbcb4666d, 0xb8757bda, 0xb5365d03, 0xb1f740b4
};

Context initContext(Elf *elf, Elf *elfOut);
void freeContext(Context *ctx);

static uint32_t crc32(uint8_t *data, uint32_t len)
{
	unsigned int crc = 0;
	while (len--)
	{
		crc = (crc << 8) ^ crc32Table[((crc >> 24) ^ *data) & 255];
		data++;
	}
	return crc;
}

static size_t appendString(GElf_Shdr *shdr, Elf_Data *data, const char *text)
{
	size_t oldSize = data->d_size;
	size_t newSize = data->d_size + strlen(text) + 1;
	char *buf = (char *)calloc(1, newSize);
	CHECK_ALLOC(buf);

	memcpy(buf, data->d_buf, data->d_size);
	strcpy(&buf[data->d_size], text);
	data->d_buf = buf;
	data->d_size = newSize;
	shdr->sh_size = newSize;
	return oldSize;
}

static GElf_Shdr getSectionHeader(Elf *elf, Elf64_Section index)
{
	GElf_Shdr shdr = {0};
	size_t shstrndx;
	elf_getshdrstrndx(elf, &shstrndx);
	Elf_Scn *scn = elf_getscn(elf, index);
	if (scn)
		gelf_getshdr(scn, &shdr);
	return shdr;
}

static Elf_Scn *getSectionByName(Elf *elf, const char *secName)
{
	Elf_Scn *scn = NULL;
	GElf_Shdr shdr;
	size_t shstrndx;
	elf_getshdrstrndx(elf, &shstrndx);
	while ((scn = elf_nextscn(elf, scn)) != NULL)
	{
		gelf_getshdr(scn, &shdr);
		char *name = elf_strptr(elf, shstrndx, shdr.sh_name);
		if (name != NULL && strcmp(name, secName) == 0)
			return scn;
	}
	return NULL;
}

static Elf_Scn *getRelForSectionIndex(Elf *elf, Elf64_Section index)
{
	Elf_Scn *scn = NULL;
	GElf_Shdr shdr;
	while ((scn = elf_nextscn(elf, scn)) != NULL)
	{
		gelf_getshdr(scn, &shdr);
		if (shdr.sh_type == SHT_RELA && shdr.sh_info == index)
			return scn;
	}
	return NULL;
}

static char *getSectionName(Elf *elf, Elf64_Section index)
{
	size_t shstrndx;
	elf_getshdrstrndx(elf, &shstrndx);
	GElf_Shdr shdr = getSectionHeader(elf, index);
	return elf_strptr(elf, shstrndx, shdr.sh_name);
}

static Symbol **readSymbols(Elf *elf, size_t *count)
{
	Symbol **syms;
	Elf_Scn *scn = getSectionByName(elf, ".symtab");
	GElf_Shdr shdr;
	GElf_Sym sym;
	Elf_Data *data = elf_getdata(scn, NULL);
	gelf_getshdr(scn, &shdr);
	size_t cnt = shdr.sh_size / shdr.sh_entsize;
	syms = (Symbol **)calloc(cnt + 1, sizeof(Symbol *));
	CHECK_ALLOC(syms);
	*count = 0;
	for (size_t i = 0; i < cnt; i++)
	{
		gelf_getsym(data, i, &sym);
		syms[i] = calloc(1, sizeof(Symbol));
		CHECK_ALLOC(syms[i]);
		syms[i]->sym = sym;
		// name
		syms[i]->name = elf_strptr(elf, shdr.sh_link, sym.st_name);
		// section index
		syms[i]->sym.st_shndx = sym.st_shndx;
		// is function
		if ((sym.st_info == ELF64_ST_INFO(STB_GLOBAL, STT_FUNC) ||
			 (sym.st_info == ELF64_ST_INFO(STB_LOCAL, STT_FUNC))) &&
			strlen(syms[i]->name) > 0)
		{
			syms[i]->isFun = true;
		}
		// is variable
		if (sym.st_info == ELF64_ST_INFO(STB_GLOBAL, STT_OBJECT) ||
			(sym.st_info == ELF64_ST_INFO(STB_LOCAL, STT_OBJECT)))
		{
			const char *scnName = getSectionName(elf, sym.st_shndx);
			if (strstr(scnName, ".data.") == scnName ||
				strstr(scnName, ".bss.") == scnName)
				syms[i]->isVar = true;
			if (strstr(scnName, ".rodata.") == scnName ||
				strstr(scnName, ".rodata.str") != scnName)
				syms[i]->isVar = true;
		}
		syms[i]->index = (*count)++;
	}

	return syms;
}

static GElf_Sym getSymbolByName(Elf *elf, const char *name, size_t *symIndex)
{
	Elf_Scn *scn = getSectionByName(elf, ".symtab");
	GElf_Shdr shdr;
	GElf_Sym sym = {0};
	Elf_Data *data = elf_getdata(scn, NULL);
	gelf_getshdr(scn, &shdr);
	size_t cnt = shdr.sh_size / shdr.sh_entsize;
	*symIndex = 1;
	for (size_t i = 1; i < cnt; i++)
	{
		gelf_getsym(data, i, &sym);
		if (strcmp(elf_strptr(elf, shdr.sh_link, sym.st_name), name) == 0)
			return sym;
		(*symIndex)++;
	}
	memset(&sym, 0, sizeof(sym));
	*symIndex = 0;
	return sym;
}

static bool getSymbolByNameAndType(Elf *elf, const char *symName, const int type, GElf_Sym *sym)
{
	Elf_Scn *scn = getSectionByName(elf, ".symtab");
	GElf_Shdr shdr;
	Elf_Data *data = elf_getdata(scn, NULL);
	gelf_getshdr(scn, &shdr);
	size_t cnt = shdr.sh_size / shdr.sh_entsize;
	for (size_t i = 0; i < cnt; i++)
	{
		gelf_getsym(data, i, sym);
		if ((sym->st_info == ELF64_ST_INFO(STB_LOCAL, type) ||
			 sym->st_info == ELF64_ST_INFO(STB_GLOBAL, type)) &&
			strcmp(elf_strptr(elf, shdr.sh_link, sym->st_name), symName) == 0)
			return true;
	}
	return false;
}

static GElf_Sym getSymbolByIndex(Elf *elf, size_t index)
{
	Elf_Scn *scn = getSectionByName(elf, ".symtab");
	GElf_Shdr shdr;
	GElf_Sym sym = {0};
	Elf_Data *data = elf_getdata(scn, NULL);
	gelf_getshdr(scn, &shdr);
	size_t cnt = shdr.sh_size / shdr.sh_entsize;
	if (index < cnt)
		gelf_getsym(data, index, &sym);
	return sym;
}

static uint16_t getSymbolIndexByName(Elf *elf, const char *symName)
{
	Elf_Scn *scn = getSectionByName(elf, ".symtab");
	GElf_Sym sym;
	GElf_Shdr shdr;
	Elf_Data *data = elf_getdata(scn, NULL);
	gelf_getshdr(scn, &shdr);
	size_t cnt = shdr.sh_size / shdr.sh_entsize;
	for (size_t i = 0; i < cnt; i++)
	{
		gelf_getsym(data, i, &sym);
		if (strcmp(elf_strptr(elf, shdr.sh_link, sym.st_name), symName) == 0)
			return i;
	}
	return 0;
}

static Symbol *getSymbolForRelocation(Context *ctx, const GElf_Rela rela)
{
	size_t symIndex = ELF64_R_SYM(rela.r_info);
	if (ctx->symbols[symIndex]->sym.st_shndx == 0)
		return ctx->symbols[symIndex];
	if (ctx->symbols[symIndex]->sym.st_size > 0)
		return ctx->symbols[symIndex];
	if (ELF64_ST_TYPE(ctx->symbols[symIndex]->sym.st_info) == STT_FUNC ||
		ELF64_ST_TYPE(ctx->symbols[symIndex]->sym.st_info) == STT_OBJECT)
		return ctx->symbols[symIndex];

	size_t secIndex = ctx->symbols[symIndex]->sym.st_shndx;
	Elf64_Sxword addend = rela.r_addend;
	switch (ELF64_R_TYPE(rela.r_info))
	{
	case R_X86_64_PC32:
	case R_X86_64_PLT32:
		addend += 4;
		break;
	}

	/*
	 * if the relocation with r_addned X points to a section, find a symbol that also points to the
	 * same section by checking its offset (st_value)
	 */
	for (Symbol **s = ctx->symbols; *s != NULL; s++)
	{
		if (s[0]->index != symIndex && s[0]->sym.st_shndx == secIndex &&
			(size_t)addend >= s[0]->sym.st_value &&
			(size_t)addend < s[0]->sym.st_value + s[0]->sym.st_size)
			return s[0];
	}

	// example: referer to symbol (st_value == st_size == 0) that points to .rodata.str1.1
	return ctx->symbols[symIndex];
}

static Elf_Data *getSymbolData(Elf *elf, const char *name, char type)
{
	Elf_Scn *scn = getSectionByName(elf, ".symtab");
	GElf_Shdr shdr;
	GElf_Sym sym;
	size_t secCount;
	elf_getshdrnum(elf, &secCount);
	Elf_Data *data = elf_getdata(scn, NULL);
	gelf_getshdr(scn, &shdr);
	size_t cnt = shdr.sh_size / shdr.sh_entsize;
	for (size_t i = 0; i < cnt; i++)
	{
		gelf_getsym(data, i, &sym);
		if (strcmp(elf_strptr(elf, shdr.sh_link, sym.st_name), name) == 0)
		{
			if (ELF64_ST_TYPE(sym.st_info) == type &&
				sym.st_size > 0 && sym.st_shndx < secCount)
			{
				Elf_Scn *scn = elf_getscn(elf, sym.st_shndx);
				return elf_getdata(scn, NULL);
			}
		}
	}
	return NULL;
}

static GElf_Sym getSymbolByOffset(Elf *elf, Elf64_Section shndx, size_t offset,
								  bool exactSymbol)
{
	Elf_Scn *scn = getSectionByName(elf, ".symtab");
	GElf_Shdr shdr;
	GElf_Sym sym = { 0 };
	Elf_Data *data = elf_getdata(scn, NULL);
	gelf_getshdr(scn, &shdr);
	size_t cnt = shdr.sh_size / shdr.sh_entsize;
	for (size_t i = 0; i < cnt; i++)
	{
		gelf_getsym(data, i, &sym);
		if (sym.st_name != 0 && sym.st_shndx == shndx)
		{
			if (exactSymbol && sym.st_value == offset)
				return sym;
			else if (!exactSymbol && offset >= sym.st_value &&
					 offset < sym.st_value + sym.st_size)
				return sym;
		}
	}
	memset(&sym, 0, sizeof(sym));
	return sym;
}

static GElf_Sym getSymbolForRelocAtOffset(Elf *elf, Elf64_Section sec,
										  size_t offset,
										  uint32_t *outSymOffset)
{
	GElf_Rela rela;
	GElf_Shdr shdr;
	GElf_Sym invalidSym = { 0 };
	Elf_Scn *scn = getRelForSectionIndex(elf, sec);
	if(scn == NULL)
		return invalidSym;
	Elf_Data *data = elf_getdata(scn, NULL);
	gelf_getshdr(scn, &shdr);
	size_t cnt = shdr.sh_size / shdr.sh_entsize;
	for (size_t i = 0; i < cnt; i++)
	{
		gelf_getrela(data, i, &rela);
		if (rela.r_offset == offset)
		{
			GElf_Sym sym = getSymbolByIndex(elf, ELF64_R_SYM(rela.r_info));
			if (sym.st_name != 0 &&
				(rela.r_addend == 0 || rela.r_addend == -4 || rela.r_addend == -5) &&
				ELF64_ST_TYPE(sym.st_info) != STT_SECTION)
				return sym;
			if (ELF64_R_TYPE(rela.r_info) == R_X86_64_PC32 ||
				ELF64_R_TYPE(rela.r_info) == R_X86_64_PLT32)
				rela.r_addend += 4;
			sym = getSymbolByOffset(elf, sym.st_shndx, rela.r_addend,
									outSymOffset == NULL);
			if (!invalidSym(sym))
			{
				if (outSymOffset != NULL)
					*outSymOffset = rela.r_addend - sym.st_value;
				return sym;
			}
		}
	}
	return invalidSym;
}

static int getSymbolsForRelocations(Elf *elf, Elf64_Section sec,
								    GElf_Sym *results, GElf_Rela **relocs)
{
	GElf_Rela rela;
	GElf_Shdr shdr;
	Elf_Scn *scn = getRelForSectionIndex(elf, sec);
	if(scn == NULL)
		return 0;
	Elf_Data *data = elf_getdata(scn, NULL);
	gelf_getshdr(scn, &shdr);
	size_t cnt = shdr.sh_size / shdr.sh_entsize;
	GElf_Rela *relas = *relocs = calloc(cnt, sizeof(GElf_Rela));
	CHECK_ALLOC(relas);
	for (size_t i = 0; i < cnt; i++)
	{
		gelf_getrela(data, i, &rela);
		GElf_Sym sym = getSymbolByIndex(elf, ELF64_R_SYM(rela.r_info));
		if (sym.st_name != 0 &&
			(rela.r_addend == 0 || rela.r_addend == -4 || rela.r_addend == -5) &&
			ELF64_ST_TYPE(sym.st_info) != STT_SECTION)
		{
			*results = sym;
			results++;
			memcpy(relas, &rela, sizeof(rela));
			relas++;
			continue;
		}
		if (ELF64_R_TYPE(rela.r_info) == R_X86_64_PC32 ||
			ELF64_R_TYPE(rela.r_info) == R_X86_64_PLT32)
			rela.r_addend += 4;
		sym = getSymbolByOffset(elf, sym.st_shndx, rela.r_addend, true);
		if (!invalidSym(sym))
		{
			*results = sym;
			results++;
			memcpy(relas, &rela, sizeof(rela));
			relas++;
		}
	}

	return relas - *relocs;
}

Symbol *getSymbolForSym(Context *ctx, GElf_Sym *sym)
{
	for (Symbol **s = ctx->symbols; *s != NULL; s++)
	{
		if (memcmp(&s[0]->sym, sym, sizeof(*sym)) == 0)
			return s[0];
	}
	return NULL;
}

static char *appendFormatString(char *buf, const char *format, ...)
{
	char localBuf[512];
	va_list args;
	va_start (args, format);
	int len = vsnprintf(localBuf, sizeof(localBuf), format, args);
	va_end (args);

	buf = realloc(buf, strlen(buf) + len + 1);
	strcat(buf, localBuf);
	return buf;
}

static int disasmPrintf(void *buf, const char *format, ...)
{
	char localBuf[MAX_DISASS_LINE_LEN];
	va_list args;
	va_start (args, format);
	int len = vsnprintf(localBuf, sizeof(localBuf), format, args);
	va_end (args);

	char **buffer = (char **)buf;
	buffer[0] = realloc(buffer[0], strlen(buffer[0]) + len + 1);
	strcat(buffer[0], localBuf);
	return len;
}

static int nullDisasmPrintf(void *buf, const char *format, ...)
{
	(void) buf;
	(void) format;
	return 0;
}

#ifdef DISASSEMBLY_STYLE_SUPPORT
static int fprintfStyled(void *buf, enum disassembler_style style,
						  const char *fmt, ...)
{
	va_list args;
	int r;
	char localBuf[MAX_DISASS_LINE_LEN];
	(void)style;

	va_start(args, fmt);
	r = vsprintf(localBuf, fmt, args);
	va_end(args);

	char **buffer = (char **)buf;
	buffer[0] = realloc(buffer[0], strlen(buffer[0]) + strlen(localBuf) + 1);
	strcat(buffer[0], localBuf);

	return r;
}

static int nullFprintfStyled(void *buf, enum disassembler_style style,
						  const char *fmt, ...)
{
	(void)buf;
	(void)style;
	(void)fmt;
	return 0;
}
#endif /* DISASSEMBLY_STYLE_SUPPORT */

static GElf_Sym getSymbolAtAddress(bfd_vma vma, DisasmData *data,
								   uint8_t *offset, uint8_t *size,
								   uint32_t *outSymOffset)
{
	GElf_Sym sym;
	uint8_t *inst = data->symData + data->pc;
	int32_t operand = 0;
	uint8_t operandOff = 0;
	uint8_t operandSize= 4;

	*outSymOffset = 0;

	if (inst[0] == 0xE8) // call
	{
		operandOff = 1;
	}
	else if (inst[0] == 0xE9) // JMP	Jump
	{
		operandOff = 1;
	}
	else if (inst[0] == 0xEA) // JMP	Jump
	{
		operandOff = 1;
		operandSize = 2;
	}
	else if (inst[0] == 0xEB) // JMP	Jump
	{
		operandOff = 1;
		operandSize = 1;
	}
	else if (inst[0] >= 0x70 && inst[0] <= 0x7F) // Jcc	Jump if condition
	{
		operandOff = 1;
		operandSize = 1;
	}
	else if (inst[0] == 0x0F && inst[1] >= 0x80 && inst[1] <= 0x8F) // Jcc	Jump if condition
	{
		operandOff = 2;
	}

	memcpy(&operand, inst + operandOff, operandSize);
	uint32_t addr = data->pc + data->sym.st_value + operandOff;
	vma += data->sym.st_value;

	// vma is for case when function refers to the .data section that have
	// relocation. E.g. call the function from global struct variable
	sym = getSymbolForRelocAtOffset(data->ctx->elf, data->sym.st_shndx,
									operandOff && operand == 0 ? addr : vma,
									NULL);

	if (invalidSym(sym) && inst[0] != 0xE8)
		sym = getSymbolForRelocAtOffset(data->ctx->elf, data->sym.st_shndx,
										operandOff ? addr : vma, outSymOffset);

	if(invalidSym(sym))
		sym = getSymbolByOffset(data->ctx->elf, data->sym.st_shndx, vma, true);

	*offset = operandOff;
	*size = operandSize;
	return sym;
}

static void printFunAtAddr(bfd_vma vma, struct disassemble_info *inf)
{
	uint8_t operandOff = 0;
	uint8_t operandSize = 0;
	uint32_t symOffset;
	DisasmData *data = (DisasmData *)inf->application_data;
	GElf_Sym sym = getSymbolAtAddress(vma, data, &operandOff, &operandSize,
									  &symOffset);
	if (invalidSym(sym))
	{
		const char *name = elf_strptr(data->ctx->elf, data->shdr.sh_link,
									  data->sym.st_name);
		(*inf->fprintf_func)(inf->stream, "<%s+0x%lX>", name, vma);
		return;
	}

	const char *name = elf_strptr(data->ctx->elf, data->shdr.sh_link, sym.st_name);
	if (strlen(name) == 0)
		LOG_ERR("Can't find function for instruction at offset: 0x%lx on "
				"disassembling %s", data->pc,
				elf_strptr(data->ctx->elf, data->shdr.sh_link, data->sym.st_name));

	int32_t operand = 0;
	memcpy(&operand, data->symData + data->pc + operandOff, operandSize);
	vma += data->sym.st_value;

	if (symOffset != 0)
		(*inf->fprintf_func)(inf->stream, "<%s+0x%X>", name, symOffset);
	else if (operand == 0 || vma == 0 || vma == sym.st_value)
		(*inf->fprintf_func)(inf->stream, "%s", name);
	else
		(*inf->fprintf_func)(inf->stream, "<%s+0x%lX>", name,
							 vma - data->sym.st_value);
}

disassembler_ftype initDisassembler(DisasmData *data, void *stream,
									fprintf_ftype fprintfFunc,
									disassemble_info *disasmInfo,
				  void (*printAddressFunc) (bfd_vma, struct disassemble_info *)
#ifdef DISASSEMBLY_STYLE_SUPPORT
				   					, fprintf_styled_ftype fprintfStyledFunc
#endif /* DISASSEMBLY_STYLE_SUPPORT */
									)
{
#ifdef DISASSEMBLY_STYLE_SUPPORT
	init_disassemble_info(disasmInfo, stream, fprintfFunc, fprintfStyledFunc);
#else
	init_disassemble_info(disasmInfo, stream, fprintfFunc);
#endif /* DISASSEMBLY_STYLE_SUPPORT */
	disasmInfo->arch = bfd_arch_i386;
	disasmInfo->mach = bfd_mach_x86_64;
	disasmInfo->read_memory_func = buffer_read_memory;
	disasmInfo->buffer = data->symData;
	disasmInfo->buffer_vma = 0;
	disasmInfo->buffer_length = data->sym.st_size;
	disasmInfo->application_data = (void *)data;
	disasmInfo->print_address_func = printAddressFunc;
	disassemble_init_for_target(disasmInfo);

	return disassembler(bfd_arch_i386, false, bfd_mach_x86_64, NULL);
}

char *disassembleBytes(DisasmData *data)
{
	char *buf[] = { calloc(0, 1) };
	disassemble_info disasmInfo = { 0 };
#ifdef DISASSEMBLY_STYLE_SUPPORT
	disassembler_ftype disasm = initDisassembler(data, buf, disasmPrintf,
				   								 &disasmInfo, printFunAtAddr,
												 fprintfStyled);
#else
	disassembler_ftype disasm = initDisassembler(data, buf, disasmPrintf,
				   								 &disasmInfo, printFunAtAddr);
#endif /* DISASSEMBLY_STYLE_SUPPORT */

	data->pc = 0;
	while (data->pc < data->sym.st_size)
	{
		data->pc += disasm(data->pc, &disasmInfo);
		disasmInfo.fprintf_func(disasmInfo.stream, "\n");
	}

	return buf[0];
}

static int getSymbolIndex(Elf *elf, GElf_Sym *sym)
{
	Elf_Scn *symScn = getSectionByName(elf, ".symtab");
	GElf_Shdr symShdr;
	GElf_Sym symSymtab;
	Elf_Data *symData = elf_getdata(symScn, NULL);
	gelf_getshdr(symScn, &symShdr);
	for (size_t i = 0; i < symShdr.sh_size / symShdr.sh_entsize; i++)
	{
		gelf_getsym(symData, i, &symSymtab);
		if (memcmp(sym, &symSymtab, sizeof(*sym)) == 0)
			return i;
	}
	LOG_ERR("Invalid index for symbol %u", sym->st_name);
}

static void checkFunAtAddr(bfd_vma vma, struct disassemble_info *inf)
{
	uint8_t operandOff = 0;
	uint8_t operandSize = 0;
	uint32_t symOffset;
	DisasmData *data = (DisasmData *)inf->application_data;
	GElf_Sym sym = getSymbolAtAddress(vma, data, &operandOff, &operandSize,
									  &symOffset);

	if (invalidSym(sym))
		return;

	if (operandSize != 4)
		return;

	// make sure it's JMP, not CALL
	uint32_t *operand = (uint32_t *)(data->symData + data->pc + operandOff);
	if (*operand == 0)
		return;

	*operand = 0;

	GElf_Shdr shdr;
	GElf_Rela rela;
	Elf_Scn *scn = getRelForSectionIndex(data->ctx->elf, data->sym.st_shndx);
	Elf_Data *outData = elf_getdata(scn, NULL);
	gelf_getshdr(scn, &shdr);
	size_t cnt = shdr.sh_size / shdr.sh_entsize;
	shdr.sh_size += shdr.sh_entsize;
	outData->d_size += shdr.sh_entsize;
	outData->d_buf = realloc(outData->d_buf, outData->d_size);

	gelf_getrela(outData->d_buf, cnt, &rela);

	uint32_t symIndex = getSymbolIndex(data->ctx->elf, &sym);
	rela.r_info = ELF64_R_INFO(symIndex, ELF64_R_TYPE(R_X86_64_PC32));
	rela.r_addend = (Elf64_Sxword) symOffset - 4;
	rela.r_offset = data->sym.st_value + data->pc + operandOff;
	gelf_update_rela(outData, cnt, &rela);

	if (!gelf_update_shdr(scn, &shdr))
		LOG_ERR("gelf_update_shdr failed");
	const char *name = elf_strptr(data->ctx->elf, data->shdr.sh_link, sym.st_name);
	if (strlen(name))
		LOG_DEBUG("Convert to relocation at 0x%lx (%s)", data->pc + operandOff, name);
}

// convert jump address to other functions to relocations
void convertToRelocations(DisasmData *data)
{
	disassemble_info disasmInfo = { 0 };
#ifdef DISASSEMBLY_STYLE_SUPPORT
	disassembler_ftype disasm = initDisassembler(data, NULL, nullDisasmPrintf,
				   								 &disasmInfo, checkFunAtAddr,
												 nullFprintfStyled);
#else
	disassembler_ftype disasm = initDisassembler(data, NULL, nullDisasmPrintf,
				   								 &disasmInfo, checkFunAtAddr);
#endif /* DISASSEMBLY_STYLE_SUPPORT */

	data->pc = 0;
	while (data->pc < data->sym.st_size)
		data->pc += disasm(data->pc, &disasmInfo);
}

static void applyStaticKeys(Elf *elf, const GElf_Sym *sym, uint8_t *bytes)
{
	GElf_Shdr shdr;
	GElf_Rela rela;
	GElf_Rela jmpRela;
	Elf_Scn *scn = getSectionByName(elf, ".rela__jump_table");
	if (!scn)
		return;
	Elf_Data *data = elf_getdata(scn, NULL);
	gelf_getshdr(scn, &shdr);
	size_t cnt = shdr.sh_size / shdr.sh_entsize;
	for (size_t i = 0; i < cnt; i++)
	{
		gelf_getrela(data, i, &rela);
		GElf_Sym rsym = getSymbolByIndex(elf, ELF64_R_SYM(rela.r_info));
		if (rsym.st_shndx != sym->st_shndx)
			continue;
		if (rela.r_offset % 16 != 0)
			continue;
		if (rela.r_addend < (Elf64_Sxword) sym->st_value || rela.r_addend > (Elf64_Sxword) (sym->st_value + sym->st_size))
			continue;
		gelf_getrela(data, i + 1, &jmpRela);

		uint8_t nop2[] = {0x66, 0x90};
		uint8_t nop4[] = {0x0f, 0x1f, 0x40, 0x00};
		uint8_t nop5[] = {0x0f, 0x1f, 0x44, 0x00, 0x00};

		if (memcmp(bytes + rela.r_addend, &nop2, sizeof(nop2)) == 0) // 2-bytes nop
		{
			bytes[rela.r_addend] = 0xEB;
			*(uint8_t *)(bytes + rela.r_addend + 1) = jmpRela.r_addend - rela.r_addend - 2;
		}
		else if (memcmp(bytes + rela.r_addend, &nop4, sizeof(nop4)) == 0) // 4-bytes nop
		{
			// TODO: Validate this case
			bytes[rela.r_addend] = 0xEA;
			*(uint16_t *)(bytes + rela.r_addend + 1) = jmpRela.r_addend - rela.r_addend - 3;
		}
		else if (memcmp(bytes + rela.r_addend, &nop5, sizeof(nop5)) == 0) // 5-bytes nop
		{
			bytes[rela.r_addend] = 0xE9;
			*(uint32_t *)(bytes + rela.r_addend + 1) = jmpRela.r_addend - rela.r_addend - 5;
		}
		else if (bytes[rela.r_addend] != 0xEB && bytes[rela.r_addend] != 0xEA &&
				 bytes[rela.r_addend] != 0xE9)
		{
			const char *name = elf_strptr(elf, shdr.sh_link, sym->st_name);
			LOG_ERR("Unrecognized static_key at index %zu for %s [%zu] "
					"(0x%x 0x%x 0x%x 0x%x)", i, name, sym->st_value,
					bytes[rela.r_addend], bytes[rela.r_addend + 1],
					bytes[rela.r_addend + 2], bytes[rela.r_addend + 3]);
		}
	}
}

static uint32_t calcRelSymHash(Elf *elf, const GElf_Sym *sym)
{
	uint32_t crc = 0;
	GElf_Rela rela;
	GElf_Shdr shdr;
	Elf_Scn *scn = getSectionByName(elf, ".symtab");
	gelf_getshdr(scn, &shdr);
	Elf64_Word symtabLink = shdr.sh_link;

	scn = getRelForSectionIndex(elf, sym->st_shndx);
	if (scn == NULL)
		return crc;

	Elf_Data *data = elf_getdata(scn, NULL);
	gelf_getshdr(scn, &shdr);
	size_t cnt = shdr.sh_size / shdr.sh_entsize;
	for (size_t i = 0; i < cnt; i++)
	{
		gelf_getrela(data, i, &rela);
		if (rela.r_offset < sym->st_value || rela.r_offset > sym->st_value + sym->st_size)
			continue;

		char *name = "";
		GElf_Sym rsym = getSymbolByIndex(elf, ELF64_R_SYM(rela.r_info));
		if (ELF64_ST_TYPE(rsym.st_info) != STT_SECTION)
		{
			name = elf_strptr(elf, symtabLink, rsym.st_name);
		}
		else
		{
			GElf_Shdr shdr = getSectionHeader(elf, rsym.st_shndx);
			if (shdr.sh_flags & (SHF_MERGE | SHF_STRINGS))
			{
				Elf_Scn *scn = elf_getscn(elf, rsym.st_shndx);
				Elf_Data *data = elf_getdata(scn, NULL);
				GElf_Shdr shdr;
				gelf_getshdr(scn, &shdr);
				if ((Elf64_Sxword)shdr.sh_size > rela.r_addend)
					name = (char *)data->d_buf + rela.r_addend;
			}
			else
			{
				if (ELF64_R_TYPE(rela.r_info) == R_X86_64_PC32 || \
					ELF64_R_TYPE(rela.r_info) == R_X86_64_PLT32)
					rela.r_addend += 4;
				rsym = getSymbolByOffset(elf, rsym.st_shndx, rela.r_addend,
										 true);
				name = elf_strptr(elf, symtabLink, rsym.st_name);
			}
		}
		crc += rela.r_offset - sym->st_value;
		crc += crc32((uint8_t *)name, strlen(name));
	}

	return crc;
}

static bool equalFunctions(Elf *elf, Elf *secondElf, const char *funName)
{
	GElf_Sym sym1;
	GElf_Sym sym2;
	getSymbolByNameAndType(elf, funName, STT_FUNC, &sym1);
	getSymbolByNameAndType(secondElf, funName, STT_FUNC, &sym2);
	if (sym1.st_size != sym2.st_size)
		return false;

	Elf_Scn *scn1 = elf_getscn(elf, sym1.st_shndx);
	Elf_Data *data1 = elf_getdata(scn1, NULL);
	uint8_t *symData1 = (uint8_t *)data1->d_buf + sym1.st_value;
	Elf_Scn *scn2 = elf_getscn(secondElf, sym2.st_shndx);
	Elf_Data *data2 = elf_getdata(scn2, NULL);
	uint8_t *symData2 = (uint8_t *)data2->d_buf + sym2.st_value;
	applyStaticKeys(elf, &sym1, (uint8_t *)data1->d_buf);
	applyStaticKeys(secondElf, &sym2, (uint8_t *)data2->d_buf);

	if (memcmp(symData1, symData2, sym2.st_size) != 0)
	{
		Context ctx = initContext(elf, NULL);
		GElf_Shdr shdr;
		gelf_getshdr(getSectionByName(elf, ".symtab"), &shdr);
		DisasmData data1 = { .ctx = &ctx, .sym = sym1, .shdr = shdr, .symData = symData1 };
		char *disassembled1 = disassembleBytes(&data1);

		ctx.elf = secondElf;
		gelf_getshdr(getSectionByName(secondElf, ".symtab"), &shdr);
		DisasmData data2 = { .ctx = &ctx, .sym = sym2, .shdr = shdr, .symData = symData2 };
		char *disassembled2 = disassembleBytes(&data2);

		bool isEqual;
		// skip checking first line if it might be a call to __fentry__ in the runtime
		if (strstr(disassembled1, "nop") == disassembled1 ||
			strstr(disassembled2, "nop") == disassembled2)
			isEqual = strcmp(strchr(disassembled1, '\n'), strchr(disassembled2, '\n')) == 0;
		else
			isEqual = strcmp(disassembled1, disassembled2) == 0;
		free(disassembled1);
		free(disassembled2);
		freeContext(&ctx);
		if (!isEqual)
			return false;
	}
	return calcRelSymHash(elf, &sym1) == calcRelSymHash(secondElf, &sym2);
}

static void checkNearJmpXReference(bfd_vma vma, struct disassemble_info *inf)
{
	uint8_t operandOff = 0;
	uint8_t operandSize = 0;
	uint32_t symOffset;
	DisasmData *data = (DisasmData *)inf->application_data;
	GElf_Sym sym = getSymbolAtAddress(vma, data, &operandOff, &operandSize,
									  &symOffset);

	if (invalidSym(sym))
		return;

	if (operandSize != 4)
	{
		if (memcmp(&data->sym, &sym, sizeof(sym)) != 0)
		{
			uint32_t symIndex = getSymbolIndex(data->ctx->elf, &sym);
			if (data->ctx->symbols[symIndex]->data != (void *) DIFF_NO_DIFF)
				return;

			data->ctx->symbols[symIndex]->data = (void *) DIFF_MOD_FUN;
			const char *name1 = elf_strptr(data->ctx->elf, data->shdr.sh_link,
										   data->sym.st_name);
			const char *name2 = elf_strptr(data->ctx->elf, data->shdr.sh_link,
										   sym.st_name);
			LOG_DEBUG("A close jump to a neighbouring function with a jump of"
					  "less than 4 bytes was detected (%s -> %s)", name1,
					  name2);
		}
	}
}

// if jump to other functions is not 5-byte instruction (can't be converted to
// relocation) then mark target function as modified
static void findNearJmpXReferences(Context *ctx, GElf_Sym *sym)
{
	GElf_Shdr shdr;
	gelf_getshdr(getSectionByName(ctx->elf, ".symtab"), &shdr);
	Elf_Scn *scn = elf_getscn(ctx->elf, sym->st_shndx);
	Elf_Data *data = elf_getdata(scn, NULL);
	uint8_t *symData = (uint8_t *)data->d_buf + sym->st_value;
	DisasmData dissData = { .sym = *sym, .shdr = shdr, .symData = symData,
							.ctx = ctx };

	disassemble_info disasmInfo = { 0 };
#ifdef DISASSEMBLY_STYLE_SUPPORT
	disassembler_ftype disasm = initDisassembler(&dissData, NULL,
												 nullDisasmPrintf, &disasmInfo,
												 checkNearJmpXReference,
												 nullFprintfStyled);
#else
	disassembler_ftype disasm = initDisassembler(&dissData, NULL,
												 nullDisasmPrintf, &disasmInfo,
												 checkNearJmpXReference);
#endif /* DISASSEMBLY_STYLE_SUPPORT */

	dissData.pc = 0;
	while (dissData.pc < sym->st_size)
		dissData.pc += disasm(dissData.pc, &disasmInfo);
}

static void checkRelatedSymbols(bfd_vma vma, struct disassemble_info *inf)
{
	uint8_t operandOff = 0;
	uint8_t operandSize = 0;
	uint32_t symOffset;
	DisasmData *data = (DisasmData *)inf->application_data;
	GElf_Sym sym = getSymbolAtAddress(vma, data, &operandOff, &operandSize,
									  &symOffset);

	if (invalidSym(sym))
		return;

	GElf_Sym *targetSym = (GElf_Sym *)data->data;
	if (memcmp(targetSym, &sym, sizeof(sym)) == 0)
	{
		Symbol *symbol = getSymbolForSym(data->ctx, &data->sym);
		symbol->data = (void *) true;
	}
}

// find all symbols referred to by a specific function
static void findSymbolsRelatingTo(Context *ctx, GElf_Sym *sym)
{
	GElf_Shdr shdr;
	gelf_getshdr(getSectionByName(ctx->elf, ".symtab"), &shdr);

	for (Symbol **s = ctx->symbols; *s != NULL; s++)
	{
		if (!s[0]->isFun)
			continue;

		Elf_Scn *scn = elf_getscn(ctx->elf, s[0]->sym.st_shndx);
		Elf_Data *data = elf_getdata(scn, NULL);
		uint8_t *symData = (uint8_t *)data->d_buf + s[0]->sym.st_value;

		DisasmData dissData = { .sym = s[0]->sym, .shdr = shdr,
								.symData = symData, .data = sym, .ctx = ctx };

		disassemble_info disasmInfo = { 0 };
#ifdef DISASSEMBLY_STYLE_SUPPORT
		disassembler_ftype disasm = initDisassembler(&dissData, NULL,
													nullDisasmPrintf, &disasmInfo,
													checkRelatedSymbols,
													nullFprintfStyled);
#else
		disassembler_ftype disasm = initDisassembler(&dissData, NULL,
													nullDisasmPrintf, &disasmInfo,
													checkRelatedSymbols);
#endif /* DISASSEMBLY_STYLE_SUPPORT */

		dissData.pc = 0;
		while (dissData.pc < s[0]->sym.st_size)
			dissData.pc += disasm(dissData.pc, &disasmInfo);
	}
}

static char *findModifiedSymbols(Context *ctx)
{
	Elf_Scn *scn = getSectionByName(ctx->elf, ".symtab");
	GElf_Shdr shdr;
	GElf_Sym sym;
	Elf_Data *data = elf_getdata(scn, NULL);
	gelf_getshdr(scn, &shdr);
	size_t cnt = shdr.sh_size / shdr.sh_entsize;

	for (size_t i = 0; i < ctx->symbolsCount; i++)
		ctx->symbols[i]->data = (void *) DIFF_NO_DIFF;

	for (size_t i = 0; i < cnt; i++)
	{
		gelf_getsym(data, i, &sym);
		if (sym.st_size == 0 || sym.st_shndx == 0 ||
			sym.st_shndx >= ctx->symbolsCount || sym.st_name == 0)
			continue;
		const char *name = elf_strptr(ctx->elf, shdr.sh_link, sym.st_name);
		if (ELF64_ST_TYPE(sym.st_info) == STT_FUNC)
		{
			GElf_Sym secondSym;
			if (!getSymbolByNameAndType(ctx->elfOut, name, STT_FUNC, &secondSym))
			{
				ctx->symbols[i]->data = (void *) DIFF_NEW_FUN;
			}
			else
			{
				if (!equalFunctions(ctx->elf, ctx->elfOut, name))
					ctx->symbols[i]->data = (void *) DIFF_MOD_FUN;
			}
		}
		else if (ELF64_ST_TYPE(sym.st_info) == STT_OBJECT)
		{
			GElf_Sym secondSym;
			if (!getSymbolByNameAndType(ctx->elfOut, name, STT_OBJECT,
										&secondSym))
			{
				char *bssName = malloc(strlen(name) + 6);
				CHECK_ALLOC(bssName);
				char *dataName = malloc(strlen(name) + 7);
				CHECK_ALLOC(dataName);
				char *rodataName = malloc(strlen(name) + 9);
				CHECK_ALLOC(rodataName);
				snprintf(bssName, strlen(name) + 6, ".bss.%s", name);
				snprintf(dataName, strlen(name) + 7, ".data.%s", name);
				snprintf(rodataName, strlen(name) + 9, ".rodata.%s", name);

				const char *scnName = getSectionName(ctx->elf, sym.st_shndx);
				if (strcmp(scnName, bssName) == 0 ||
					strcmp(scnName, dataName) == 0 ||
					strcmp(scnName, rodataName) == 0 ||
					strcmp(scnName, ".bss") == 0 ||
					strcmp(scnName, ".data") == 0 ||
					strcmp(scnName, ".rodata") == 0)
					ctx->symbols[i]->data = (void *) DIFF_NEW_VAR;

				free(rodataName);
				free(dataName);
				free(bssName);
			}
			else if (strstr(name, "__func__") == name)
			{
				ctx->symbols[i]->data = (void *) DIFF_NEW_VAR;
			}
		}
	}

	int diffCount;
	do
	{
		diffCount = 0;
		for (size_t i = 0; i < ctx->symbolsCount; i++)
		{
			if (ctx->symbols[i]->data != (void *) DIFF_NO_DIFF)
				diffCount++;
		}
		for (size_t i = 0; i < ctx->symbolsCount; i++)
		{
			if (ctx->symbols[i]->data == (void *) DIFF_NEW_FUN ||
				ctx->symbols[i]->data == (void *) DIFF_MOD_FUN)
				findNearJmpXReferences(ctx, &ctx->symbols[i]->sym);
		}
		for (size_t i = 0; i < ctx->symbolsCount; i++)
		{
			if (ctx->symbols[i]->data != (void *) DIFF_NO_DIFF)
				diffCount--;
		}
	} while(diffCount);


	char *result = calloc(1, sizeof(char));
	for (size_t i = 0; i < ctx->symbolsCount; i++)
	{
		DiffResult diff = (uint64_t) ctx->symbols[i]->data;
		switch (diff)
		{
			case DIFF_MOD_VAR:
				result = appendFormatString(result, "Modified variable: %s\n",
											ctx->symbols[i]->name);
				break;
			case DIFF_NEW_VAR:
				result = appendFormatString(result, "New variable: %s\n",
											ctx->symbols[i]->name);
				break;
			case DIFF_MOD_FUN:
				result = appendFormatString(result, "Modified function: %s\n",
											ctx->symbols[i]->name);
				break;
			case DIFF_NEW_FUN:
				result = appendFormatString(result, "New function: %s\n",
											ctx->symbols[i]->name);
				break;
			case DIFF_NO_DIFF:
			break;
		}
	}

	return result;
}

static Elf *createNewElf(const char *outFile)
{
	int fd = open(outFile, O_RDWR|O_TRUNC|O_CREAT, 0666);
	if (fd == -1)
		LOG_ERR("Failed to create file: %s", outFile);

	Elf *elf = elf_begin(fd, ELF_C_WRITE, 0);

	Elf64_Ehdr *m_ehdr = elf64_newehdr(elf);

	m_ehdr->e_ident[EI_MAG0] = ELFMAG0;
	m_ehdr->e_ident[EI_MAG1] = ELFMAG1;
	m_ehdr->e_ident[EI_MAG2] = ELFMAG2;
	m_ehdr->e_ident[EI_MAG3] = ELFMAG3;
	m_ehdr->e_ident[EI_CLASS] = ELFCLASS64;
	m_ehdr->e_ident[EI_DATA] = ELFDATA2LSB;
	m_ehdr->e_ident[EI_VERSION] = EV_CURRENT;
	m_ehdr->e_machine = EM_X86_64;
	m_ehdr->e_type = ET_REL;
	m_ehdr->e_version = EV_CURRENT;
	m_ehdr->e_shstrndx = 1;

	GElf_Shdr shdr;
	Elf_Scn *strtabScn = elf_newscn(elf);
	Elf_Data *newData = elf_newdata(strtabScn);
	gelf_getshdr(strtabScn, &shdr);
	static uint8_t blank[1] = {'\0'};
	newData->d_buf = blank;
	newData->d_size = 1;
	Elf64_Word shname = appendString(&shdr, newData, ".strtab");
	Elf64_Word symtabname = appendString(&shdr, newData, ".symtab");
	shdr.sh_type = SHT_STRTAB;
	shdr.sh_name = appendString(&shdr, newData, ".shstrtab");
	gelf_update_shdr(strtabScn, &shdr);

	Elf_Scn *shstrtabScn = elf_newscn(elf);
	newData = elf_newdata(shstrtabScn);
	newData->d_buf = blank;
	newData->d_size = 1;
	gelf_getshdr(shstrtabScn, &shdr);
	shdr.sh_size = 1;
	shdr.sh_type = SHT_STRTAB;
	shdr.sh_name = shname;
	gelf_update_shdr(shstrtabScn, &shdr);

	Elf_Scn *symtabScn = elf_newscn(elf);
	newData = elf_newdata(symtabScn);
	gelf_getshdr(symtabScn, &shdr);

	GElf_Sym emptySym = {0};
	newData->d_type = ELF_T_SYM;
	newData->d_buf = malloc(sizeof(GElf_Sym));
	CHECK_ALLOC(newData->d_buf);
	memcpy(newData->d_buf, &emptySym, sizeof(GElf_Sym));
	newData->d_size = sizeof(GElf_Sym);

	shdr.sh_size = sizeof(GElf_Sym);
	shdr.sh_link = elf_ndxscn(getSectionByName(elf, ".strtab"));
	shdr.sh_type = SHT_SYMTAB;
	shdr.sh_name = symtabname;
	shdr.sh_entsize = sizeof(GElf_Sym);
	gelf_update_shdr(symtabScn, &shdr);

	return elf;
}

void markSymbolsToCopy(Context *ctx, bool *symToCopy, const char *symbols)
{
	GElf_Sym sym;
	size_t symIndex;

	// mark the symbols to be copied from "symbols" text parameter
	while (symbols[0] != '\0')
	{
		char *comma = strchr(symbols, ',');
		if (comma)
			*comma = '\0';
		const char *symbol = symbols;

		sym = getSymbolByName(ctx->elf, symbol, &symIndex);
		if (sym.st_name == 0)
			LOG_ERR("Can't find symbol: %s", symbol);
		symToCopy[symIndex] = true;

		if (!comma)
			break;

		symbols = comma + 1;
	}

	// mark the symbol from the "key" field in the static_key to be copied
	Elf_Scn *scn = getSectionByName(ctx->elf, "__jump_table");
	if (scn == NULL)
		return;

	Elf_Scn *relScn = getRelForSectionIndex(ctx->elf, elf_ndxscn(scn));
	if (relScn == NULL)
		LOG_ERR("Can't find relocation section for __jump_table");

	GElf_Rela rela;
	GElf_Shdr shdr;
	Elf_Data *data = elf_getdata(relScn, NULL);
	gelf_getshdr(relScn, &shdr);
	size_t cnt = shdr.sh_size / shdr.sh_entsize;
	for (size_t i = 0; i < cnt; i += 3)
	{
		gelf_getrela(data, i, &rela);
		Symbol *symbol = getSymbolForRelocation(ctx, rela);
		if (symToCopy[symbol->index])
		{
			gelf_getrela(data, i + 2, &rela);
			Symbol *symbol = getSymbolForRelocation(ctx, rela);
			symToCopy[symbol->index] = true;
			LOG_DEBUG("Mark '%s' symbol to copy as it's a static_key", symbol->name);
		}
	}
}

static Elf_Scn *copySection(Context *ctx, Elf64_Section index, bool copyData)
{
	if (ctx->copiedScnMap[index] != NULL)
		return ctx->copiedScnMap[index];

	if (index >= ctx->sectionsCount)
		LOG_ERR("Try to copy section that is out range (%d/%ld)", index, ctx->sectionsCount);

	size_t shstrndx;
	GElf_Shdr newShdr;
	GElf_Shdr oldShdr;
	GElf_Shdr strshdr;
	elf_getshdrstrndx(ctx->elf, &shstrndx);
	Elf_Scn *strtabScn = getSectionByName(ctx->elfOut, ".shstrtab");
	Elf_Data *strData = elf_getdata(strtabScn, NULL);
	gelf_getshdr(strtabScn, &strshdr);
	Elf_Scn *oldScn = elf_getscn(ctx->elf, index);
	Elf_Data *oldData = elf_getdata(oldScn, NULL);
	Elf_Scn *newScn = elf_newscn(ctx->elfOut);
	Elf_Data *newData = elf_newdata(newScn);
	gelf_getshdr(oldScn, &oldShdr);
	gelf_getshdr(newScn, &newShdr);
	newShdr.sh_type = oldShdr.sh_type;
	newShdr.sh_flags = oldShdr.sh_flags;
	newShdr.sh_entsize = oldShdr.sh_entsize;
	newShdr.sh_name = appendString(&strshdr, strData, elf_strptr(ctx->elf, shstrndx, oldShdr.sh_name));
	newData->d_type = oldData->d_type;
	if (copyData)
	{
		newShdr.sh_size = oldShdr.sh_size;
		newData->d_buf = calloc(1, oldData->d_size);
		CHECK_ALLOC(newData->d_buf);
		newData->d_size = oldData->d_size;
		if (oldData->d_buf)
			memcpy(newData->d_buf, oldData->d_buf, oldData->d_size);
	}
	gelf_update_shdr(newScn, &newShdr);
	gelf_update_shdr(strtabScn, &strshdr);

	ctx->copiedScnMap[index] = newScn;
	return newScn;
}

static Elf64_Word copyStrtabItem(Elf *elf, Elf *outElf, size_t offset)
{
	GElf_Shdr strshdr;
	Elf_Scn *strtabScn = getSectionByName(outElf, ".strtab");
	Elf_Data *strData = elf_getdata(strtabScn, NULL);
	gelf_getshdr(strtabScn, &strshdr);
	size_t strtabIdx = elf_ndxscn(getSectionByName(elf, ".strtab"));
	char *text = elf_strptr(elf, strtabIdx, offset);
	Elf64_Word newStrOffset = appendString(&strshdr, strData, text);

	gelf_update_shdr(strtabScn, &strshdr);
	return newStrOffset;
}

static void swapSymbolIndex(Elf *elf, size_t left, size_t right)
{
	Elf_Scn *scn = NULL;
	GElf_Shdr shdr;
	size_t shstrndx;
	elf_getshdrstrndx(elf, &shstrndx);
	while ((scn = elf_nextscn(elf, scn)) != NULL)
	{
		gelf_getshdr(scn, &shdr);
		if (shdr.sh_type != SHT_RELA)
			continue;

		GElf_Rela rela;
		Elf_Data *data = elf_getdata(scn, NULL);
		size_t cnt = shdr.sh_size / shdr.sh_entsize;
		for (size_t i = 0; i < cnt; i++)
		{
			gelf_getrela(data, i, &rela);
			if (ELF64_R_SYM(rela.r_info) == left)
			{
				rela.r_info = ELF64_R_INFO(right, ELF64_R_TYPE(rela.r_info));
				gelf_update_rela(data, i, &rela);
			}
			else if (ELF64_R_SYM(rela.r_info) == right)
			{
				rela.r_info = ELF64_R_INFO(left, ELF64_R_TYPE(rela.r_info));
				gelf_update_rela(data, i, &rela);
			}
		}
	}
}

static void sortSymtab(Elf *elf)
{
	Elf_Scn *scn = getSectionByName(elf, ".symtab");
	GElf_Shdr shdr;
	GElf_Sym sym;
	Elf_Data *data = elf_getdata(scn, NULL);
	gelf_getshdr(scn, &shdr);
	int firstGlobalIndex = 0;
	size_t cnt = shdr.sh_size / shdr.sh_entsize;
	for (size_t i = 0; i < cnt; i++)
	{
		gelf_getsym(data, i, &sym);
		if (firstGlobalIndex == 0 && ELF64_ST_BIND(sym.st_info) == STB_GLOBAL)
			firstGlobalIndex = i;

		if (firstGlobalIndex != 0 && ELF64_ST_BIND(sym.st_info) == STB_LOCAL)
		{
			GElf_Sym globalSym;
			gelf_getsym(data, firstGlobalIndex, &globalSym);
			gelf_update_sym(data, i, &globalSym);
			gelf_update_sym(data, firstGlobalIndex, &sym);
			swapSymbolIndex(elf, i, firstGlobalIndex);
			firstGlobalIndex = 0;
			i = 0;
		}
	}
	// update section info
	shdr.sh_info = firstGlobalIndex;
	gelf_update_shdr(scn, &shdr);
}

static Elf64_Word appendStringToScn(Elf *elf, char *scnName, char *text)
{
	GElf_Shdr shdr;
	Elf_Scn *scn = getSectionByName(elf, scnName);
	Elf_Data *data = elf_getdata(scn, NULL);
	gelf_getshdr(scn, &shdr);
	Elf64_Word result = appendString(&shdr, data, text);
	gelf_update_shdr(scn, &shdr);
	return result;
}

static size_t copySymbol(Context *ctx, size_t index, bool copySec)
{
	GElf_Sym oldSym;
	GElf_Shdr shdr;
	GElf_Shdr outShdr;
	Elf_Scn *scn;
	Elf_Data *data;
	GElf_Sym newSym;

	if (ctx->symbols[index]->copiedIndex)
		return ctx->symbols[index]->copiedIndex;

	scn = getSectionByName(ctx->elf, ".symtab");
	data = elf_getdata(scn, NULL);
	gelf_getshdr(scn, &shdr);
	gelf_getsym(data, index, &oldSym);
	scn = getSectionByName(ctx->elfOut, ".symtab");
	gelf_getshdr(scn, &outShdr);
	size_t newIndex = outShdr.sh_size/outShdr.sh_entsize;
	data = elf_getdata(scn, NULL);
	data->d_buf = realloc(data->d_buf, data->d_size + sizeof(GElf_Sym));
	CHECK_ALLOC(data->d_buf);
	newSym = oldSym;

	char symType = ELF64_ST_TYPE(oldSym.st_info);
	if (oldSym.st_shndx > 0 && oldSym.st_shndx < ctx->sectionsCount &&
		copySec)
	{
		Elf_Scn *scn = copySection(ctx, oldSym.st_shndx, true);
		newSym.st_shndx = elf_ndxscn(scn);

		if (oldSym.st_name != 0)
		{
			newSym.st_info = ELF64_ST_INFO(STB_GLOBAL, symType);
			// TODO: Avoid modify symbol name for functions
			if (symType == STT_FUNC)
			{
				char *funName = strdup(ctx->symbols[index]->name);
				CHECK_ALLOC(funName);
				char *n;
				while ((n = strchr(funName, '.')) != NULL)
					*n = '_';
				newSym.st_name = appendStringToScn(ctx->elfOut, ".strtab", funName);
				free(funName);

				Elf_Data *data = elf_getdata(scn, NULL);
				uint8_t *symData = (uint8_t *)data->d_buf + newSym.st_value;
				DisasmData dissData = { .ctx = ctx, .sym = oldSym,
										.shdr = shdr, .symData = symData };
				convertToRelocations(&dissData);
			}
			else
			{
				newSym.st_name = appendStringToScn(ctx->elfOut, ".strtab", ctx->symbols[index]->name);
			}
		}
	}
	else // mark symbol as "external"
	{
		if (oldSym.st_shndx > 0 && oldSym.st_shndx < ctx->sectionsCount)
			newSym.st_shndx = 0;
		newSym.st_size = 0;
		newSym.st_info = ELF64_ST_INFO(STB_GLOBAL, symType);
		if (oldSym.st_name != 0)
			newSym.st_name = copyStrtabItem(ctx->elf, ctx->elfOut, oldSym.st_name);
	}

	memcpy((uint8_t *)data->d_buf + data->d_size, &newSym, sizeof(GElf_Sym));
	data->d_size += sizeof(GElf_Sym);
	outShdr.sh_size = data->d_size;
	gelf_update_shdr(scn, &outShdr);

	ctx->symbols[index]->copiedIndex = newIndex;
	return newIndex;
}

typedef bool (*RelocFilter)(Context *, GElf_Rela *, Elf_Data *, size_t);

static bool jumpTableRelocFilter(Context *ctx, GElf_Rela *unused, Elf_Data *data,
								 size_t index)
{
	GElf_Sym sym;
	GElf_Rela rela;
	size_t idx = (index / 3) * 3;

	// check if code points to fully copied function (modified function)
	gelf_getrela(data, idx, &rela);

	Symbol *symbol = getSymbolForRelocation(ctx, rela);
	if (ctx->symbols[symbol->index]->copiedIndex == 0)
		return false;

	sym = getSymbolByIndex(ctx->elfOut,
						   ctx->symbols[symbol->index]->copiedIndex);
	if (sym.st_size == 0)
		return false;

	// check if key is external symbol
	gelf_getrela(data, idx + 2, &rela);
	symbol = getSymbolForRelocation(ctx, rela);

	return symbol->sym.st_size > 0;
}

static void copyRelSection(Context *ctx, Elf64_Section index, size_t relTo,
						   GElf_Sym *fromSym, RelocFilter filter)
{
	Elf_Scn *outScn = copySection(ctx, index, false);
	GElf_Shdr shdr;
	gelf_getshdr(outScn, &shdr);
	shdr.sh_link = elf_ndxscn(getSectionByName(ctx->elfOut, ".symtab"));
	shdr.sh_info = relTo;
	gelf_update_shdr(outScn, &shdr);

	GElf_Rela rela;
	size_t j = shdr.sh_size / shdr.sh_entsize;
	Elf_Scn *scn = elf_getscn(ctx->elf, index);
	Elf_Data *data = elf_getdata(scn, NULL);
	Elf_Data *outData = elf_getdata(outScn, NULL);
	gelf_getshdr(scn, &shdr);
	size_t cnt = shdr.sh_size / shdr.sh_entsize;
	outData->d_size += shdr.sh_size;
	outData->d_buf = realloc(outData->d_buf, outData->d_size);
	CHECK_ALLOC(outData->d_buf);
	for (size_t i = 0; i < cnt; i++)
	{
		gelf_getrela(data, i, &rela);

		if (fromSym != NULL &&
			(rela.r_offset < fromSym->st_value ||
			rela.r_offset > fromSym->st_value + fromSym->st_size))
			continue;

		if (filter != NULL && !filter(ctx, &rela, data, i))
			continue;

		size_t newSymIndex;
		size_t symIndex = ELF64_R_SYM(rela.r_info);
		int rType = ELF64_R_TYPE(rela.r_info);
		GElf_Shdr shdr = getSectionHeader(ctx->elf, ctx->symbols[symIndex]->sym.st_shndx);
		const char *secName = getSectionName(ctx->elf, ctx->symbols[symIndex]->sym.st_shndx);
		if (shdr.sh_flags & SHF_STRINGS ||
			strstr(secName, ".rodata.__func__") == secName ||
			strstr(secName, ".rodata.cst16") == secName ||
			strstr(secName, "__tracepoint_str") == secName ||
			strstr(secName, "__trace_printk_fmt") == secName)
		{
			newSymIndex = copySymbol(ctx, symIndex, true);
		}
		else
		{
			Symbol *sym = fromSym == NULL ? ctx->symbols[symIndex] : getSymbolForRelocation(ctx, rela);
			bool isFuncOrVar = ctx->symbols[sym->index]->isFun || ctx->symbols[sym->index]->isVar;
			bool copySec = fromSym == NULL ? true : !isFuncOrVar;
			newSymIndex = copySymbol(ctx, sym->index, copySec);
			if (fromSym != NULL &&
				(rType == R_X86_64_PC32 || rType == R_X86_64_PLT32 ||
				 rType == R_X86_64_32S || rType == R_X86_64_64))
			{
				if (ELF64_ST_TYPE(ctx->symbols[symIndex]->sym.st_info) == STT_SECTION && \
					rela.r_addend != -4)
					rela.r_addend -= sym->sym.st_value;
			}
		}
		rela.r_info = ELF64_R_INFO(newSymIndex, rType);
		gelf_update_rela(outData, j, &rela);
		j++;
	}
	gelf_getshdr(outScn, &shdr);
	shdr.sh_size = j * shdr.sh_entsize;
	outData->d_size = shdr.sh_size;
	if (!gelf_update_shdr(outScn, &shdr))
		LOG_ERR("gelf_update_shdr failed");
}

static Elf_Scn *copySectionWithRel(Context *ctx, Elf64_Section index,
								   GElf_Sym *fromSym, RelocFilter filter)
{
	Elf_Scn *newScn = copySection(ctx, index, true);
	Elf_Scn *relScn = getRelForSectionIndex(ctx->elf, index);
	if (relScn)
		copyRelSection(ctx, elf_ndxscn(relScn), elf_ndxscn(newScn), fromSym,
					   filter);
	return newScn;
}

static void copySymbols(Context *ctx, const char *symbols)
{
	Elf_Scn *scn;
	Elf_Scn *relScn = NULL;
	GElf_Shdr shdr;
	GElf_Sym sym;
	size_t symIndex;
	bool *symToCopy = calloc(sizeof(bool), ctx->symbolsCount);
	CHECK_ALLOC(symToCopy);

	markSymbolsToCopy(ctx, symToCopy, symbols);

	for (size_t i = 0; i < ctx->symbolsCount; i++)
	{
		if (!symToCopy[i])
			continue;

		sym = getSymbolByIndex(ctx->elf, i);
		Elf_Scn *newScn = copySection(ctx, sym.st_shndx, true);
		size_t index = copySymbol(ctx, i, true);
		Elf_Scn *symScn = getSectionByName(ctx->elfOut, ".symtab");
		gelf_getshdr(symScn, &shdr);
		Elf_Data *symData = elf_getdata(symScn, NULL);
		gelf_getsym(symData, index, &sym);
		if (sym.st_shndx != 0) // __tracepoint_... symbol points to section 0
			sym.st_shndx = elf_ndxscn(newScn);
		gelf_update_sym(symData, index, &sym);
	}

	for (size_t i = 0; i < ctx->symbolsCount; i++)
	{
		if (!symToCopy[i])
			continue;

		sym = getSymbolByIndex(ctx->elf, i);
		copySectionWithRel(ctx, sym.st_shndx, &sym, NULL);
	}

	// Copy missed relocation sections
	while ((relScn = elf_nextscn(ctx->elf, relScn)) != NULL)
	{
		gelf_getshdr(relScn, &shdr);
		if (shdr.sh_type != SHT_RELA)
			continue;

		Elf_Scn *copiedScn = ctx->copiedScnMap[shdr.sh_info];
		if (!copiedScn)
			continue;

		Elf_Scn *copiedRelScn = ctx->copiedScnMap[elf_ndxscn(relScn)];
		if (copiedRelScn)
			continue;

		// at this moment copy only relocations for .rodata
		const char *secName = getSectionName(ctx->elf, shdr.sh_info);
		if (strstr(secName, ".rodata") != secName)
			continue;

		LOG_DEBUG("Copy missed %s section",
				  getSectionName(ctx->elf, elf_ndxscn(relScn)));
		GElf_Sym mockSym = {};
		mockSym.st_size = -1;
		copyRelSection(ctx, elf_ndxscn(relScn),
					   elf_ndxscn(copiedScn), &mockSym, NULL);
	}

	/**
	* TODO: Consider copy following sections:
	".smp_locks", "__ex_table", ".discard.reachable", ".discard.unreachable",
	".discard.addressable", ".discard.retpoline_safe", ".static_call_sites",
	".static_call.text", ".retpoline_sites", ".return_sites", ".orc_unwind",
	".orc_unwind_ip", ".initcall4.init", ".meminit.text", "__tracepoints"
	*/
	const char *extraSections[] = {".altinstructions", ".altinstr_aux",
								   ".altinstr_replacement", "__bug_table"
								  };
	for (size_t i = 0; i < sizeof(extraSections) / sizeof(*extraSections); i++)
	{
		scn = getSectionByName(ctx->elf, extraSections[i]);
		if (scn)
		{
			LOG_DEBUG("Copy %s section", extraSections[i]);
			size_t index = elf_ndxscn(scn);
			copySectionWithRel(ctx, index, NULL, NULL);
		}
	}

	scn = getSectionByName(ctx->elf, "__jump_table");
	if (scn)
	{
		LOG_DEBUG("Copy %s section", "__jump_table");
		size_t index = elf_ndxscn(scn);
		scn = copySectionWithRel(ctx, index, NULL, jumpTableRelocFilter);

		Elf_Scn *relScn = getRelForSectionIndex(ctx->elfOut, elf_ndxscn(scn));
		gelf_getshdr(relScn, &shdr);
		size_t cnt = shdr.sh_size / shdr.sh_entsize;

		gelf_getshdr(scn, &shdr);
		shdr.sh_size = /*sizeof(struct jump_entry)*/ 16 * cnt / 3;
		gelf_update_shdr(scn, &shdr);

		Elf_Data *data = elf_getdata(scn, NULL);
		data->d_size = shdr.sh_size;

		// update offsets
		GElf_Rela rela;
		data = elf_getdata(relScn, NULL);
		for (size_t i = 0; i < cnt; i++)
		{
			gelf_getrela(data, i, &rela);
			rela.r_offset = i / 3 * 16 + i % 3 * 4; // get sequence: 0x0, 0x4, 0x8, 0x10, 0x14, 0x18, 0x20, ...
			gelf_update_rela(data, i, &rela);
		}
	}

	// TODO: Fix file path in string sections

	sortSymtab(ctx->elfOut);

	elf_update(ctx->elfOut, ELF_C_WRITE);
	elf_end(ctx->elfOut);

	free(symToCopy);
}

static Elf *openElf(const char *filePath, int *fd)
{
	elf_version(EV_CURRENT);
	*fd = open(filePath, O_RDONLY);
	if (*fd == -1)
		error(EXIT_FAILURE, errno, "Cannot open file '%s'", filePath);

	Elf *elf = elf_begin(*fd, ELF_C_READ, NULL);
	if (elf == NULL)
		error(EXIT_FAILURE, errno, "Problems opening '%s' as ELF file: %s",
			  filePath, elf_errmsg(-1));

	size_t shstrndx;
	if (elf_getshdrstrndx(elf, &shstrndx))
		error(EXIT_FAILURE, errno, "Cannot get section header string index in %s", filePath);

	if (getSectionByName(elf, ".strtab") == NULL)
		LOG_ERR("Failed to find .strtab section");
	if (getSectionByName(elf, ".symtab") == NULL)
		LOG_ERR("Failed to find .symtab section");

	return elf;
}

static void symbolCallees(Context *ctx, Symbol *s, size_t *result)
{
	GElf_Shdr shdr;
	GElf_Rela rela;
	Elf_Scn *scn = getRelForSectionIndex(ctx->elf, s->sym.st_shndx);
	gelf_getshdr(scn, &shdr);
	Elf_Data *data = elf_getdata(scn, NULL);
	size_t cnt = shdr.sh_size / shdr.sh_entsize;
	for (size_t i = 0; i < cnt; i++)
	{
		gelf_getrela(data, i, &rela);
		size_t symIndex = ELF64_R_SYM(rela.r_info);
		if (symIndex >= ctx->symbolsCount)
			LOG_ERR("Invalid symbol index: %ld in section relocation %ld", symIndex, elf_ndxscn(scn));
		Symbol *sym = getSymbolForRelocation(ctx, rela);
		if (sym->isFun)
		{
			size_t *r = result;
			// add symIndex to result if result not contains it
			while (*r != 0)
			{
				if (*r == sym->index)
					break;
				r++;
			}
			if (*r == 0)
				*r = sym->index;
		}
	}
}

static void printCallees(Context *ctx, Symbol *s, size_t *callStack, bool *visited)
{
	size_t *stack = callStack;
	while(*stack-- != 0)
	{
		if (*stack == s->index)
			return;
	}
	if (visited[s->index])
		return;
	visited[s->index] = true;

	*callStack = s->index;
	size_t *calleeIdx = s->data;
	if (*calleeIdx == 0)
	{
		do
		{
			printf("%s ", ctx->symbols[*callStack]->name);
			callStack--;
		} while(*callStack != 0);
		puts("");
		return;
	}
	while(*calleeIdx != 0)
	{
		printCallees(ctx, ctx->symbols[*calleeIdx], callStack + 1, visited);
		calleeIdx++;
	}
}

Context initContext(Elf *elf, Elf *elfOut)
{
	Context ctx = {};
	ctx.elf = elf;
	ctx.elfOut = elfOut;
	elf_getshdrnum(elf, &ctx.sectionsCount);
	return ctx;
}

void freeContext(Context *ctx)
{
	if (ctx->symbols)
	{
		for (Symbol **s = ctx->symbols; *s != NULL; s++)
			free(s[0]);
	}

	free(ctx->symbols);
	free(ctx->copiedScnMap);
	ctx->symbols = NULL;
	ctx->copiedScnMap = NULL;
}

char *showDiff(char *firstFile, char *secondFile)
{
	int firstFd;
	int secondFd;


	Elf *firstElf = openElf(firstFile, &firstFd);
	Elf *secondElf = openElf(secondFile, &secondFd);
	Context ctx = initContext(secondElf, firstElf);
	ctx.symbols = readSymbols(secondElf, &ctx.symbolsCount);

	char *diff = findModifiedSymbols(&ctx);

	close(firstFd);
	close(secondFd);
	freeContext(&ctx);

	return diff;
}

void findCallChains(char *filePath)
{
	int fd;
	Elf *elf = openElf(filePath, &fd);

	Context ctx = initContext(elf, NULL);
	ctx.symbols = readSymbols(elf, &ctx.symbolsCount);
	for (Symbol **s = ctx.symbols; *s != NULL; s++)
	{
		if (s[0]->isFun)
		{
			s[0]->data = calloc(ctx.symbolsCount, sizeof(size_t));
			CHECK_ALLOC(s[0]->data);
			symbolCallees(&ctx, s[0], s[0]->data);
		}
	}
	size_t *callStack = malloc(ctx.symbolsCount * sizeof(size_t));
	bool *visited = malloc(ctx.symbolsCount * sizeof(bool));
	CHECK_ALLOC(callStack);
	for (Symbol **s = ctx.symbols; *s != NULL; s++)
	{
		if (s[0]->isFun)
		{
			memset(callStack, 0, ctx.symbolsCount * sizeof(size_t));
			memset(visited, 0, ctx.symbolsCount * sizeof(bool));
			printCallees(&ctx, s[0], callStack + 1, visited);
		}
	}
	for (Symbol **s = ctx.symbols; *s != NULL; s++)
	{
		if (s[0]->isFun)
		{
			free(s[0]->data);
			s[0]->data = NULL;
		}
	}
	free(callStack);
	free(visited);
	close(fd);
	freeContext(&ctx);
}

void extractSymbols(char *filePath, char *outFile, char *symToCopy)
{
	int fd;
	Elf *elf = openElf(filePath, &fd);

	Context ctx = initContext(elf, NULL);
	ctx.copiedScnMap = calloc(ctx.sectionsCount, sizeof(Elf_Scn *));
	CHECK_ALLOC(ctx.copiedScnMap);

	Elf *outElf = createNewElf(outFile);
	ctx.elfOut = outElf;
	ctx.symbols = readSymbols(elf, &ctx.symbolsCount);
	copySymbols(&ctx, symToCopy);

	freeContext(&ctx);

	close(fd);
}

size_t changeCallSymbol(char *filePath, char *fromRelSym, char *toRelSym)
{
	size_t replaced = 0;

	int fd = open(filePath, O_RDWR);
	if (fd == -1)
		error(EXIT_FAILURE, errno, "Cannot open input file '%s'", filePath);

	Elf *elf = elf_begin(fd, ELF_C_RDWR, NULL);
	if (elf == NULL)
		error(EXIT_FAILURE, errno, "Problems opening '%s' as ELF file: %s",
			  filePath, elf_errmsg(-1));

	size_t shstrndx;
	if (elf_getshdrstrndx(elf, &shstrndx))
		error(EXIT_FAILURE, errno, "Cannot get section header string index");

	Elf_Scn *scn = NULL;
	GElf_Shdr shdr;
	GElf_Rela rela;
	Elf_Data *data;

	uint16_t oldSymIndex = getSymbolIndexByName(elf, fromRelSym);
	uint16_t newSymIndex = getSymbolIndexByName(elf, toRelSym);
	if (oldSymIndex == 0)
		LOG_ERR("Can't find symbol '%s'\n", fromRelSym);
	if (newSymIndex == 0)
		LOG_ERR("Can't find symbol '%s'\n", toRelSym);

	while ((scn = elf_nextscn(elf, scn)) != NULL)
	{
		gelf_getshdr(scn, &shdr);
		if (shdr.sh_type != SHT_RELA)
			continue;
		data = elf_getdata(scn, NULL);
		Elf64_Xword cnt = shdr.sh_size / shdr.sh_entsize;
		for (Elf64_Xword i = 0; i < cnt; i++)
		{
			gelf_getrela(data, i, &rela);
			if (ELF64_R_SYM(rela.r_info) == oldSymIndex)
			{
				rela.r_info = ELF64_R_INFO(newSymIndex, ELF64_R_TYPE(rela.r_info));
				gelf_update_rela(data, i, &rela);
				replaced++;
			}
		}
	}

	if (replaced && elf_update(elf, ELF_C_WRITE) == -1)
		error(EXIT_FAILURE, errno, "elf_update failed: %s", elf_errmsg(-1));

	return replaced;
}

char *disassemble(char *filePath, char *symName, bool convertToReloc)
{
	int fd;
	Elf *elf = openElf(filePath, &fd);

	Context ctx = initContext(elf, NULL);
	GElf_Sym sym;
	if (!getSymbolByNameAndType(elf, symName, STT_FUNC, &sym))
		LOG_ERR("Can't find symbol %s", symName);

	GElf_Shdr shdr;
	gelf_getshdr(getSectionByName(elf, ".symtab"), &shdr);
	Elf_Data *data = getSymbolData(elf, symName, STT_FUNC);
	uint8_t *symData = (uint8_t *)data->d_buf + sym.st_value;
	DisasmData dissData = { .ctx = &ctx, .sym = sym, .shdr = shdr, .symData = symData };

	if (convertToReloc)
		convertToRelocations(&dissData);
	applyStaticKeys(elf, &sym, data->d_buf);

	char *disassembled = disassembleBytes(&dissData);

	freeContext(&ctx);
	close(fd);

	return disassembled;
}

char *symbolReferenceFrom(char *filePath, char *symName)
{
	int fd;
	Elf *elf = openElf(filePath, &fd);
	GElf_Sym sym;
	if (!getSymbolByNameAndType(elf, symName, STT_FUNC, &sym))
		LOG_ERR("Can't find symbol %s", symName);

	Context ctx = initContext(elf, NULL);
	ctx.symbols = readSymbols(elf, &ctx.symbolsCount);

	// find functions
	findSymbolsRelatingTo(&ctx, &sym);

	// find variables
	GElf_Shdr shdr;
	size_t shstrndx;
	elf_getshdrstrndx(elf, &shstrndx);
	Elf_Scn *scn = getSectionByName(elf, ".symtab");
	gelf_getshdr(scn, &shdr);
	int symtabLink = shdr.sh_link;

	GElf_Sym *syms = calloc(ctx.symbolsCount, sizeof(GElf_Sym));
	CHECK_ALLOC(syms);
	scn = NULL;
	while ((scn = elf_nextscn(elf, scn)) != NULL)
	{
		gelf_getshdr(scn, &shdr);
		if (shdr.sh_type != SHT_RELA)
			continue;

		Elf_Scn *parentScn = elf_getscn(elf, shdr.sh_info);
		gelf_getshdr(parentScn, &shdr);
		if (shdr.sh_type != SHT_PROGBITS || (shdr.sh_flags & SHF_ALLOC) == 0 ||
			shdr.sh_flags & SHF_EXECINSTR)
			continue;

		Elf64_Section secIndex = elf_ndxscn(parentScn);
		const char *secName = getSectionName(elf, secIndex);
		if (strstr(secName, ".discard.") == secName ||
			strstr(secName, "___ksymtab+") == secName)
			continue;

		GElf_Rela *relocs = NULL;
		int count = getSymbolsForRelocations(elf, secIndex, syms, &relocs);
		for (int i = 0; i < count; i++)
		{
			if (sym.st_name != syms[i].st_name)
				continue;
			GElf_Sym varSym = getSymbolByOffset(elf, secIndex,
												relocs[i].r_offset, false);
			Symbol *symbol = getSymbolForSym(&ctx, &varSym);
			symbol->data = (void *) true;
		}
		free(relocs);
	}

	free(syms);
	close(fd);

	char *result = calloc(1, sizeof(char));
	for (Symbol **s = ctx.symbols; *s != NULL; s++)
	{
		if (s[0]->data)
		{
			result = appendFormatString(result, "%s:%s\n",
										s[0]->isFun ? "f" : "v",
										s[0]->name);
		}
	}
	freeContext(&ctx);

	return result;
}
