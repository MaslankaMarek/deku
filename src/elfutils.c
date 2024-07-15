/*
* Author: Marek Maślanka
* Project: DEKU
* URL: https://github.com/MarekMaslanka/deku
*/

#include <errno.h>
#include <error.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <getopt.h>

#include <gelf.h>
#include "elfutils.h"

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

extern bool ShowDebugLog;

static void help(const char *execName)
{
	error(EXIT_FAILURE, EINVAL, "Usage: %s [-diff|--callchain|--extract|"
										   "--changeCallSymbol|--disassemble"
										   "--referenceFrom"
										   "] ...", execName);
}

static void _showDiff(int argc, char *argv[])
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

	char *result = showDiff(firstFile, secondFile);
	puts(result);

	free(secondFile);
	free(firstFile);
}

static void _findCallChains(int argc, char *argv[])
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

	findCallChains(filePath);
}

static void _extractSymbols(int argc, char *argv[])
{
	char *filePath = NULL;
	char *outFile = NULL;
	char *symToCopy = calloc(1, sizeof(char));
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
			symToCopy = realloc(symToCopy, strlen(symToCopy) + strlen(optarg) + 2);
			CHECK_ALLOC(symToCopy);
			strcat(symToCopy, optarg);
			strcat(symToCopy, ",");
			break;
		}
	}

	if (filePath == NULL || outFile == NULL || symToCopy[0] == '\0')
		error(EXIT_FAILURE, EINVAL, "Invalid parameters to extract symbols. Valid parameters:"
			  "-f <ELF_FILE> -o <OUT_FILE> -s <SYMBOL_NAME> [-n <SKIP_DEP_SYMBOL>] [-V]");

	symToCopy[strlen(symToCopy)-1] = '\0';
	extractSymbols(filePath, outFile, symToCopy);

	free(symToCopy);
	free(outFile);
	free(filePath);
}

static void _changeCallSymbol(int argc, char *argv[])
{
	char *filePath = NULL;
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

	replaced = changeCallSymbol(filePath, fromRelSym, toRelSym);

	free(fromRelSym);
	free(toRelSym);

	if (replaced == 0)
		LOG_ERR("No relocation has been replaced");
}

static void _disassemble(int argc, char *argv[])
{
	char *filePath = NULL;
	char *symName = NULL;
	bool convertToReloc = false;
	int opt;
	while ((opt = getopt(argc, argv, "f:s:r")) != -1)
	{
		switch (opt)
		{
		case 'f':
			filePath = strdup(optarg);
			break;
		case 's':
			symName = strdup(optarg);
			break;
		case 'r':
			convertToReloc = true;
			break;
		}
	}

	if (filePath == NULL || symName == NULL)
		error(EXIT_FAILURE, 0, "Invalid parameters to disassemble. Valid parameters:"
			  "-f <ELF_FILE> -s <SYMBOL_NAME>");

	char *disassembled = disassemble(filePath, symName, convertToReloc);
	if (strlen(disassembled) > 0)
		disassembled[strlen(disassembled) - 1] = '\0';
	puts(disassembled);

#ifdef OUTPUT_DISASSEMBLY_TO_FILE
	FILE  *fptr = fopen("disassembly", "w");
	if(fptr == NULL)
		LOG_ERR("Can't open output file for disassembly");
	fputs(disassembled, fptr);
	fclose(fptr);
#endif
	free(disassembled);
	free(symName);
	free(filePath);
}

static void _symbolReferenceFrom(int argc, char *argv[])
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
		error(EXIT_FAILURE, 0, "Invalid parameters to find symbols referenced to. Valid parameters:"
			  "-f <ELF_FILE> -s <SYMBOL_NAME>");

	char *syms = symbolReferenceFrom(filePath, symName);
	printf("%s", syms);

	free(syms);
	free(symName);
	free(filePath);
}

int main(int argc, char *argv[])
{
	bool showDiffElf = false;
	bool showCallChain = false;
	bool extractSym = false;
	bool changeCallSym = false;
	bool disasm = false;
	bool referenceFrom = false;
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
		if (strcmp(argv[i], "--disassemble") == 0)
			disasm = true;
		if (strcmp(argv[i], "--referenceFrom") == 0)
			referenceFrom = true;
	}
	elf_version(EV_CURRENT);

	if (showDiffElf)
		_showDiff(argc - 1, argv + 1);
	else if (showCallChain)
		_findCallChains(argc - 1, argv + 1);
	else if (extractSym)
		_extractSymbols(argc - 1, argv + 1);
	else if (changeCallSym)
		_changeCallSymbol(argc - 1, argv + 1);
	else if (disasm)
		_disassemble(argc - 1, argv + 1);
	else if (referenceFrom)
		_symbolReferenceFrom(argc - 1, argv + 1);
	else
		help(argv[0]);
	return 0;
}
