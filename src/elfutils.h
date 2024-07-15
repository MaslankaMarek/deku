#include <stdbool.h>

char *showDiff(char *firstFile, char *secondFile);
void findCallChains(char *filePath);
void extractSymbols(char *filePath, char *outFile, char *symToCopy);
size_t changeCallSymbol(char *filePath, char *fromRelSym, char *toRelSym);
char *disassemble(char *filePath, char *symName, bool convertToReloc);
char *symbolReferenceFrom(char *filePath, char *symName);
