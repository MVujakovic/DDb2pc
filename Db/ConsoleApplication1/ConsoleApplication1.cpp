// ConsoleApplication1.cpp : Defines the entry point for the console application.


#include "stdafx.h"
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>
#include "StaticLibrarySample.h"



int main()
{

	_tprintf(_T("%d"), Test(9, 1));
	cleanup();

	

	getchar();
    return 0;
}

