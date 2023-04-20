/* SPDX-License-Identifier: GPL-2.0 OR Apache-2.0 */
/*
 * Copyright (C) Semihalf, 2022
 * Author: Marek Maślanka <mm@semihalf.com>
 *
 * Convert kernel module to livepatch module
 */

#include <errno.h>
#include <error.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
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

/* In kernel, this size is defined in linux/module.h;
 * here we use Elf_Addr instead of long for covering cross-compile
 */
#define MODULE_NAME_LEN (64 - sizeof(GElf_Addr))

#define KSYM_NAME_LEN 128

static int ShowDebugLog = 1;
#define LOG_ERR(fmt, ...) do { fprintf(stderr, "ERROR: " fmt "\n", ##__VA_ARGS__); exit(1); } while(0)
#define LOG_DEBUG(fmt, ...) do { if (ShowDebugLog) printf(fmt "\n", ##__VA_ARGS__); } while(0)

#define CHECK_ALLOC(m) if (m == NULL) LOG_ERR("Failed to alloc memory in %s (%s:%d)", __func__, __FILE__, __LINE__)

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

size_t relaSectionCount = 0;

Symbol *symToRelocate = NULL;
size_t symToRelocateCnt = 0;
char **funToReplace = NULL;

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

static void addSymbolToRelocate(const char *sym)
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
		LOG_ERR("symbol '%s' has an incorrectly formatted name", sym);
	Symbol s =
	{
		.sym = klpSym, .fName = fName
	};
	symToRelocate = realloc(symToRelocate, (symToRelocateCnt + 1) * sizeof(*symToRelocate));
	CHECK_ALLOC(symToRelocate);
	symToRelocate[symToRelocateCnt++] = s;
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
		LOG_ERR("Failed to find .symtab section");
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

static void addRelocateSymToStrtab(Elf *elf)
{
	GElf_Shdr shdr;
	Elf_Scn *scn = getSectionByName(elf, ".strtab");
	if (scn == NULL)
		LOG_ERR("Failed to find .strtab section");
	gelf_getshdr(scn, &shdr);
	Elf_Data *data = elf_getdata(scn, NULL);
	for(size_t i = 0; i < symToRelocateCnt; i++)
	{
		symToRelocate[i].symOff = appendString(&shdr, data, symToRelocate[i].sym);
		if (symToRelocate[i].symOff == (size_t)-1)
			LOG_ERR("Failed to add sybmol name '%s' to string table", symToRelocate[i].sym);
	}
}

static void addSectionStr(Elf *elf, RelaSym **relocs, const char *objName)
{
	GElf_Shdr shdr;
	size_t shstrndx;
	elf_getshdrstrndx(elf, &shstrndx);
	Elf_Scn *scn = getSectionByName(elf, ".shstrtab");
	if (scn == NULL)
		LOG_ERR("Failed to find .shstrtab section");
	gelf_getshdr(scn, &shdr);
	Elf_Data *data = elf_getdata(scn, NULL);
	const char *lastName = "";
	for (size_t i = 0; i < relaSectionCount; i++)
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
			LOG_ERR("Failed to add section '%s' to string table", relaSecName);
		relocs[i]->shdr.sh_name = off;
		relocs[i]->secName = relaSecName;
		LOG_DEBUG("Add section '%s' to string table", relaSecName);
	}
}

static int convSymToLpRelSym(Elf *elf)
{
	GElf_Shdr shdr;
	Elf_Scn *scn = getSectionByName(elf, ".symtab");
	if (scn == NULL)
		LOG_ERR("Failed to find .symtab section");
	Elf_Data *data = elf_getdata(scn, NULL);
	gelf_getshdr(scn, &shdr);
	size_t cnt = shdr.sh_size / shdr.sh_entsize;
	for (size_t i = 0; i < cnt; i++)
	{
		GElf_Sym sym;
		gelf_getsym(data, i, &sym);
		char *name = elf_strptr(elf, shdr.sh_link, sym.st_name);
		for(size_t j = 0; j < symToRelocateCnt; j++)
		{
			if (strcmp(name, symToRelocate[j].fName) == 0)
			{
				sym.st_name = symToRelocate[j].symOff;
				sym.st_shndx = 0xFF20;
				LOG_DEBUG("Convert to livepatch symbol '%s'", name);
			}
		}
		gelf_update_sym(data, i, &sym);
	}
	return 0;
}

static RelaSym **removeRelaSymbols(Elf *elf, char **names)
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
			for (k = 0; k < symToRelocateCnt; k++)
			{
				if (strcmp(names[idx], symToRelocate[k].fName) == 0)
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

					LOG_DEBUG("Remove relocation '%s' from '%s'", symToRelocate[k].fName, secName);
					break;
				}
			}
			if (k == symToRelocateCnt)
			{
				gelf_update_rela(data, j, &rela);
				j++;
			}
		}
		if (relaSym != NULL)
		{
			result = (RelaSym **)realloc(result, sizeof(*result) * (relaSectionCount + 1));
			CHECK_ALLOC(result);
			result[relaSectionCount++] = relaSym;
			shdr.sh_size = j * shdr.sh_entsize;
			data->d_size = shdr.sh_size;
			gelf_update_shdr(scn, &shdr);
		}
	}
	return result;
}

static void addRelaSection(Elf *elf, RelaSym **relocs, char **names)
{
	Elf_Scn *newscn;
	Elf_Data *newData;
	GElf_Shdr shdr;
	for (size_t i = 0; i < relaSectionCount; i++)
	{
		newscn = elf_newscn(elf);
		if (!newscn)
			LOG_ERR("elf_newscn failed");

		newData = elf_newdata(newscn);
		if (!newData)
			LOG_ERR("elf_newdata failed");

		gelf_getshdr(newscn, &shdr);
		RelaSym *relaSym = relocs[i];
		memcpy(&shdr, &relaSym->shdr, sizeof(GElf_Shdr));
		shdr.sh_flags = SHF_ALLOC | 0x0000000000100000;

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
				LOG_DEBUG("Add relocation '%s' to '%s'", names[idx], relaSym->secName);
			else
				LOG_ERR("Fail to add relocation '%s' to '%s'", names[idx], relaSym->secName);
		}

		if (!gelf_update_shdr(newscn, &shdr))
			LOG_ERR("gelf_update_shdr failed");
	}
}

static void help(const char *execName)
{
	error(EXIT_FAILURE, 0, "Usage: %s -s <OBJ.PATCH_FUNCTION> -r <OBJ.RELOCATION_FUNCTION,IDX> [-V] <MODULE.ko>", execName);
}

int main(int argc, char *argv[])
{
	char *file = NULL;
	char *objName = NULL;
	int opt, funCnt = 0;

	funToReplace = calloc(argc, sizeof(char *));
	CHECK_ALLOC(funToReplace);
	symToRelocate = malloc(sizeof(*symToRelocate));
	CHECK_ALLOC(symToRelocate);
	while ((opt = getopt(argc, argv, "s:r:Vh")) != -1)
	{
		switch (opt)
		{
		case 's':
		{
			char *fun = strchr(optarg, '.') + 1;
			int offset = fun - optarg - 1;
			objName = strdup(optarg);
			objName[offset] = '\0';
			funToReplace[funCnt++] = fun;
			break;
		}
		case 'r':
			addSymbolToRelocate(optarg);
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

	if (file == NULL || objName == NULL)
		help(argv[0]);

	elf_version(EV_CURRENT);

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

	char **symbolNames = getSymbolNames(elf);
	RelaSym **relocs = removeRelaSymbols(elf, symbolNames);
	addRelocateSymToStrtab(elf);
	convSymToLpRelSym(elf);
	addSectionStr(elf, relocs, objName);
	addRelaSection(elf, relocs, symbolNames);

	if (elf_update(elf, ELF_C_WRITE) == -1)
		error(EXIT_FAILURE, 0, "elf_update failed: %s", elf_errmsg(-1));

	close(fd);
	for (size_t i = 0; i < relaSectionCount; i++)
	{
		free(relocs[i]->secName);
	}
	free(relocs);
	free(objName);
	free(symbolNames);
	free(symToRelocate);
	free(funToReplace);

	return 0;
}
