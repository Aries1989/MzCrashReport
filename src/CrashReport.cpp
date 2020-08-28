//
// CrashReport.cpp
//
// Created by liuliu.mz on 20-08-26
//

#include <dbghelp.h>
#include <string>

#include "CrashReport.h"
#include "CrashUtils.h"
#include "cvconst.h"

// 
#define FMT_BUFFER_LEN 1024
char FmtBuffer[FMT_BUFFER_LEN] = {};

std::string CurrentPath;

// �쳣��ת�ַ���
std::string ExceptionCodeToString(DWORD ecode)
{
	#define EXCEPTION(x) case EXCEPTION_##x: return (#x);
	switch (ecode)
	{
		EXCEPTION(ACCESS_VIOLATION)
		EXCEPTION(DATATYPE_MISALIGNMENT)
		EXCEPTION(BREAKPOINT)
		EXCEPTION(SINGLE_STEP)
		EXCEPTION(ARRAY_BOUNDS_EXCEEDED)
		EXCEPTION(FLT_DENORMAL_OPERAND)
		EXCEPTION(FLT_DIVIDE_BY_ZERO)
		EXCEPTION(FLT_INEXACT_RESULT)
		EXCEPTION(FLT_INVALID_OPERATION)
		EXCEPTION(FLT_OVERFLOW)
		EXCEPTION(FLT_STACK_CHECK)
		EXCEPTION(FLT_UNDERFLOW)
		EXCEPTION(INT_DIVIDE_BY_ZERO)
		EXCEPTION(INT_OVERFLOW)
		EXCEPTION(PRIV_INSTRUCTION)
		EXCEPTION(IN_PAGE_ERROR)
		EXCEPTION(ILLEGAL_INSTRUCTION)
		EXCEPTION(NONCONTINUABLE_EXCEPTION)
		EXCEPTION(STACK_OVERFLOW)
		EXCEPTION(INVALID_DISPOSITION)
		EXCEPTION(GUARD_PAGE)
		EXCEPTION(INVALID_HANDLE)
	}

	// If not one of the "known" exceptions, try to get the string
	// from NTDLL.DLL's message table.
	FormatMessageA(FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_FROM_HMODULE, GetModuleHandleA("NTDLL.DLL"), ecode, 0, FmtBuffer, sizeof(FmtBuffer), 0);

	return std::string(FmtBuffer);
}

#define MAX_NAME_LEN 1024
BYTE symbolBuffer[sizeof(SYMBOL_INFO) + MAX_NAME_LEN] = {};

std::string DumpSymbolName(DWORD dwLevel, const STACKFRAME& sf)
{
	DWORD dwAddress = sf.AddrPC.Offset;

	PSYMBOL_INFO pSymbol = (PSYMBOL_INFO)symbolBuffer;
	pSymbol->SizeOfStruct = sizeof(SYMBOL_INFO);
	pSymbol->MaxNameLen = MAX_NAME_LEN;
	DWORD64 symDisplacement = 0;
	if (!::SymFromAddr(CurrentProcess, dwAddress, &symDisplacement, pSymbol))
	{
		// LOG_LAST_ERROR();
		sprintf_s(FmtBuffer, "%08X, SymFromAddr Failed \n", dwAddress);
		return FmtBuffer;
	}

	IMAGEHLP_LINE line = { sizeof(IMAGEHLP_LINE) };
	DWORD dwLineDisplacement = 0;
	if (!::SymGetLineFromAddr(CurrentProcess, dwAddress, &dwLineDisplacement, &line))
	{
		// it is normal that we don't have source info for some symbols,
		// notably all the ones from the system DLLs...
		// sprintf_s(FmtBuffer, "SymGetLineFromAddr fail 0x%08x\n", dwAddress);
		// return "b";
		sprintf_s(FmtBuffer, "%08I64X, %s()\n", pSymbol->Address, pSymbol->Name);
		return FmtBuffer;
	}

	sprintf_s(FmtBuffer, "%08I64X, %s(), line %u in\n %s\n", pSymbol->Address, pSymbol->Name, line.LineNumber, line.FileName);
	return FmtBuffer;
}

BasicType GetBasicType(DWORD typeIndex, DWORD64 modBase)
{
	BasicType basicType;
	if (::SymGetTypeInfo(CurrentProcess, modBase, typeIndex,
		TI_GET_BASETYPE, &basicType))
	{
		return basicType;
	}

	// Get the real "TypeId" of the child.  We need this for the
	// SymGetTypeInfo( TI_GET_TYPEID ) call below.
	DWORD typeId;
	if (::SymGetTypeInfo(CurrentProcess, modBase, typeIndex, TI_GET_TYPEID, &typeId))
	{
		if (::SymGetTypeInfo(CurrentProcess, modBase, typeId, TI_GET_BASETYPE,
			&basicType))
		{
			return basicType;
		}
	}

	return btNoType;
}

char* FormatOutputValue(char* pszCurrBuffer, BasicType basicType, DWORD64 length, PVOID pAddress)
{
	// Format appropriately (assuming it's a 1, 2, or 4 bytes (!!!)
	if (length == 1)
		pszCurrBuffer += sprintf(pszCurrBuffer, " = %X", *(PBYTE)pAddress);
	else if (length == 2)
		pszCurrBuffer += sprintf(pszCurrBuffer, " = %X", *(PWORD)pAddress);
	else if (length == 4)
	{
		if (basicType == btFloat)
		{
			pszCurrBuffer += sprintf(pszCurrBuffer, " = %f", *(PFLOAT)pAddress);
		}
		else if (basicType == btChar)
		{
			if (!IsBadStringPtrA(*(PSTR*)pAddress, 32))
			{
				pszCurrBuffer += sprintf(pszCurrBuffer, " = \"%.31s\"",
					*(PDWORD)pAddress);
			}
			else
				pszCurrBuffer += sprintf(pszCurrBuffer, " = %X",
				*(PDWORD)pAddress);
		}
		else
			pszCurrBuffer += sprintf(pszCurrBuffer, " = %X", *(PDWORD)pAddress);
	}
	else if (length == 8)
	{
		if (basicType == btFloat)
		{
			pszCurrBuffer += sprintf(pszCurrBuffer, " = %lf",
				*(double *)pAddress);
		}
		else
			pszCurrBuffer += sprintf(pszCurrBuffer, " = %I64X",
			*(DWORD64*)pAddress);
	}

	return pszCurrBuffer;
}

// If it's a user defined type (UDT), recurse through its members until we're
// at fundamental types.  When he hit fundamental types, return
// bHandled = false, so that FormatSymbolValue() will format them.
char* DumpTypeIndex(char* pszCurrBuffer, DWORD64 modBase, DWORD dwTypeIndex,
	unsigned nestingLevel, DWORD_PTR offset, bool & bHandled, char* Name)
{
	bHandled = false;

	// Get the name of the symbol.  This will either be a Type name (if a UDT),
	// or the structure member name.
	WCHAR * pwszTypeName;
	if (::SymGetTypeInfo(CurrentProcess, modBase, dwTypeIndex, TI_GET_SYMNAME,
		&pwszTypeName))
	{
		pszCurrBuffer += sprintf(pszCurrBuffer, " %ls", pwszTypeName);
		LocalFree(pwszTypeName);
	}

	// Determine how many children this type has.
	DWORD dwChildrenCount = 0;
	::SymGetTypeInfo(CurrentProcess, modBase, dwTypeIndex, TI_GET_CHILDRENCOUNT,
		&dwChildrenCount);

	if (!dwChildrenCount)                                 // If no children, we're done
		return pszCurrBuffer;

	// Prepare to get an array of "TypeIds", representing each of the children.
	// SymGetTypeInfo(TI_FINDCHILDREN) expects more memory than just a
	// TI_FINDCHILDREN_PARAMS struct has.  Use derivation to accomplish this.
	struct FINDCHILDREN : TI_FINDCHILDREN_PARAMS
	{
		ULONG   MoreChildIds[1024];
		FINDCHILDREN(){ Count = sizeof(MoreChildIds) / sizeof(MoreChildIds[0]); }
	} children;

	children.Count = dwChildrenCount;
	children.Start = 0;

	// Get the array of TypeIds, one for each child type
	if (!::SymGetTypeInfo(CurrentProcess, modBase, dwTypeIndex, TI_FINDCHILDREN,
		&children))
	{
		return pszCurrBuffer;
	}

	// Append a line feed
	pszCurrBuffer += sprintf(pszCurrBuffer, "\r\n");

	// Iterate through each of the children
	for (unsigned i = 0; i < dwChildrenCount; i++)
	{
		// Add appropriate indentation level (since this routine is recursive)
		for (unsigned j = 0; j <= nestingLevel + 1; j++)
			pszCurrBuffer += sprintf(pszCurrBuffer, "\t");

		// Recurse for each of the child types
		bool bHandled2;
		BasicType basicType = GetBasicType(children.ChildId[i], modBase);
		pszCurrBuffer += sprintf(pszCurrBuffer, rgBaseType[basicType]);

		pszCurrBuffer = DumpTypeIndex(pszCurrBuffer, modBase,
			children.ChildId[i], nestingLevel + 1,
			offset, bHandled2, ""/*Name */);

		// If the child wasn't a UDT, format it appropriately
		if (!bHandled2)
		{
			// Get the offset of the child member, relative to its parent
			DWORD dwMemberOffset;
			::SymGetTypeInfo(CurrentProcess, modBase, children.ChildId[i],
				TI_GET_OFFSET, &dwMemberOffset);

			// Get the real "TypeId" of the child.  We need this for the
			// SymGetTypeInfo( TI_GET_TYPEID ) call below.
			DWORD typeId;
			::SymGetTypeInfo(CurrentProcess, modBase, children.ChildId[i],
				TI_GET_TYPEID, &typeId);

			// Get the size of the child member
			ULONG64 length;
			::SymGetTypeInfo(CurrentProcess, modBase, typeId, TI_GET_LENGTH, &length);

			// Calculate the address of the member
			DWORD_PTR dwFinalOffset = offset + dwMemberOffset;
			pszCurrBuffer = FormatOutputValue(pszCurrBuffer, basicType,
				length, (PVOID)dwFinalOffset);

			pszCurrBuffer += sprintf(pszCurrBuffer, "\r\n");
		}
	}

	bHandled = true;
	return pszCurrBuffer;
}

// Given a SYMBOL_INFO representing a particular variable, displays its
// contents.  If it's a user defined type, display the members and their
// values.
bool FormatSymbolValue(PSYMBOL_INFO pSym, STACKFRAME* sf, char* pszBuffer, unsigned cbBuffer)
{
	char* pszCurrBuffer = pszBuffer;

	// Indicate if the variable is a local or parameter
	if (pSym->Flags & IMAGEHLP_SYMBOL_INFO_PARAMETER)
		pszCurrBuffer += sprintf(pszCurrBuffer, "Parameter ");
	else if (pSym->Flags & IMAGEHLP_SYMBOL_INFO_LOCAL)
		pszCurrBuffer += sprintf(pszCurrBuffer, "Local ");

	// If it's a function, don't do anything.
	if (pSym->Tag == 5)                                   // SymTagFunction from CVCONST.H from the DIA SDK
		return false;

	DWORD_PTR pVariable = 0;                                // Will point to the variable's data in memory

	if (pSym->Flags & IMAGEHLP_SYMBOL_INFO_REGRELATIVE)
	{
		// if ( pSym->Register == 8 )   // EBP is the value 8 (in DBGHELP 5.1)
		{                                                   //  This may change!!!
			pVariable = sf->AddrFrame.Offset;
			pVariable += (DWORD_PTR)pSym->Address;
		}
		// else
		//  return false;
	}
	else if (pSym->Flags & IMAGEHLP_SYMBOL_INFO_REGISTER)
	{
		return false;                                       // Don't try to report register variable
	}
	else
	{
		pVariable = (DWORD_PTR)pSym->Address;               // It must be a global variable
	}

	// Determine if the variable is a user defined type (UDT).  IF so, bHandled
	// will return true.
	bool bHandled;
	pszCurrBuffer = DumpTypeIndex(pszCurrBuffer, pSym->ModBase, pSym->TypeIndex,
		0, pVariable, bHandled, pSym->Name);

	if (!bHandled)
	{
		// The symbol wasn't a UDT, so do basic, stupid formatting of the
		// variable.  Based on the size, we're assuming it's a char, WORD, or
		// DWORD.
		BasicType basicType = GetBasicType(pSym->TypeIndex, pSym->ModBase);
		pszCurrBuffer += sprintf(pszCurrBuffer, rgBaseType[basicType]);

		// Emit the variable name
		pszCurrBuffer += sprintf(pszCurrBuffer, "\'%s\'", pSym->Name);

		pszCurrBuffer = FormatOutputValue(pszCurrBuffer, basicType, pSym->Size,
			(PVOID)pVariable);
	}

	return true;
}

struct UserContext
{
	STACKFRAME* sf;
	std::string ret;
};

char szBuffer[2048];

BOOL CALLBACK EnumSymbolsProcCallback(PSYMBOL_INFO pSymInfo, ULONG SymSize, PVOID userContext)
{
	UserContext* uc = (UserContext*)userContext;

	// we're only interested in parameters and local variables
	if (pSymInfo->Flags & SYMF_PARAMETER || pSymInfo->Flags & SYMF_LOCAL)
	{
		__try
		{
			if (FormatSymbolValue(pSymInfo, uc->sf, szBuffer, _countof(szBuffer)))
			{
				sprintf(FmtBuffer, "\t%s\n", szBuffer);
				uc->ret.append(FmtBuffer);
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
	}

	// return true to continue enumeration, false would have stopped it
	return TRUE;
}

std::string DumpSymbolParam(STACKFRAME& sf)
{
	UserContext uc;

	DWORD dwSymAddr = sf.AddrPC.Offset;
	DWORD dwAddrFrame = sf.AddrFrame.Offset;

	// use SymSetContext to get just the locals/params for this frame
	
	IMAGEHLP_STACK_FRAME imagehlpStackFrame = {};
	imagehlpStackFrame.InstructionOffset = dwSymAddr;
	if (!::SymSetContext(CurrentProcess, &imagehlpStackFrame, 0))
	{
		// for symbols from kernel DLL we might not have access to their
		// address, this is not a real error
		sprintf_s(FmtBuffer, "%08X, SymSetContext fail\n", dwSymAddr);
		return FmtBuffer;
	}
	
	uc.sf = &sf;
	if (!::SymEnumSymbols(
		CurrentProcess,
		NULL,           // DLL base: use current context
		NULL,           // no mask, get all symbols
		EnumSymbolsProcCallback,
		(PVOID)&uc))    // data parameter for this callback
	{
		sprintf_s(FmtBuffer, "%08X, SymSetContext fail\n", dwSymAddr);
		return FmtBuffer;
	}

	return uc.ret;
}

std::string WalkCallStack(CONTEXT ctx, size_t skip, size_t depth = 20)
{
	DWORD dwMachineType;

	// initialize the initial frame: currently we can do it for x86 only
	STACKFRAME sf = {};
#if defined(_M_AMD64)
	sf.AddrPC.Offset = ctx.Rip;
	sf.AddrPC.Mode = AddrModeFlat;
	sf.AddrStack.Offset = ctx.Rsp;
	sf.AddrStack.Mode = AddrModeFlat;
	sf.AddrFrame.Offset = ctx.Rbp;
	sf.AddrFrame.Mode = AddrModeFlat;
	dwMachineType = IMAGE_FILE_MACHINE_AMD64;
#elif defined(_M_IX86)
	sf.AddrPC.Offset = ctx.Eip;
	sf.AddrPC.Mode = AddrModeFlat;
	sf.AddrStack.Offset = ctx.Esp;
	sf.AddrStack.Mode = AddrModeFlat;
	sf.AddrFrame.Offset = ctx.Ebp;
	sf.AddrFrame.Mode = AddrModeFlat;
	dwMachineType = IMAGE_FILE_MACHINE_I386;
#else
#error "Need to initialize STACKFRAME on non x86"
#endif // _M_IX86

	std::string ret;

	HANDLE CurrentThread = GetCurrentThread();
	// iterate over all stack frames
	for (size_t nLevel = 0; nLevel < depth; nLevel++)
	{
		if (!::StackWalk(
			dwMachineType,
			CurrentProcess,
			CurrentThread,
			&sf,
			&ctx,
			nullptr,   // read memory function (default)
			::SymFunctionTableAccess,
			::SymGetModuleBase,
			nullptr))  // address translator for 16 bit
		{
			break;
		}

		//Invalid frame
		if (!sf.AddrFrame.Offset)
		{
			break;
		}

		// don't show this frame itself in the output
		// if (nLevel >= skip)
		{
			ret.append(DumpSymbolName(nLevel - skip, sf));
			ret.append(DumpSymbolParam(sf));
			ret.append("\n");
		}
	}

	return ret;
}

void ShowRpt(PEXCEPTION_POINTERS eps)
{
	if (!eps)
	{
		::MessageBoxA(0, "invaild eps", "CrashRpt 2020", 0);
	}
	else
	{
		std::string Rpt = "";

		DWORD ExceptionCode = eps->ExceptionRecord->ExceptionCode;
		PVOID ExceptionAddress = eps->ExceptionRecord->ExceptionAddress;

		MEMORY_BASIC_INFORMATION mbi;
		if (VirtualQuery(ExceptionAddress, &mbi, sizeof(mbi)))
		{
			if (GetModuleFileNameA((HMODULE)mbi.AllocationBase, FmtBuffer, MAX_PATH))
			{
				Rpt.append("ModuleFileName: ");
				Rpt.append(FmtBuffer);
				Rpt.append("\n\n");
			}
		}

		sprintf_s(FmtBuffer, "CurrentThreadId: %d\n\n", GetCurrentThreadId());
		Rpt.append(FmtBuffer);

		sprintf_s(FmtBuffer, "ExceptionAddress: 0x%08X\n", (DWORD)(ExceptionAddress));
		Rpt.append(FmtBuffer);

		if (ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
		{
			ULONG_PTR* ExceptionInformation = eps->ExceptionRecord->ExceptionInformation;
			if (ExceptionInformation[0])
			{
				sprintf_s(FmtBuffer, "Failed to write address 0x%08X\n", ExceptionInformation[1]);
			}
			else
			{
				sprintf_s(FmtBuffer, "Failed to read address 0x%08X\n", ExceptionInformation[1]);
			}

			Rpt.append(FmtBuffer);
		}
		Rpt.append("\n");

		std::string code = ExceptionCodeToString(ExceptionCode);
		sprintf_s(FmtBuffer, "ExceptionCode: 0x%08X (%s)\n\n", ExceptionCode, code.c_str());
		Rpt.append(FmtBuffer);

		Rpt.append(("Call Stack:\n------------------------------------------------------\n"));

		if (!::SymInitialize(CurrentProcess, nullptr, TRUE))
		{
			Rpt.append("SymInitialize Error");
		}
		else
		{
			Rpt.append(WalkCallStack(*eps->ContextRecord, 0));
		}

		//
		::MessageBoxA(0, Rpt.c_str(), "CrashRpt Mz", 0);
	}
}

void InitCrashReport()
{
	CurrentProcess = GetCurrentProcess();



	SetUnhandledExceptionFilter(SEH_Handler);
}
