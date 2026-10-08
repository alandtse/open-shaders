#include <windows.h>

#include <cstdio>
#include <cstring>
#include <cwchar>

#ifndef PAD_BYTES
#	define PAD_BYTES 0x5000000
#endif

// The host stands in for the game exe: its image is filled with `ret` so engine calls made by
// static initialisers return harmlessly, and its version resource selects the runtime.
#pragma section(".pad", read, write, execute)
__declspec(allocate(".pad")) static unsigned char pad[PAD_BYTES];

int wmain(int argc, wchar_t** argv)
{
	if (argc < 3)
		return 2;

	memset(pad, 0xC3, sizeof(pad));
	if (!SetCurrentDirectoryW(argv[2]))
		return 2;

	const HMODULE module = LoadLibraryW(argv[1]);
	const DWORD error = GetLastError();
	std::wprintf(module ? L"LOADED\n" : L"LOAD FAILED %lu\n", error);
	std::fflush(stdout);
	TerminateProcess(GetCurrentProcess(), module ? 0 : 1);
	return 1;
}
