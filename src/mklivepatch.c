/*
* Author: Marek Maślanka
* Project: DEKU
* URL: https://github.com/MarekMaslanka/deku
*
* Convert kernel module to livepatch module
*/

#include <errno.h>
#include <error.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <getopt.h>

#include <gelf.h>
/*
* Make livepatch procedure:
* * Add private symbols to the table (symbols not accessible from outside module/file)
* * Find the relocation sections that contain the symbols above and remove them
* * Add the symbol names from the first step to .strtab
* * Update the symbols name and flag in .symtab as the kernel livepatch requirements
* * Add the names for the new relocation sections to .shstrtab
* * Add new relocation sections with a relocations entry
*/

#define SHF_RELA_LIVEPATCH	0x00100000
#define SHN_LIVEPATCH	0xff20

/* In kernel, this size is defined in linux/module.h;
 * here we use Elf_Addr instead of long for covering cross-compile
 */
#define MODULE_NAME_LEN (64 - sizeof(GElf_Addr))

#define KSYM_NAME_LEN 128

static int ShowDebugLog = 0;
#define LOG_ERR(fmt, ...) do { fprintf(stderr, "ERROR: " fmt "\n", ##__VA_ARGS__); } while(0)
#define LOG_DEBUG(fmt, ...) do { if (ShowDebugLog) printf(fmt "\n", ##__VA_ARGS__); } while(0)

#define CHECK_ALLOC(m) if (m == NULL) do { LOG_ERR("Failed to alloc memory in %s (%s:%d)", __func__, __FILE__, __LINE__); exit(1); } while(0)

typedef struct
{
	size_t symOff;
	char *sym;
	char *fName;
} Symbol;

typedef struct
{
	GElf_Shdr shdr;
	GElf_Rela *rela;
	size_t relaCnt;
	char *secName;
} RelaSym;

typedef struct
{
	size_t relaSectionCount;
	Symbol *symToRelocate;
	size_t symToRelocateCnt;
	char **funToReplace;
} Context;

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

static int appendString(GElf_Shdr *shdr, Elf_Data *data, const char *text)
{
	size_t oldSize = data->d_size;
	size_t newSize = data->d_size + strlen(text) + 1;
	char *buf = (char *)malloc(newSize);
	CHECK_ALLOC(buf);

	memcpy(buf, data->d_buf, data->d_size);
	strcpy(&buf[data->d_size], text);
	data->d_buf = buf;
	data->d_size = newSize;
	shdr->sh_size = newSize;
	return oldSize;
}

static int addSymbolToRelocate(Context *ctx, const char *sym)
{
	size_t cnt, symPos;
	char objName[MODULE_NAME_LEN];
	char *fName = (char *)malloc(KSYM_NAME_LEN);
	char *klpSym = (char *)malloc(strlen(sym) + 16);
	CHECK_ALLOC(fName);
	CHECK_ALLOC(klpSym);

	sprintf(klpSym, ".klp.sym.%s", sym);
	/* Format: sym_objname.sym_name,sympos */
	cnt = sscanf(sym, "%55[^.].%127[^,],%lu", objName, fName, &symPos);
	if (cnt != 3)
	{
		LOG_ERR("symbol '%s' has an incorrectly formatted name", sym);
		return 2;
	}
	Symbol s =
	{
		.sym = klpSym, .fName = fName
	};
	ctx->symToRelocate = realloc(ctx->symToRelocate, (ctx->symToRelocateCnt + 1) * sizeof(*ctx->symToRelocate));
	CHECK_ALLOC(ctx->symToRelocate);
	ctx->symToRelocate[ctx->symToRelocateCnt++] = s;
	return 0;
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
		if (strcmp(name, secName) == 0)
			return scn;
	}
	return NULL;
}

static char **getSymbolNames(Elf *elf)
{
	char **result;
	Elf_Scn *scn = getSectionByName(elf, ".symtab");
	if (scn == NULL)
	{
		LOG_ERR("Failed to find .symtab section");
		return NULL;
	}
	GElf_Shdr shdr;
	Elf_Data *data = elf_getdata(scn, NULL);
	gelf_getshdr(scn, &shdr);
	size_t cnt = shdr.sh_size / shdr.sh_entsize;
	result = (char **)calloc(cnt + 1, sizeof(char *));
	CHECK_ALLOC(result);
	for (size_t i = 0; i < cnt; i++)
	{
		GElf_Sym sym;
		gelf_getsym(data, i, &sym);
		result[i] = elf_strptr(elf, shdr.sh_link, sym.st_name);
	}
	return result;
}

static int addRelocateSymToStrtab(Context *ctx, Elf *elf)
{
	GElf_Shdr shdr;
	Elf_Scn *scn = getSectionByName(elf, ".strtab");
	if (scn == NULL)
	{
		LOG_ERR("Failed to find .strtab section");
		return 2;
	}
	gelf_getshdr(scn, &shdr);
	Elf_Data *data = elf_getdata(scn, NULL);
	for(size_t i = 0; i < ctx->symToRelocateCnt; i++)
	{
		ctx->symToRelocate[i].symOff = appendString(&shdr, data, ctx->symToRelocate[i].sym);
		if (ctx->symToRelocate[i].symOff == (size_t)-1)
		{
			LOG_ERR("Failed to add sybmol name '%s' to string table", ctx->symToRelocate[i].sym);
			return 2;
		}
	}
	return 0;
}

static int addSectionStr(Context *ctx, Elf *elf, RelaSym **relocs, const char *objName)
{
	GElf_Shdr shdr;
	size_t shstrndx;
	elf_getshdrstrndx(elf, &shstrndx);
	Elf_Scn *scn = getSectionByName(elf, ".shstrtab");
	if (scn == NULL)
	{
		LOG_ERR("Failed to find .shstrtab section");
		return 2;
	}
	gelf_getshdr(scn, &shdr);
	Elf_Data *data = elf_getdata(scn, NULL);
	const char *lastName = "";
	for (size_t i = 0; i < ctx->relaSectionCount; i++)
	{
		char *name = elf_strptr(elf, shstrndx, relocs[i]->shdr.sh_name);
		if (strcmp(name, lastName) == 0)
			continue;
		lastName = name;
		char *relaSecName = (char *)malloc(16 + strlen(objName) + strlen(name));
		CHECK_ALLOC(relaSecName);
		sprintf(relaSecName, ".klp.rela.%s%s", objName, name + 5);
		int off = appendString(&shdr, data, relaSecName);
		if (off == -1)
		{
			LOG_ERR("Failed to add section '%s' to string table", relaSecName);
			return 2;
		}
		relocs[i]->shdr.sh_name = off;
		relocs[i]->secName = relaSecName;
		LOG_DEBUG("Add section '%s' to string table", relaSecName);
	}
	return 0;
}

static int convSymToLpRelSym(Context *ctx, Elf *elf)
{
	GElf_Shdr shdr;
	Elf_Scn *scn = getSectionByName(elf, ".symtab");
	if (scn == NULL)
	{
		LOG_ERR("Failed to find .symtab section");
		return 2;
	}
	Elf_Data *data = elf_getdata(scn, NULL);
	gelf_getshdr(scn, &shdr);
	size_t cnt = shdr.sh_size / shdr.sh_entsize;
	for (size_t i = 0; i < cnt; i++)
	{
		GElf_Sym sym;
		gelf_getsym(data, i, &sym);
		char *name = elf_strptr(elf, shdr.sh_link, sym.st_name);
		for(size_t j = 0; j < ctx->symToRelocateCnt; j++)
		{
			if (strcmp(name, ctx->symToRelocate[j].fName) == 0)
			{
				sym.st_name = ctx->symToRelocate[j].symOff;
				sym.st_shndx = SHN_LIVEPATCH;
				LOG_DEBUG("Convert to livepatch symbol '%s'", name);
			}
		}
		gelf_update_sym(data, i, &sym);
	}
	return 0;
}

static RelaSym **removeRelaSymbols(Context *ctx, Elf *elf, char **names)
{
	RelaSym **result = NULL;
	Elf_Scn *scn = NULL;
	GElf_Shdr shdr;
	size_t shstrndx;
	GElf_Rela rela;
	Elf_Data *data;

	elf_getshdrstrndx(elf, &shstrndx);
	while ((scn = elf_nextscn(elf, scn)) != NULL)
	{
		gelf_getshdr(scn, &shdr);
		if (shdr.sh_type != SHT_RELA)
			continue;
		char *secName = elf_strptr(elf, shstrndx, shdr.sh_name);
		if (strcmp(".rela.debug_info", secName) == 0 ||
			strcmp(".rela__jump_table", secName) == 0)
			continue;
		RelaSym *relaSym = NULL;
		data = elf_getdata(scn, NULL);
		size_t j = 0;
		size_t cnt = shdr.sh_size / shdr.sh_entsize;
		for (size_t i = 0; i < cnt; i++)
		{
			gelf_getrela(data, i, &rela);
			int idx = ELF64_R_SYM(rela.r_info);
			size_t k;
			for (k = 0; k < ctx->symToRelocateCnt; k++)
			{
				if (strcmp(names[idx], ctx->symToRelocate[k].fName) == 0)
				{
					if (relaSym == NULL)
					{
						relaSym = (RelaSym *)calloc(1, sizeof(RelaSym));
						CHECK_ALLOC(relaSym);
						relaSym->rela = (GElf_Rela *)malloc(sizeof(GElf_Rela) * cnt);
						CHECK_ALLOC(relaSym->rela);
					}
					relaSym->shdr = shdr;
					relaSym->rela[relaSym->relaCnt++] = rela;

					LOG_DEBUG("Remove relocation '%s' from '%s'", ctx->symToRelocate[k].fName, secName);
					break;
				}
			}
			if (k == ctx->symToRelocateCnt)
			{
				gelf_update_rela(data, j, &rela);
				j++;
			}
		}
		if (relaSym != NULL)
		{
			result = (RelaSym **)realloc(result, sizeof(*result) * (ctx->relaSectionCount + 1));
			CHECK_ALLOC(result);
			result[ctx->relaSectionCount++] = relaSym;
			shdr.sh_size = j * shdr.sh_entsize;
			data->d_size = shdr.sh_size;
			gelf_update_shdr(scn, &shdr);
		}
	}
	return result;
}

static int addRelaSection(Context *ctx, Elf *elf, RelaSym **relocs, char **names)
{
	Elf_Scn *newscn;
	Elf_Data *newData;
	GElf_Shdr shdr;
	for (size_t i = 0; i < ctx->relaSectionCount; i++)
	{
		newscn = elf_newscn(elf);
		if (!newscn)
		{
			LOG_ERR("elf_newscn failed");
			return 2;
		}

		newData = elf_newdata(newscn);
		if (!newData)
		{
			LOG_ERR("elf_newdata failed");
			return 2;
		}

		gelf_getshdr(newscn, &shdr);
		RelaSym *relaSym = relocs[i];
		memcpy(&shdr, &relaSym->shdr, sizeof(GElf_Shdr));
		shdr.sh_flags = SHF_ALLOC | SHF_RELA_LIVEPATCH;

		newData->d_size = relaSym->relaCnt * shdr.sh_entsize;
		newData->d_type = ELF_T_RELA;
		newData->d_buf = malloc(newData->d_size);
		CHECK_ALLOC(newData->d_buf);

		for (size_t j = 0; j < relaSym->relaCnt; j++)
		{
			GElf_Rela rela = relaSym->rela[j];
			int idx = ELF64_R_SYM(rela.r_info);
			int r = gelf_update_rela(newData, j, &rela);
			if (r)
			{
				LOG_DEBUG("Add relocation '%s' to '%s'", names[idx], relaSym->secName);
			}
			else
			{
				LOG_ERR("Fail to add relocation '%s' to '%s'", names[idx], relaSym->secName);
				return 2;
			}
		}

		if (!gelf_update_shdr(newscn, &shdr))
		{
			LOG_ERR("gelf_update_shdr failed");
			return 2;
		}
	}
	return 0;
}

static Context initContext()
{
	Context ctx = {};
	ctx.symToRelocate = malloc(sizeof(*ctx.symToRelocate));
	CHECK_ALLOC(ctx.symToRelocate);
	return ctx;
}

static void freeContext(Context *ctx)
{
	free(ctx->symToRelocate);
}

int mklivepatch(const char *file, const char *objName, char *syms)
{
	elf_version(EV_CURRENT);

	int ret = 0;
	char **symbolNames = NULL;
	RelaSym **relocs  = NULL;
	Context ctx = initContext();
	int fd = open(file, O_RDWR);
	if (fd == -1)
		error(EXIT_FAILURE, errno, "cannot open input file '%s'", file);

	Elf *elf = elf_begin(fd, ELF_C_RDWR, NULL);
	if (elf == NULL)
		error(EXIT_FAILURE, 0, "problems opening '%s' as ELF file: %s",
			  file, elf_errmsg(-1));

	size_t shstrndx;
	if (elf_getshdrstrndx(elf, &shstrndx))
		error(EXIT_FAILURE, errno, "cannot get section header string index");

	while (syms[0] != '\0')
	{
		char *sep = strchr(syms, '|');
		if (sep)
			*sep = '\0';
		char *symbol = syms;
		ret = addSymbolToRelocate(&ctx, symbol);
		if (ret)
			goto cleanup;

		if (!sep)
			break;

		syms = sep + 1;
	}

	symbolNames = getSymbolNames(elf);
	if (!symbolNames)
	{
		ret = 1;
		goto cleanup;
	}

	relocs = removeRelaSymbols(&ctx, elf, symbolNames);
	ret = addRelocateSymToStrtab(&ctx, elf);
	if (ret)
		goto cleanup;
	ret = convSymToLpRelSym(&ctx, elf);
	if (ret)
		goto cleanup;
	ret = addSectionStr(&ctx, elf, relocs, objName);
	if (ret)
		goto cleanup;
	ret = addRelaSection(&ctx, elf, relocs, symbolNames);
	if (ret)
		goto cleanup;

	if (elf_update(elf, ELF_C_WRITE) == -1)
		error(EXIT_FAILURE, 0, "elf_update failed: %s", elf_errmsg(-1));

cleanup:
	for (size_t i = 0; i < ctx.relaSectionCount; i++)
	{
		free(relocs[i]->secName);
	}
	free(relocs);
	free(symbolNames);
	close(fd);
	freeContext(&ctx);

	return ret;
}

#ifndef USE_AS_LIB
static void help(const char *execName)
{
	LOG_ERR("Usage: %s -s <OBJ.PATCH_FUNCTION> -r <OBJ.RELOCATION_FUNCTION,IDX> [-V] <MODULE.ko>", execName);
}

int main(int argc, char *argv[])
{
	char *file = NULL;
	char *objName = NULL;
	int opt;
	char *syms = calloc(1, sizeof(char));

	while ((opt = getopt(argc, argv, "s:r:Vh")) != -1)
	{
		switch (opt)
		{
		case 's':
		{
			if (objName)
				break;
			char *fun = strchr(optarg, '.') + 1;
			int offset = fun - optarg - 1;
			objName = strdup(optarg);
			objName[offset] = '\0';
			break;
		}
		case 'r':
			syms = appendFormatString(syms, "%s|", optarg);
			break;
		case 'V':
			ShowDebugLog = 0;
			break;
		case '?':
		case 'h':
			help(argv[0]);
			break;
		case ':':
			LOG_ERR("Missing arg for %c", optopt);
			break;
		}
	}

	if (optind < argc)
	{
		file = argv[optind++];
		while (optind < argc)
			LOG_ERR("Unknown parameter: %s", argv[optind++]);
	}

	if (file == NULL || objName == NULL || syms[0] == '\0')
	{
		free(objName);
		free(syms);
		help(argv[0]);
		return 1;
	}

	syms[strlen(syms)-1] = '\0';
	int ret = mklivepatch(file, objName, syms);

	free(objName);
	free(syms);
	return ret;
}
#endif /* USE_AS_LIB */
