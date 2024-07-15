package main

import (
	"debug/elf"
	"errors"
	"fmt"
	"os"
	"strings"
	"unsafe"
)

type ELF struct {
	file     *elf.File
	Sections []*elf.Section
	Symbols  []elf.Symbol
	Strtab   elf.Section
}

func Open(file string) (*ELF, error) {
	e := ELF{
		file:     nil,
		Sections: nil,
		Symbols:  nil,
	}
	f, err := elf.Open(file)
	if err != nil {
		fmt.Printf("Couldn’t open ELF file: \"%s\". %q\n", file, err)
		return &e, err
	}
	e.file = f
	e.Sections = f.Sections
	e.Symbols, _ = f.Symbols()

	for _, section := range e.Sections {
		if section.Type == elf.SHT_STRTAB {
			e.Strtab = *section
			break
		}
	}
	return &e, nil
}

func (e ELF) Close() {
	err := e.file.Close()
	if err != nil {
		fmt.Fprintf(os.Stderr, "error: %v\n", err)
		os.Exit(1)
	}
}

func (e ELF) getSectionByName(sectionName string) (*elf.Section, error) {
	for _, section := range e.Sections {
		if section.Name == sectionName {
			return section, nil
		}
	}

	return nil, errors.New(fmt.Sprint("Can't find section %s", sectionName))
}

func (e ELF) getSymbolByName(name string) (elf.Symbol, uint32, error) {
	for i, sym := range e.Symbols {
		if sym.Name == name {
			return sym, uint32(i + 1), nil
		}
	}
	return elf.Symbol{}, 0, errors.New(fmt.Sprintf("Can't find symbol with name: ", name))
}

func (e ELF) getRelocsForFunction(funName string) ([]elf.Rela64, error) {
	var result []elf.Rela64
	sym, _, err := e.getSymbolByName(funName)
	if err != nil {
		return nil, err
	}
	for _, section := range e.Sections {
		if section.Type == elf.SHT_RELA && section.Info == uint32(sym.Section) {
			for i := 0; i < int(section.Size); i += elf.Sym64Size {
				bytes := make([]byte, elf.Sym64Size)
				section.ReadAt(bytes, int64(i))
				rela := *(*elf.Rela64)(unsafe.Pointer(&bytes[0]))
				if rela.Off >= sym.Value && rela.Off < sym.Value+sym.Size {
					result = append(result, rela)
				}
			}
		}
	}
	return result, nil
}

func checkIsTraceable(file string, funName string) (bool, []string) {
	e, err := Open(file)
	if err != nil {
		return false, []string{}
	}
	defer e.Close()

	_, fentryIdx, err := e.getSymbolByName("__fentry__")
	if err != nil {
		return false, []string{}
	}

	var isTraceable = func(funName string) bool {
		relocs, err := e.getRelocsForFunction(funName)
		if err == nil && len(relocs) > 0 {
			if elf.R_SYM64(relocs[0].Info) == fentryIdx {
				return true
			}
		}
		return false
	}

	if isTraceable(funName) {
		return true, []string{}
	}

	sym, _, _ := e.getSymbolByName(funName)
	if elf.ST_BIND(sym.Info) != elf.STB_LOCAL {
		LOG_DEBUG("The '%s' function is forbidden to modify. The function is non-local", funName)
		return false, []string{}
	}

	refersFrom := referenceFrom(file, funName)
	if len(refersFrom) == 0 {
		return false, []string{}
	}

	callers := []string{}
	LOG_DEBUG("The '%s' function is forbidden to modify. This function is called from:\n%s", funName, refersFrom)
	for _, rSym := range strings.Split(refersFrom, "\n") {
		if rSym[:2] == "v:" || !isTraceable(rSym[2:]) {
			return false, []string{}
		}
		callers = append(callers, rSym[2:])
	}

	return false, callers
}

func checkIfIsInitOrExit(file string, funName string) bool {
	e, err := Open(file)
	if err != nil {
		return false
	}
	defer e.Close()

	symbol, _, err := e.getSymbolByName(funName)
	if err != nil {
		return false
	}

	secName := e.Sections[symbol.Section].Name
	if secName == ".init.text" {
		LOG_INFO(fmt.Sprintf("The init function '%s' has been modified. Any changes made to this function will not be applied.", funName))
		return true
	}
	if secName == ".exit.text" {
		LOG_INFO(fmt.Sprintf("The exit function '%s' has been modified. Any changes made to this function will not be applied.", funName))
		return true
	}

	return false
}

func getFunctionsName(file string) ([]string, error) {
	var funcs []string
	e, err := Open(file)
	if err != nil {
		return nil, err
	}
	defer e.Close()

	for _, symbol := range e.Symbols {
		if elf.ST_TYPE(symbol.Info) == elf.STT_FUNC {
			funcs = append(funcs, symbol.Name)
		}
	}

	return funcs, err
}

func getVariablesName(file string) ([]string, error) {
	var funcs []string
	e, err := Open(file)
	if err != nil {
		return nil, err
	}
	defer e.Close()

	for _, symbol := range e.Symbols {
		if elf.ST_TYPE(symbol.Info) == elf.STT_OBJECT {
			funcs = append(funcs, symbol.Name)
		}
	}

	return funcs, nil
}

func getUndefinedSymbols(objFile string) ([]elf.Symbol, error) {
	var symbols []elf.Symbol

	e, err := Open(objFile)
	if err != nil {
		return nil, err
	}
	defer e.Close()

	for _, symbol := range e.Symbols {
		if symbol.Section == 0 && len(symbol.Name) > 0 &&
			(elf.ST_TYPE(symbol.Info) == elf.STT_OBJECT ||
				elf.ST_TYPE(symbol.Info) == elf.STT_FUNC ||
				elf.ST_TYPE(symbol.Info) == elf.STT_NOTYPE) {
			symbols = append(symbols, symbol)
		}
	}

	return symbols, nil
}
