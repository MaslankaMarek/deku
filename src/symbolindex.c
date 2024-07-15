/*
* Author: Marek Maślanka
* Project: DEKU
* URL: https://github.com/MarekMaslanka/deku
*
* Find symbol index in the object file for specific source file
*/

#define _GNU_SOURCE	/* needed for memmem */

#include <errno.h>
#include <error.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <getopt.h>

#include <gelf.h>

/** GENERIC **/
#define MODULE_NAME_LEN (64 - sizeof(GElf_Addr))

#define KSYM_NAME_LEN 128

static int ShowDebugLog = 1;
#define LOG_ERR(fmt, ...) do { fprintf(stderr, "ERROR: " fmt "\n", ##__VA_ARGS__); } while(0)
#define LOG_INFO(fmt, ...) do { printf("INFO: " fmt "\n", ##__VA_ARGS__); } while(0)
#define LOG_DEBUG(fmt, ...) do { if (ShowDebugLog) printf(fmt "\n", ##__VA_ARGS__); } while(0)

#define CHECK_ALLOC(m) if (m == NULL) { LOG_ERR("Failed to alloc memory in %s (%s:%d)", __func__, __FILE__, __LINE__); exit(EXIT_FAILURE);}

typedef struct _Symbol
{
	char *name;
	size_t index;
	size_t secIndex;
	bool isFun;
	bool isVar;
	size_t linkedSymbol;
	size_t copiedIndex;
	unsigned char st_info;
	size_t *callees;
	unsigned offset;
	struct _Symbol *next;
} Symbol;

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

static unsigned getSymbolIndexForSection(Elf *elf, int secIndex)
{
	GElf_Sym sym;
	Elf_Scn *scn = getSectionByName(elf, ".symtab");
	GElf_Shdr shdr;
	Elf_Data *data = elf_getdata(scn, NULL);
	gelf_getshdr(scn, &shdr);
	size_t cnt = shdr.sh_size / shdr.sh_entsize;
	for (size_t i = 0; i < cnt; i++)
	{
		gelf_getsym(data, i, &sym);
		if (sym.st_shndx == secIndex && sym.st_info == ELF64_ST_INFO(STB_LOCAL, STT_SECTION))
			return i;
	}

	LOG_ERR("Can't find symbol for section at index: %d\n", secIndex);
	return 0;
}

static Elf *openElf(const char *filePath, int *fd)
{
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

/** END GENERIC **/

int findObjIndex(const char *path, const char *srcFile)
{
	char *objFileName = NULL;
	char *objFilePath = NULL;
	char *ptr = NULL;
	int index = 0;
	size_t srcFileLen = strlen(srcFile);

	FILE *fp = fopen(path, "rb");
	if (fp == NULL)
	{
		LOG_ERR("Can't open file: %s", path);
		return -1;
	}

	fseek(fp, 0, SEEK_END);
	long fileSize = ftell(fp);
	fseek(fp, 0, SEEK_SET);

	char *buffer = malloc(fileSize + 1);
	CHECK_ALLOC(buffer);

	fread(buffer, 1, fileSize, fp);
	buffer[fileSize] = '\0';

	objFilePath = malloc(srcFileLen + 2);
	CHECK_ALLOC(objFilePath);
	strcpy(objFilePath, srcFile);
	objFileName = strrchr(objFilePath, '/') + 1;
	objFilePath[srcFileLen - 1] = 'o';
	objFilePath[srcFileLen] = '/';
	objFilePath[srcFileLen + 1] = '\0';

	char *filePtr = memmem(buffer, fileSize, objFilePath, strlen(objFilePath));
	if (filePtr == NULL)
	{
		LOG_ERR("Can't find object by path");
		index = -1;
		goto out;
	}

	ptr = buffer;
	while (ptr < buffer + fileSize)
	{
		ptr = memmem(ptr, buffer + fileSize - ptr, objFileName, strlen(objFileName));
		if (ptr == NULL)
		{
			LOG_ERR("Can't find object index");
			index = -1;
			break;
		}
		else if (ptr + strlen(objFilePath) > filePtr)
		{
			break;
		}
		index++;
		ptr += strlen(objFileName);
	}

out:
	free(objFilePath);
	free(buffer);
	fclose(fp);
	return index;
}

static void help(const char *execName)
{
	printf("Usage: %s -o <OBJECT_FILE> -a <ARCHIVE> -f <SRC_FILE_PATH> -t <SYMBOL_TYPE> [-V] <SYMBOL_NAME>", execName);
}

int main(int argc, char *argv[])
{
	char *objPath = NULL;
	char *archivePath = NULL;
	char *srcPath = NULL;
	char *srcFile = NULL;
	char *symToFind = NULL;
	int objIndex = 0;
	int objFd = -1;
	int opt;
	char type = 0;
	bool foundFile = false;
	Elf64_Addr symbolAddress = 0;
	uint32_t addrCount = 0;
	Elf64_Addr addresses[256];

	while ((opt = getopt(argc, argv, "o:a:f:t:Vh")) != -1)
	{
		switch (opt)
		{
		case 'o':
			objPath = strdup(optarg);
			break;
		case 'a':
			archivePath = strdup(optarg);
			break;
		case 'f':
			srcPath = strdup(optarg);
			break;
		case 't':
			if (strcmp(optarg, "v") == 0)
				type = STT_OBJECT;
			else if (strcmp(optarg, "f") == 0)
				type = STT_FUNC;
			else
			{
				LOG_ERR("Invalid -t parameter. Avaiable option: [f|v]");
				goto failure;
			}
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
	if (optind == argc - 1)
		symToFind = strdup(argv[optind]);

	if (objPath == NULL || srcPath == NULL || symToFind == NULL)
	{
		if (objPath == NULL)
			LOG_ERR("Missing object file path [-o]");
		if (srcPath == NULL)
			LOG_ERR("Missing source file path [-f]");
		if (symToFind == NULL)
			LOG_ERR("Missing symbol name to find");
		help(argv[0]);
		goto failure;
	}

	if (archivePath != NULL) {
		objIndex = findObjIndex(archivePath, srcPath);
		if (objIndex == -1)
			return 2;
	}

	srcFile = strrchr(srcPath, '/') + 1;

	elf_version(EV_CURRENT);

	Elf *elf = openElf(objPath, &objFd);
	if (elf == NULL)
		goto failure;

	GElf_Sym sym;
	Elf_Scn *scn = getSectionByName(elf, ".symtab");
	GElf_Shdr shdr;
	Elf_Data *data = elf_getdata(scn, NULL);
	gelf_getshdr(scn, &shdr);
	size_t cnt = shdr.sh_size / shdr.sh_entsize;
	for (size_t i = 0; i < cnt; i++)
	{
		gelf_getsym(data, i, &sym);
		const char *symName = elf_strptr(elf, shdr.sh_link, sym.st_name);
		if (ELF64_ST_TYPE(sym.st_info) == STT_FILE)
		{
			foundFile = strcmp(symName, srcFile) == 0;
		}
		else if (ELF64_ST_TYPE(sym.st_info) == type &&
				 strcmp(symName, symToFind) == 0)
		{
			if (foundFile)
			{
				if (objIndex == 0)
					symbolAddress = sym.st_value;
				foundFile = false;
				objIndex--;
			}
			addresses[addrCount++] = sym.st_value;
		}
	}

	if (addrCount > 0)
	{
		uint32_t index = 0;
		for (uint32_t i = 0; i < addrCount; i++)
		{
			if (addresses[i] < symbolAddress)
				index++;
		}
		printf("%d\n", index + 1);
	}

	close(objFd);

	free(objPath);
	free(srcPath);

	return addrCount > 0 ? 0 : 3;

failure:
	LOG_ERR("failure");
	free(objPath);
	free(srcPath);
	if (objFd == -1)
		close(objFd);
	help(argv[0]);
	return EXIT_FAILURE;
}
