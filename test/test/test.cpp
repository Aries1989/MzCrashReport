// test.cpp : �������̨Ӧ�ó������ڵ㡣
//

#include "stdafx.h"
#include "CrashReport.h"

int _tmain(int argc, _TCHAR* argv[])
{
	InitCrashReport();

	*((int*)nullptr) = 6;

	return 0;
}

