/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) Semihalf, 2022
 * Author: Marek Maślanka <mm@semihalf.com>
 */

#include <errno.h>
#include <error.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>

#include <gelf.h>

#ifdef SUPPORT_DISASS
#define PACKAGE 1			//requred by libbfd
#include <dis-asm.h>

#define MAX_DISASS_LINE_LEN 256
#endif

static bool ShowDebugLog = 1;
#define LOG_ERR(fmt, ...)                                   \
	do                                                      \
	{                                                       \
		fprintf(stderr, "ERROR: " fmt "\n", ##__VA_ARGS__); \
		exit(1);                                            \
	} while (0)
#define LOG_INFO(fmt, ...)                   \
	do                                       \
	{                                        \
		printf(fmt "\n", ##__VA_ARGS__);     \
	} while (0)
#define LOG_DEBUG(fmt, ...)                  \
	do                                       \
	{                                        \
		if (ShowDebugLog)                    \
			printf(fmt "\n", ##__VA_ARGS__); \
	} while (0)

#define CHECK_ALLOC(m) \
	if (m == NULL)     \
	LOG_ERR("Failed to alloc memory in %s (%s:%d)", __func__, __FILE__, __LINE__)

#define invalidSym(sym) (sym.st_name == 0 && sym.st_info == 0 && sym.st_shndx == 0)

typedef struct
{
	uint8_t *data;
	size_t size;
} SymbolData;

typedef struct
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
} Symbol;

static Symbol **Symbols = NULL;
static Elf_Scn **CopiedScnMap = NULL;
static size_t SectionsCount = 0;
static size_t SymbolsCount = 0;

typedef struct
{
	Elf *elf;
	GElf_Sym sym;
	GElf_Shdr shdr;
	SymbolData *symData;
} DisasmData;

static uint32_t crc32(uint8_t *data, uint32_t len)
{
	uint32_t byte, crc, mask;
	crc = 0xFFFFFFFF;
	for (uint32_t i = 0; i < len; i++)
	{
		byte = data[i];
		crc = crc ^ byte;
		for (int j = 7; j >= 0; j--)
		{
			mask = -(crc & 1);
			crc = (crc >> 1) ^ (0xEDB88320 & mask);
		}
		i = i + 1;
	}
	return ~crc;
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
	GElf_Shdr shdr;
	size_t shstrndx;
	elf_getshdrstrndx(elf, &shstrndx);
	Elf_Scn *scn = elf_getscn(elf, index);
	if (!scn)
		return NULL;
	gelf_getshdr(scn, &shdr);
	return elf_strptr(elf, shstrndx, shdr.sh_name);
}

static Symbol **readSymbols(Elf *elf)
{
	Symbol **syms;
	Elf_Scn *scn = getSectionByName(elf, ".symtab");
	if (scn == NULL)
		LOG_ERR("Failed to find .symtab section");
	GElf_Shdr shdr;
	GElf_Sym sym;
	Elf_Data *data = elf_getdata(scn, NULL);
	gelf_getshdr(scn, &shdr);
	size_t cnt = shdr.sh_size / shdr.sh_entsize;
	syms = (Symbol **)calloc(cnt + 1, sizeof(Symbol *));
	CHECK_ALLOC(syms);
	for (size_t i = 0; i < cnt; i++)
	{
		gelf_getsym(data, i, &sym);
		syms[i] = calloc(1, sizeof(Symbol));
		CHECK_ALLOC(syms[i]);
		// name
		syms[i]->name = elf_strptr(elf, shdr.sh_link, sym.st_name);
		// section index
		syms[i]->secIndex = sym.st_shndx;
		// is function & hash of function
		if ((sym.st_info == ELF64_ST_INFO(STB_GLOBAL, STT_FUNC) ||
			 (sym.st_info == ELF64_ST_INFO(STB_LOCAL, STT_FUNC))) &&
			strlen(syms[i]->name) > 0)
		{
			syms[i]->isFun = true;
		}
		// is variable
		if ((sym.st_info == ELF64_ST_INFO(STB_GLOBAL, STT_OBJECT) ||
			 (sym.st_info == ELF64_ST_INFO(STB_LOCAL, STT_OBJECT))) &&
			strlen(syms[i]->name) > 0 && sym.st_value == 0)
		{
			const char *scnName = getSectionName(elf, sym.st_shndx);
			if (strstr(scnName, ".data.") == scnName ||
				strstr(scnName, ".bss.") == scnName)
				syms[i]->isVar = true;
		}
		syms[i]->st_info = sym.st_info;
		syms[i]->index = SymbolsCount++;
	}
	return syms;
}

static GElf_Sym getSymbolByName(Elf *elf, char *name, size_t *symIndex)
{
	Elf_Scn *scn = getSectionByName(elf, ".symtab");
	GElf_Shdr shdr;
	GElf_Sym sym = {0};
	Elf_Data *data = elf_getdata(scn, NULL);
	gelf_getshdr(scn, &shdr);
	size_t cnt = shdr.sh_size / shdr.sh_entsize;
	*symIndex = 0;
	for (size_t i = 0; i < cnt; i++)
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
	if (scn == NULL)
		LOG_ERR("Failed to find .symtab section");
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
	if (scn == NULL)
		LOG_ERR("Failed to find .symtab section");
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

static size_t getLinkedFuncSymbol(Symbol *symbol)
{
	for (Symbol **s = Symbols; *s != NULL; s++)
	{
		if (s[0] != symbol && s[0]->secIndex == symbol->secIndex && s[0]->isFun)
			return s - Symbols;
	}
	return -1;
}

static size_t getLinkedSymbol(Symbol *symbol)
{
	for (Symbol **s = Symbols; *s != NULL; s++)
	{
		if (s[0] != symbol && s[0]->secIndex == symbol->secIndex && (s[0]->isFun || s[0]->isVar))
			return s - Symbols;
	}
	return -1;
}

static GElf_Sym getLinkedSym(Elf *elf, GElf_Sym *sym)
{
	Elf_Scn *scn = getSectionByName(elf, ".symtab");
	GElf_Shdr shdr;
	GElf_Sym tsym = {0};
	Elf_Data *data = elf_getdata(scn, NULL);
	gelf_getshdr(scn, &shdr);
	size_t cnt = shdr.sh_size / shdr.sh_entsize;
	for (size_t i = 0; i < cnt; i++)
	{
		gelf_getsym(data, i, &tsym);
		if (memcmp(&tsym, sym, sizeof(*sym)) != 0 && tsym.st_name != 0 &&
				   tsym.st_shndx == sym->st_shndx)
			return tsym;
	}
	memset(&tsym, 0, sizeof(tsym));
	return tsym;
}

static SymbolData getSymbolData(Elf *elf, const char *name, char type, bool modReloc)
{
	SymbolData result;
	Elf_Scn *scn = getSectionByName(elf, ".symtab");
	if (scn == NULL)
		LOG_ERR("Failed to find .symtab section");
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
				Elf_Data *data = elf_getdata(scn, NULL);
				result.data = &((uint8_t *)data->d_buf)[sym.st_value];
				result.size = sym.st_size;
				if (modReloc)
				{
					GElf_Rela rela;
					GElf_Shdr shdr;
					Elf_Scn *scn = getRelForSectionIndex(elf, sym.st_shndx);
					if (scn == NULL)
						continue;
					Elf_Data *rdata = elf_getdata(scn, NULL);
					gelf_getshdr(scn, &shdr);
					size_t cnt = shdr.sh_size / shdr.sh_entsize;
					for (size_t i = 0; i < cnt; i++)
					{
						gelf_getrela(rdata, i, &rela);
						if (rela.r_offset >= sym.st_value && rela.r_offset < sym.st_value + sym.st_size)
						{
							void *addr = &((uint8_t *)data->d_buf)[rela.r_offset];
							if (ELF64_R_TYPE(rela.r_info) == R_X86_64_PC32)
								*(uint32_t *)addr += -4;
							else
								*(uint32_t *)addr += rela.r_addend;
						}
					}
				}
				break;
			}
		}
	}
	return result;
}

#ifdef SUPPORT_DISASSEMBLE

static GElf_Sym getSymbolForReloc(Elf *elf, Elf64_Section sec, size_t offset)
{
	GElf_Rela rela;
	GElf_Shdr shdr;
	GElf_Sym invalidSym = {};
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
			return getSymbolByIndex(elf, ELF64_R_SYM(rela.r_info));
	}
	return invalidSym;
}

static GElf_Sym getSymbolByOffset(Elf *elf, Elf64_Section shndx, size_t offset)
{
	Elf_Scn *scn = getSectionByName(elf, ".symtab");
	GElf_Shdr shdr;
	GElf_Sym sym = {};
	Elf_Data *data = elf_getdata(scn, NULL);
	gelf_getshdr(scn, &shdr);
	size_t cnt = shdr.sh_size / shdr.sh_entsize;
	for (size_t i = 0; i < cnt; i++)
	{
		gelf_getsym(data, i, &sym);
		if (sym.st_name != 0 && sym.st_shndx == shndx && sym.st_value == offset)
			return sym;
	}
	memset(&sym, 0, sizeof(sym));
	return sym;
}

static int disasmPrintf(void *buf, const char *format, ...)
{
	char localBuf[MAX_DISASS_LINE_LEN];
	va_list args;
	va_start (args, format);
	int len = vsnprintf(localBuf, sizeof(localBuf), format, args);
	va_end (args);

	char **buffer = (char **)buf;
	buffer[0] = realloc(buffer[0], strlen(buffer[0]) + len  + 1);
	strcat(buffer[0], localBuf);
	return len;
}

static void printFunAtAddr(bfd_vma vma, struct disassemble_info *inf)
{
	DisasmData *data = (DisasmData *)inf->application_data;
	int32_t x = *(uint32_t *)(data->symData->data + vma);
	vma += data->sym.st_value;
	GElf_Sym sym = getSymbolByOffset(data->elf, data->sym.st_shndx, vma);
	if(invalidSym(sym))
		sym = getSymbolForReloc(data->elf, data->sym.st_shndx, vma);

	if(invalidSym(sym))
	{
		sym = getSymbolForReloc(data->elf, data->sym.st_shndx, vma);
		const char *name = elf_strptr(data->elf, data->shdr.sh_link, data->sym.st_name);
		(*inf->fprintf_func)(inf->stream, "<%s+0x%lX>", name, vma - data->sym.st_value);
	}
	else
	{
		const char *name = elf_strptr(data->elf, data->shdr.sh_link, sym.st_name);
		(*inf->fprintf_func)(inf->stream, "%s", name);
	}
}

char *disassembleBytes(uint8_t *inputBuf, size_t inputBufSize, DisasmData *data)
{
	char *buf[] = { calloc(0, 1) };

	disassemble_info disasmInfo = {};
	init_disassemble_info(&disasmInfo, buf, disasmPrintf);
	disasmInfo.arch = bfd_arch_i386;
	disasmInfo.mach = bfd_mach_x86_64;
	disasmInfo.read_memory_func = buffer_read_memory;
	disasmInfo.buffer = inputBuf;
	disasmInfo.buffer_vma = 0;
	disasmInfo.buffer_length = inputBufSize;
	disasmInfo.application_data = (void *)data;
	disassemble_init_for_target(&disasmInfo);

	disasmInfo.print_address_func = printFunAtAddr;

	disassembler_ftype disasm = disassembler(bfd_arch_i386, false, bfd_mach_x86_64, NULL);

	size_t pc = 0;
	while (pc < inputBufSize)
	{
		pc += disasm(pc, &disasmInfo);
		disasmInfo.fprintf_func(disasmInfo.stream, "\n");
	}

	return buf[0];
}
#endif

static uint32_t calcSymHash(Elf *elf, const GElf_Sym *sym)
{
	Elf_Scn *scn = elf_getscn(elf, sym->st_shndx);
	Elf_Data *data = elf_rawdata(scn, NULL);
	uint32_t crc = crc32((uint8_t *)data->d_buf + sym->st_value, sym->st_size);

	GElf_Rela rela;
	GElf_Shdr shdr;
	scn = getRelForSectionIndex(elf, sym->st_shndx);
	if (scn == NULL)
		return crc;
	Elf_Data *rdata = elf_getdata(scn, NULL);
	gelf_getshdr(scn, &shdr);
	size_t cnt = shdr.sh_size / shdr.sh_entsize;

	scn = getSectionByName(elf, ".symtab");
	gelf_getshdr(scn, &shdr);
	Elf64_Word symtabLink = shdr.sh_link;

	for (size_t i = 0; i < cnt; i++)
	{
		gelf_getrela(rdata, i, &rela);
		if (rela.r_offset >= sym->st_value && rela.r_offset < sym->st_value + sym->st_size)
		{
			GElf_Sym rsym = getSymbolByIndex(elf, ELF64_R_SYM(rela.r_info));
			if(invalidSym(rsym))
				LOG_ERR("Can't find symbol at index: %ld", ELF64_R_SYM(rela.r_info));
			int secIndex = rsym.st_shndx;
			const char *name = NULL;
			if(rsym.st_name == 0)
			{
				if (rsym.st_info == STT_SECTION)
				{
					name = getSectionName(elf, secIndex);
				}
				else
				{
					rsym = getLinkedSym(elf, &rsym);
					if(invalidSym(rsym))
						LOG_ERR("Can't find symbol at index: %ld", ELF64_R_SYM(rela.r_info));
					name = elf_strptr(elf, symtabLink, rsym.st_name);
				}
			}
			else
			{
				name = elf_strptr(elf, symtabLink, rsym.st_name);
			}
			if (name)
			{
				if (strstr(name, ".str.") || strstr(name, ".str1.") || strstr(name, ".rodata.str") == name)
				{
					Elf_Scn *scn = elf_getscn(elf, secIndex);
					Elf_Data *data = elf_getdata(scn, NULL);
					GElf_Shdr shdr;
					gelf_getshdr(scn, &shdr);
					if ((Elf64_Sxword)shdr.sh_size > rela.r_addend)
						name = (char *)data->d_buf + rela.r_addend;
				}
				if (strstr(name, ".text.unlikely.") == name)
					name += strlen(".text.unlikely.");
				else if (strstr(name, ".text.") == name)
					name += strlen(".text.");
				crc += crc32((uint8_t *)name, strlen(name));
			}
		}
	}
	return crc;
}

static bool equalFunctions(Elf *elf, Elf *secondElf, const char *funName)
{
	SymbolData symData1 = getSymbolData(elf, funName, STT_FUNC, false);
	SymbolData symData2 = getSymbolData(secondElf, funName, STT_FUNC, false);

	if (symData1.size != symData2.size)
		return false;

	GElf_Sym sym1;
	GElf_Sym sym2;
	getSymbolByNameAndType(elf, funName, STT_FUNC, &sym1);
	getSymbolByNameAndType(secondElf, funName, STT_FUNC, &sym2);
	return calcSymHash(elf, &sym1) == calcSymHash(secondElf, &sym2);
}

static void findModifiedSymbols(Elf *elf, Elf *secondElf)
{
	Elf_Scn *scn = getSectionByName(elf, ".symtab");
	if (scn == NULL)
		LOG_ERR("Failed to find .symtab section");
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
		if (sym.st_size == 0 || sym.st_shndx == 0 || sym.st_shndx >= secCount || sym.st_name == 0)
			continue;
		const char *name = elf_strptr(elf, shdr.sh_link, sym.st_name);
		if (ELF64_ST_TYPE(sym.st_info) == STT_FUNC)
		{
			GElf_Sym secondSym;
			if (!getSymbolByNameAndType(secondElf, name, STT_FUNC, &secondSym))
			{
				printf("New function: %s\n", name);
			}
			else
			{
				if (!equalFunctions(elf, secondElf, name))
					printf("Modified function: %s\n", name);
			}
		}
		else if (ELF64_ST_TYPE(sym.st_info) == STT_OBJECT)
		{
			GElf_Sym secondSym;
			if (!getSymbolByNameAndType(secondElf, name, STT_OBJECT, &secondSym))
			{
				char *bssName = malloc(strlen(name) + 6);
				CHECK_ALLOC(bssName);
				char *dataName = malloc(strlen(name) + 7);
				CHECK_ALLOC(dataName);

				const char *scnName = getSectionName(elf, sym.st_shndx);
				if (strcmp(scnName, dataName) == 0  ||
					strcmp(scnName, bssName) == 0  ||
					strcmp(scnName, ".data") == 0 ||
					strcmp(scnName, ".bss") == 0)
					printf("New variable: %s\n", name);

				free(dataName);
				free(bssName);
			}
		}
	}
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

static Elf_Scn *copySection(Elf *elf, Elf *outElf, Elf64_Section index, bool copyData)
{
	if (CopiedScnMap[index] != NULL)
		return CopiedScnMap[index];

	if (index >= SectionsCount)
		LOG_ERR("Try to copy section that is out range (%d/%ld)", index, SectionsCount);

	size_t shstrndx;
	GElf_Shdr newShdr;
	GElf_Shdr oldShdr;
	GElf_Shdr strshdr;
	elf_getshdrstrndx(elf, &shstrndx);
	Elf_Scn *strtabScn = getSectionByName(outElf, ".shstrtab");
	Elf_Data *strData = elf_getdata(strtabScn, NULL);
	gelf_getshdr(strtabScn, &strshdr);
	Elf_Scn *oldScn = elf_getscn(elf, index);
	Elf_Data *oldData = elf_getdata(oldScn, NULL);
	Elf_Scn *newScn = elf_newscn(outElf);
	Elf_Data *newData = elf_newdata(newScn);
	gelf_getshdr(oldScn, &oldShdr);
	gelf_getshdr(newScn, &newShdr);
	newShdr.sh_type = oldShdr.sh_type;
	newShdr.sh_flags = oldShdr.sh_flags;
	newShdr.sh_entsize = oldShdr.sh_entsize;
	newShdr.sh_name = appendString(&strshdr, strData, elf_strptr(elf, shstrndx, oldShdr.sh_name));
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

	CopiedScnMap[index] = newScn;
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
	if (scn == NULL)
		LOG_ERR("Failed to find .symtab section");
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

static size_t copySymbol(Elf *elf, Elf *outElf, size_t index, bool fullCopy)
{
	GElf_Sym oldSym;
	GElf_Shdr shdr;

	size_t linkIndex = getLinkedSymbol(Symbols[index]);
	if (Symbols[index]->name == NULL && linkIndex != (size_t)-1)
		index = linkIndex;

	if (Symbols[index]->copiedIndex)
		return Symbols[index]->copiedIndex;

	Elf_Scn *scn = getSectionByName(elf, ".symtab");
	Elf_Data *data = elf_getdata(scn, NULL);
	gelf_getshdr(scn, &shdr);
	gelf_getsym(data, index, &oldSym);
	scn = getSectionByName(outElf, ".symtab");
	gelf_getshdr(scn, &shdr);
	size_t newIndex = shdr.sh_size/shdr.sh_entsize;
	data = elf_getdata(scn, NULL);
	data->d_buf = realloc(data->d_buf, data->d_size + sizeof(GElf_Sym));
	CHECK_ALLOC(data->d_buf);
	GElf_Sym newSym = oldSym;

	char symType = ELF64_ST_TYPE(oldSym.st_info);
	if (oldSym.st_shndx > 0 && oldSym.st_shndx < SectionsCount &&
		(fullCopy || (symType != STT_FUNC && symType != STT_OBJECT)))
	{
		Elf_Scn *scn = copySection(elf, outElf, oldSym.st_shndx, true);
		newSym.st_shndx = elf_ndxscn(scn);

		if (oldSym.st_name != 0)
		{
			newSym.st_info = ELF64_ST_INFO(STB_GLOBAL, symType);
			// TODO: Avoid modify symbol name for functions
			if (symType == STT_FUNC)
			{
				char *symName = strdup(Symbols[index]->name);
				CHECK_ALLOC(symName);
				char *n;
				while ((n = strchr(symName, '.')) != NULL)
					*n = '_';
				newSym.st_name = appendStringToScn(outElf, ".strtab", symName);

				free(symName);
			}
			else
			{
				newSym.st_name = appendStringToScn(outElf, ".strtab", Symbols[index]->name);
			}
		}
	}
	else // mark symbol as "external"
	{
		if (oldSym.st_shndx > 0 && oldSym.st_shndx < SectionsCount)
			newSym.st_shndx = 0;
		newSym.st_size = 0;
		newSym.st_info = ELF64_ST_INFO(STB_GLOBAL, symType);
		if (oldSym.st_name != 0)
			newSym.st_name = copyStrtabItem(elf, outElf, oldSym.st_name);
	}

	memcpy((uint8_t *)data->d_buf + data->d_size, &newSym, sizeof(GElf_Sym));
	data->d_size += sizeof(GElf_Sym);
	shdr.sh_size = data->d_size;
	gelf_update_shdr(scn, &shdr);

	Symbols[index]->copiedIndex = newIndex;
	return newIndex;
}

static void copyRelSection(Elf *elf, Elf *outElf, Elf64_Section index, size_t relTo, GElf_Sym *fromSym, bool fullCopy)
{
	Elf_Scn *outScn = copySection(elf, outElf, index, false);
	GElf_Shdr shdr;
	gelf_getshdr(outScn, &shdr);
	shdr.sh_link = elf_ndxscn(getSectionByName(outElf, ".symtab"));
	shdr.sh_info = relTo;
	gelf_update_shdr(outScn, &shdr);

	GElf_Rela rela;
	size_t j = shdr.sh_size / shdr.sh_entsize;
	Elf_Scn *scn = elf_getscn(elf, index);
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
			(rela.r_offset < fromSym->st_value || rela.r_offset > fromSym->st_value + fromSym->st_size))
			continue;
		size_t symIndex = ELF64_R_SYM(rela.r_info);
		if (!Symbols[symIndex]->isFun && !Symbols[symIndex]->isVar)
		{
			int idx = getLinkedSymbol(Symbols[symIndex]);
			if (idx != -1 && (Symbols[idx]->isFun || Symbols[idx]->isVar))
				symIndex = idx;
		}
		size_t newSymIndex = copySymbol(elf, outElf, symIndex, fullCopy);
		rela.r_info = ELF64_R_INFO(newSymIndex, ELF64_R_TYPE(rela.r_info));
		gelf_update_rela(outData, j, &rela);
		j++;
	}
	gelf_getshdr(outScn, &shdr);
	shdr.sh_size = j * shdr.sh_entsize;
	outData->d_size = shdr.sh_size;
	if (!gelf_update_shdr(outScn, &shdr))
		LOG_ERR("gelf_update_shdr failed");
}

static void copySectionWithRel(Elf *elf, Elf *outElf, Elf64_Section index, GElf_Sym *fromSym, bool fullCopy)
{
	Elf_Scn *newScn = copySection(elf, outElf, index, true);
	Elf_Scn *relScn = getRelForSectionIndex(elf, index);
	if (relScn)
		copyRelSection(elf, outElf, elf_ndxscn(relScn), elf_ndxscn(newScn), fromSym, fullCopy);
}

static void copySymbols(Elf *elf, Elf *outElf, char **symbols)
{
	GElf_Shdr shdr;
	GElf_Sym sym;
	size_t symIndex;
	char **syms = symbols;
	while(*syms != NULL)
	{
		sym = getSymbolByName(elf, *syms, &symIndex);
		if (sym.st_name == 0)
			LOG_ERR("Can't find symbol: %s", *syms);
		Elf_Scn *newScn = copySection(elf, outElf, sym.st_shndx, true);
		size_t index = copySymbol(elf, outElf, symIndex, true);
		Elf_Scn *symScn = getSectionByName(outElf, ".symtab");
		gelf_getshdr(symScn, &shdr);
		Elf_Data *symData = elf_getdata(symScn, NULL);
		gelf_getsym(symData, index, &sym);
		sym.st_shndx = elf_ndxscn(newScn);
		gelf_update_sym(symData, index, &sym);
		syms++;
	}
	syms = symbols;
	while(*syms != NULL)
	{
		sym = getSymbolByName(elf, *syms, &symIndex);
		copySectionWithRel(elf, outElf, sym.st_shndx, &sym, false);
		syms++;
	}
	// needed for BUG()
	Elf_Scn *scn = getSectionByName(elf, "__bug_table");
	if (scn)
	{
		size_t index = elf_ndxscn(scn);
		copySectionWithRel(elf, outElf, index, NULL, true);
	}
	// TODO: Fix file path in string sections

	sortSymtab(outElf);

	elf_update(outElf, ELF_C_WRITE);
	elf_end(outElf);
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

static void symbolCallees(Elf *elf, Symbol *s, size_t *result)
{
	GElf_Shdr shdr;
	GElf_Rela rela;
	Elf_Scn *scn = getRelForSectionIndex(elf, s->secIndex);
	gelf_getshdr(scn, &shdr);
	Elf_Data *data = elf_getdata(scn, NULL);
	size_t cnt = shdr.sh_size / shdr.sh_entsize;
	for (size_t i = 0; i < cnt; i++)
	{
		gelf_getrela(data, i, &rela);
		size_t symIndex = ELF64_R_SYM(rela.r_info);
		if (symIndex >= SymbolsCount)
			LOG_ERR("Invalid symbol index: %ld in section relocation %ld", symIndex, elf_ndxscn(scn));
		if (Symbols[symIndex]->st_info == STT_SECTION)
		{
			size_t linkIndex = getLinkedFuncSymbol(Symbols[symIndex]);
			if (linkIndex != (size_t)-1)
				symIndex = linkIndex;
		}
		if (Symbols[symIndex]->isFun)
		{
			size_t *r = result;
			// add symIndex to result if result not contains it
			while (*r != 0)
			{
				if (*r == symIndex)
					break;
				r++;
			}
			if (*r == 0)
				*r = symIndex;
		}
	}
}

static void printCallees(Symbol *s, size_t *callStack)
{
	size_t *stack = callStack;
	while(*stack-- != 0)
	{
		if (*stack == s->index)
			return;
	}

	*callStack = s->index;
	size_t *calleeIdx = s->callees;
	if (*calleeIdx == 0)
	{
		do
		{
			printf("%s ", Symbols[*callStack]->name);
			callStack--;
		} while(*callStack != 0);
		puts("");
		return;
	}
	while(*calleeIdx != 0)
	{
		printCallees(Symbols[*calleeIdx], callStack + 1);
		calleeIdx++;
	}
}

static void help(const char *execName)
{
	error(EXIT_FAILURE, EINVAL, "Usage: %s [-diff|--callchain|--extract|--changeCallSymbol"
#ifdef SUPPORT_DISASSEMBLE
	"|--disassemble"
#endif
	"] ...", execName);
}

static void showDiff(int argc, char *argv[])
{
	char *firstFile;
	char *secondFile;
	int opt;
	while ((opt = getopt(argc, argv, "a:b:")) != -1)
	{
		switch (opt)
		{
		case 'a':
			firstFile = strdup(optarg);
			break;
		case 'b':
			secondFile = strdup(optarg);
			break;
		}
	}

	if (firstFile == NULL || secondFile == NULL)
		error(EXIT_FAILURE, EINVAL, "Invalid parameters to show difference between objects file. Valid parameters:"
			  "-a <ELF_FILE> -b <ELF_FILE> [-V]");

	int firstFd;
	int secondFd;
	Elf *firstElf = openElf(firstFile, &firstFd);
	Elf *secondElf = openElf(secondFile, &secondFd);
	findModifiedSymbols(secondElf, firstElf);
	close(firstFd);
	close(secondFd);
}

static void findCallChains(int argc, char *argv[])
{
	char *filePath = NULL;
	int opt;
	while ((opt = getopt(argc, argv, "f:")) != -1)
	{
		switch (opt)
		{
		case 'f':
			filePath = strdup(optarg);
			break;
		}
	}

	if (filePath == NULL)
		error(EXIT_FAILURE, EINVAL, "Invalid parameters to print call chain. Valid parameters:"
			  "-f <ELF_FILE>");

	int fd;
	Elf *elf = openElf(filePath, &fd);
	Symbols = readSymbols(elf);
	for (Symbol **s = Symbols; *s != NULL; s++)
	{
		if (s[0]->isFun)
		{
			s[0]->callees = calloc(SymbolsCount, sizeof(size_t));
			CHECK_ALLOC(s[0]->callees);
			symbolCallees(elf, s[0], s[0]->callees);
		}
	}
	size_t *callStack = malloc(SymbolsCount * sizeof(size_t));
	CHECK_ALLOC(callStack);
	for (Symbol **s = Symbols; *s != NULL; s++)
	{
		if (s[0]->isFun)
		{
			memset(callStack, 0, SymbolsCount * sizeof(size_t));
			printCallees(s[0], callStack + 1);
		}
	}
	free(callStack);
	close(fd);
}

static void extractSymbols(int argc, char *argv[])
{
	char *filePath = NULL;
	char *outFile = NULL;
	char **symToCopy = calloc(argc, sizeof(char *));
	CHECK_ALLOC(symToCopy);
	char **syms;
	int opt;
	while ((opt = getopt(argc, argv, "f:o:s:")) != -1)
	{
		switch (opt)
		{
		case 'f':
			filePath = strdup(optarg);
			break;
		case 'o':
			outFile = strdup(optarg);
			break;
		case 's':
			syms = symToCopy;
			while (*syms != NULL)
			{
				if (strcmp(*syms, optarg) == 0)
					break;
				syms++;
			}
			if (*syms == NULL)
				*syms = strdup(optarg);
			break;
		}
	}

	if (filePath == NULL || outFile == NULL || *symToCopy == NULL)
		error(EXIT_FAILURE, EINVAL, "Invalid parameters to extract symbols. Valid parameters:"
			  "-f <ELF_FILE> -o <OUT_FILE> -s <SYMBOL_NAME> [-V]");

	int fd;
	Elf *pelf = openElf(filePath, &fd);

	elf_getshdrnum(pelf, &SectionsCount);
	CopiedScnMap = calloc(SectionsCount, sizeof(Elf_Scn *));
	CHECK_ALLOC(CopiedScnMap);

	Elf *outElf = createNewElf(outFile);
	Symbols = readSymbols(pelf);
	copySymbols(pelf, outElf, symToCopy);

	for (Symbol **s = Symbols; *s != NULL; s++)
		free(s[0]);

	free(Symbols);
	free(CopiedScnMap);

	close(fd);
	free(filePath);
}

static void changeCallSymbol(int argc, char *argv[])
{
	char *filePath = NULL;
	char **symToCopy = calloc(argc, sizeof(char *));
	CHECK_ALLOC(symToCopy);
	int opt;
	char *fromRelSym = NULL;
	char *toRelSym = NULL;
	size_t replaced = 0;

	while ((opt = getopt(argc, argv, "s:d:vh")) != -1)
	{
		switch (opt)
		{
		case 's':
			fromRelSym = strdup(optarg);
			CHECK_ALLOC(fromRelSym);
			break;
		case 'd':
			toRelSym = strdup(optarg);
			CHECK_ALLOC(toRelSym);
			break;
		case 'v':
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

	if (optind - 1 < argc)
	{
		filePath = argv[optind++];
		while (optind < argc)
			LOG_ERR("Unknown parameter: %s", argv[optind++]);
	}

	if (filePath == NULL || fromRelSym == NULL || toRelSym== NULL)
		error(EXIT_FAILURE, EINVAL, "Invalid parameters to change calling function. Valid parameters:"
			  "-s <SYMBOL_NAME_SOURCE> -d <SYMBOL_NAME_DEST> [-v] <MODULE.ko>");

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

	close(fd);
	free(fromRelSym);
	free(toRelSym);

	if (replaced == 0)
		LOG_ERR("No relocation has been replaced");
}

#ifdef SUPPORT_DISASSEMBLE
static void disassemble(int argc, char *argv[])
{
	char *filePath = NULL;
	char *symName = NULL;
	int opt;
	while ((opt = getopt(argc, argv, "f:s:")) != -1)
	{
		switch (opt)
		{
		case 'f':
			filePath = strdup(optarg);
			break;
		case 's':
			symName = strdup(optarg);
			break;
		}
	}

	if (filePath == NULL || symName == NULL)
		error(EXIT_FAILURE, 0, "Invalid parameters to disassemble. Valid parameters:"
			  "-f <ELF_FILE> -s <SYMBOL_NAME>");

	int fd;
	Elf *elf = openElf(filePath, &fd);
	GElf_Sym sym;
	if (!getSymbolByNameAndType(elf, symName, STT_FUNC, &sym))
		LOG_ERR("Can't find symbol %s", symName);

	GElf_Shdr shdr;
	gelf_getshdr(getSectionByName(elf, ".symtab"), &shdr);
	SymbolData symData = getSymbolData(elf, symName, STT_FUNC, true);
	DisasmData data = { .elf = elf, .sym = sym, .shdr = shdr, .symData = &symData };

	char *disassembled = disassembleBytes(data.symData->data, data.symData->size, &data);
	puts(disassembled);

	free(disassembled);
	free(symName);
	free(filePath);
	close(fd);
}
#endif

int main(int argc, char *argv[])
{
	bool showDiffElf = false;
	bool showCallChain = false;
	bool extractSym = false;
	bool changeCallSym = false;
#ifdef SUPPORT_DISASSEMBLE
	bool disasm = false;
#endif
	for (int i = 1; i < argc; i++)
	{
		if (strcmp(argv[i], "--diff") == 0)
			showDiffElf = true;
		if (strcmp(argv[i], "--callchain") == 0)
			showCallChain = true;
		if (strcmp(argv[i], "--extract") == 0)
			extractSym = true;
		if (strcmp(argv[i], "--changeCallSymbol") == 0)
			changeCallSym = true;
#ifdef SUPPORT_DISASSEMBLE
		if (strcmp(argv[i], "--disassemble") == 0)
			disasm = true;
#endif
		if (strcmp(argv[i], "-V") == 0)
			ShowDebugLog = true;
	}
	elf_version(EV_CURRENT);

	if (showDiffElf)
		showDiff(argc - 1, argv + 1);
	else if (showCallChain)
		findCallChains(argc - 1, argv + 1);
	else if (extractSym)
		extractSymbols(argc - 1, argv + 1);
	else if (changeCallSym)
		changeCallSymbol(argc - 1, argv + 1);
#ifdef SUPPORT_DISASSEMBLE
	else if (disasm)
		disassemble(argc - 1, argv + 1);
#endif
	else
		help(argv[0]);
	return 0;
}
