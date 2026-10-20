/* Copyright (c) 2026, PostgreSQL Global Development Group */
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Private test helper: identify exactly one process, wait, or probe a port. */
int
main(int argc, char **argv)
{
	if (argc == 3 && strcmp(argv[1], "--bind") == 0)
	{
		WSADATA data;
		SOCKET s;
		struct sockaddr_in address = {0};
		BOOL exclusive = TRUE;
		int result;
		if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
			return 2;
		s = socket(AF_INET, SOCK_STREAM, 0);
		if (s == INVALID_SOCKET || setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
								(const char *) &exclusive, sizeof(exclusive)) != 0)
			return 2;
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		address.sin_port = htons((unsigned short) atoi(argv[2]));
		result = bind(s, (struct sockaddr *) &address, sizeof(address));
		closesocket(s);
		WSACleanup();
		return result == 0 ? 0 : 1;
	}
	if (argc == 5 && (strcmp(argv[1], "--alive") == 0 ||
					 strcmp(argv[1], "--wait") == 0))
	{
		HANDLE process;
		FILETIME created, exited, kernel, user;
		DWORD result;
		BOOL wait = strcmp(argv[1], "--wait") == 0;
		process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
							 FALSE, strtoul(argv[2], NULL, 10));
		if (process == NULL)
			return GetLastError() == ERROR_INVALID_PARAMETER && wait ? 0 : 2;
		if (!GetProcessTimes(process, &created, &exited, &kernel, &user))
		{
			CloseHandle(process);
			return 2;
		}
		/* A reused PID must never count as the owned helper. */
		if (created.dwHighDateTime != strtoul(argv[3], NULL, 10) ||
			created.dwLowDateTime != strtoul(argv[4], NULL, 10))
		{
			CloseHandle(process);
			return wait ? 0 : 1;
		}
		result = WaitForSingleObject(process, wait ? 10000 : 0);
		CloseHandle(process);
		return result == (wait ? WAIT_OBJECT_0 : WAIT_TIMEOUT) ? 0 : 1;
	}
	if (argc == 3)
	{
		FILE *ready;
		FILETIME created, exited, kernel, user;
		DWORD deadline = GetTickCount() + 35000;
		if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
			return 2;
		ready = fopen(argv[1], "w");
		if (ready == NULL)
			return 2;
		fprintf(ready, "%lu %lu %lu\n", GetCurrentProcessId(),
				created.dwHighDateTime, created.dwLowDateTime);
		fclose(ready);
		while (GetFileAttributes(argv[2]) == INVALID_FILE_ATTRIBUTES &&
			   (LONG) (deadline - GetTickCount()) > 0)
			Sleep(50);
		return 0;
	}
	return 2;
}
