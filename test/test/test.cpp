// test.cpp : 定义控制台应用程序的入口点。
//

#include "stdafx.h"
#include "CrashReport.h"

int _tmain(int argc, _TCHAR* argv[])
{
	InitCrashReport();

	*((int*)nullptr) = 6;

	return 0;
}

