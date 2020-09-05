#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <winternl.h>
#include <malloc.h>

#ifdef _M_AMD64
#include <intrin.h>
#elif defined(_M_ARM)
#include <armintr.h>
#endif

#ifdef _M_IX86 
static __inline PEB __declspec(naked) __forceinline *GetPEBx86()
{
	__asm
	{
		mov eax, dword ptr fs : [0x30];
		retn;
	}
}
#endif

HMODULE WINAPI GetModuleBaseAddress(LPCWSTR moduleName)
{
	PEB *pPeb = NULL;
	LIST_ENTRY *pListEntry = NULL;
	LDR_DATA_TABLE_ENTRY *pLdrDataTableEntry = NULL;

#ifdef _M_IX86 
	pPeb = GetPEBx86();
#elif defined(_M_AMD64)
	pPeb = (PPEB)__readgsqword(0x60);
#elif defined(_M_ARM)
	PTEB pTeb = (PTEB)_MoveFromCoprocessor(15, 0, 13, 0, 2); /* CP15_TPIDRURW */
	if (pTeb)
		pPeb = (PPEB)pTeb->ProcessEnvironmentBlock;
#endif

	if (pPeb == NULL)
		return NULL;

	pLdrDataTableEntry = (PLDR_DATA_TABLE_ENTRY)pPeb->Ldr->InMemoryOrderModuleList.Flink;
	pListEntry = pPeb->Ldr->InMemoryOrderModuleList.Flink;

	do
	{
		if (lstrcmpiW(pLdrDataTableEntry->FullDllName.Buffer, moduleName) == 0)
			return (HMODULE)pLdrDataTableEntry->Reserved2[0];

		pListEntry = pListEntry->Flink;
		pLdrDataTableEntry = (PLDR_DATA_TABLE_ENTRY)(pListEntry->Flink);

	} while (pListEntry != pPeb->Ldr->InMemoryOrderModuleList.Flink);

	return NULL;
}

FARPROC WINAPI GetExportAddress(HMODULE hMod, const char *lpProcName)
{
	char *pBaseAddress = (char *)hMod;

	IMAGE_DOS_HEADER *pDosHeader = (IMAGE_DOS_HEADER *)pBaseAddress;
	IMAGE_NT_HEADERS *pNtHeaders = (IMAGE_NT_HEADERS *)(pBaseAddress + pDosHeader->e_lfanew);
	IMAGE_OPTIONAL_HEADER *pOptionalHeader = &pNtHeaders->OptionalHeader;
	IMAGE_DATA_DIRECTORY *pDataDirectory = (IMAGE_DATA_DIRECTORY *)(&pOptionalHeader->DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT]);
	IMAGE_EXPORT_DIRECTORY *pExportDirectory = (IMAGE_EXPORT_DIRECTORY *)(pBaseAddress + pDataDirectory->VirtualAddress);

	void **ppFunctions = (void **)(pBaseAddress + pExportDirectory->AddressOfFunctions);
	WORD *pOrdinals = (WORD *)(pBaseAddress + pExportDirectory->AddressOfNameOrdinals);
	ULONG *pNames = (ULONG *)(pBaseAddress + pExportDirectory->AddressOfNames);
	/* char **pNames = (char **)(pBaseAddress + pExportDirectory->AddressOfNames); /* */

	void *pAddress = NULL;

	typedef HMODULE(WINAPI *LoadLibraryAF)(LPCSTR lpFileName);
	LoadLibraryAF pLoadLibraryA = NULL;

	DWORD i;

	/*  Check if the function name is an ordinal  */
	if (((DWORD_PTR)lpProcName >> 16) == 0)
	{
		WORD ordinal = LOWORD(lpProcName);
		DWORD dwOrdinalBase = pExportDirectory->Base;

		if (ordinal < dwOrdinalBase || ordinal >= dwOrdinalBase + pExportDirectory->NumberOfFunctions)
			return NULL;

		pAddress = (FARPROC)(pBaseAddress + (DWORD_PTR)ppFunctions[ordinal - dwOrdinalBase]);
	}
	/*  Function is exported by name, iterate through each function name and compare to the requested function  */
	else
	{
		for (i = 0; i < pExportDirectory->NumberOfNames; i++)
		{
			char *szName = (char*)pBaseAddress + (DWORD_PTR)pNames[i];
			if (strcmp(lpProcName, szName) == 0)
			{
				pAddress = (FARPROC)(pBaseAddress + ((ULONG*)(pBaseAddress + pExportDirectory->AddressOfFunctions))[pOrdinals[i]]);
				break;
			}
		}
	}
	/*  If the acquired address is within the range of addresses that make up the export directory, then it is a pointer to
	 *  a string containing the DLL name that actually exports the function. GetExportAddress() is recursively called using
	 *  the pointer to the DLL name string  */
	if ((char *)pAddress >= (char *)pExportDirectory && (char *)pAddress < (char *)pExportDirectory + pDataDirectory->Size)
	{
		char *szDllName, *szFunctionName;
		HMODULE hForward;
		
		/*  Copy the DLL name  */
		szDllName = _strdup((const char *)pAddress);
		if (!szDllName)
			return NULL;

		pAddress = NULL;
		szFunctionName = strchr(szDllName, '.');
		*szFunctionName++ = 0;

		/* Get a function pointer to LoadLibraryA in order to load the export DLL  */
		pLoadLibraryA = (LoadLibraryAF)GetExportAddress(GetModuleBaseAddress(L"KERNEL32.DLL"), "LoadLibraryA");

		if (pLoadLibraryA == NULL)
			return NULL;
		
		/* Get the address of the export DLL  */
		hForward = pLoadLibraryA(szDllName);
		free(szDllName);

		if (!hForward)
			return NULL;
		
		/*  Resolve the address of the exported function  */
		pAddress = GetExportAddress(hForward, szFunctionName);
	}

	return pAddress;
}

int main()
{
	typedef HMODULE(WINAPI *LoadLibraryAF)(LPCSTR lpFileName);
	typedef FARPROC(WINAPI *GetProcAddressF)(HMODULE hModule, LPCSTR lpProcName);
	HMODULE hKernel32 = GetModuleBaseAddress(L"KERNEL32.DLL");
	LoadLibraryAF pLoadLibraryA = (LoadLibraryAF)GetExportAddress(hKernel32, "LoadLibraryA");
	GetProcAddressF pGetProcAddress = (GetProcAddressF)GetExportAddress(hKernel32, "GetProcAddress");

	typedef HMODULE(WINAPI *GetModuleHandleWF)(LPCWSTR lpModuleName);
	HMODULE hUser32 = pLoadLibraryA("user32.dll");
	FARPROC pMessageBox = pGetProcAddress(hUser32, "MessageBoxW");

	pMessageBox(NULL, L"It works!", L"Hello World!", MB_OK);

	return 0;
}
