package main

/*
#include <stdlib.h>
#include "../elfutils.h"

#cgo LDFLAGS: -L../.. elfutils.o -lopcodes -lbfd -lz -liberty -lelf
*/
import "C"
import (
	"errors"
	"os"
	"os/exec"
	"strings"
	"unsafe"
)

func showDiff(filepathA, filepathB string) (string, error) {
	if USE_EXTERNAL_EXECUTABLE {
		out, err := exec.Command("./elfutils", "--diff", "-a", filepathA, "-b",
			filepathB).CombinedOutput()
		if err != nil {
			LOG_ERR(errors.New(string(out)), "Can't find modified functions for %s", filepathA)
			return "", err
		}
		diff := string(out)
		return diff, nil
	} else {
		fileA := C.CString(filepathA)
		fileB := C.CString(filepathB)
		diff := C.GoString(C.showDiff(fileA, fileB))
		C.free(unsafe.Pointer(fileB))
		C.free(unsafe.Pointer(fileA))

		return diff, nil
	}
}

// func findCallChains(filePath string) error {
// 	path := C.CString(filePath)
// 	C.findCallChains(path)
// 	C.free(unsafe.Pointer(path))

// 	return nil
// }

func extractSymbols(filePath, outFile string, symToCopy []string) error {
	if USE_EXTERNAL_EXECUTABLE {
		cmd := exec.Command("./elfutils", "--extract", "-f", filePath, "-o", outFile)
		for _, sym := range symToCopy {
			cmd.Args = append(cmd.Args, "-s")
			cmd.Args = append(cmd.Args, sym)
		}
		out, err := cmd.CombinedOutput()
		if err != nil {
			LOG_ERR(errors.New(string(out)), "Failed to extract modified symbols for %s", filePath)
			// return errors.New("ERROR_EXTRACT_SYMBOLS")
			return err
		}
		LOG_INFO("%s", out)
	} else {
		path := C.CString(filePath)
		out := C.CString(outFile)
		syms := C.CString(strings.Join(symToCopy, ","))
		C.extractSymbols(path, out, syms)
		C.free(unsafe.Pointer(syms))
		C.free(unsafe.Pointer(out))
		C.free(unsafe.Pointer(path))
	}
	return nil
}

func changeCallSymbol(filePath, fromRelSym, toRelSym string) error {
	if USE_EXTERNAL_EXECUTABLE {
		cmd := exec.Command("./elfutils", "--changeCallSymbol", "-s", fromRelSym,
			"-d", toRelSym, filePath)
		cmd.Stderr = os.Stderr
		return cmd.Run()
	} else {
		file := C.CString(filePath)
		srcRelSym := C.CString(fromRelSym)
		dstRelSym := C.CString(toRelSym)
		C.changeCallSymbol(file, srcRelSym, dstRelSym)
		C.free(unsafe.Pointer(dstRelSym))
		C.free(unsafe.Pointer(srcRelSym))
		C.free(unsafe.Pointer(file))

		return nil
	}
}

func referenceFrom(file, funName string) string {
	if USE_EXTERNAL_EXECUTABLE {
		out, err := exec.Command("elfutils", "--referenceFrom", "-f", file, "-s", funName).Output()
		if err != nil {
			return ""
		}
		return strings.TrimSuffix(string(out), "\n")
	} else {
		path := C.CString(file)
		symName := C.CString(funName)
		refersFrom := C.GoString(C.symbolReferenceFrom(path, symName))
		C.free(unsafe.Pointer(symName))
		C.free(unsafe.Pointer(path))

		return strings.TrimSuffix(refersFrom, "\n")
	}
}
