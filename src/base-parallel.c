/*
 * base-parallel - A base console application for multithreaded parallel processing
 *
 * Copyright © 2020 Pete Batard <pete@akeo.ie>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef _DEBUG
#define _CRTDBG_MAP_ALLOC
#include <stdlib.h>
#include <crtdbg.h>
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "msapi_utf8.h"

#pragma warning(disable: 6258)		// I know what I'm using TerminateThread for

#define _STRINGIFY(x)		#x
#define STRINGIFY(x)		_STRINGIFY(x)

#ifndef APP_VERSION
#define APP_VERSION_STR		"[DEV]"
#else
#define APP_VERSION_STR		STRINGIFY(APP_VERSION)
#endif

// Maximum amount of time we allow for a thread (ms).
// Set it to INFINITE to wait forever
#define WAIT_TIME			15000
// If you don't want to use all the logical processors
// You can define a number of "spare threads" here.
#define SPARE_THREADS		0
// Number of iterations for our dummy loop
#define MAX_ITERATIONS		100

// The following is used to indicate cancellation
volatile BOOL cancel_requested = FALSE;

// Global thread variables
DWORD num_threads = 0;
DWORD_PTR* thread_affinity = NULL;
HANDLE *data_ready = NULL, *thread_ready = NULL;
uint32_t* thread_data = NULL;

static __inline char* appname(const char* path)
{
	static char appname[128];
	_splitpath_s(path, NULL, 0, NULL, 0, appname, sizeof(appname), NULL, 0);
	return appname;
}

static __inline uint8_t popcnt64(register uint64_t u)
{
	u = (u & 0x5555555555555555) + ((u >> 1) & 0x5555555555555555);
	u = (u & 0x3333333333333333) + ((u >> 2) & 0x3333333333333333);
	u = (u & 0x0f0f0f0f0f0f0f0f) + ((u >> 4) & 0x0f0f0f0f0f0f0f0f);
	u = (u & 0x00ff00ff00ff00ff) + ((u >> 8) & 0x00ff00ff00ff00ff);
	u = (u & 0x0000ffff0000ffff) + ((u >> 16) & 0x0000ffff0000ffff);
	u = (u & 0x00000000ffffffff) + ((u >> 32) & 0x00000000ffffffff);
	return (uint8_t)u;
}

/*
 * This call (loosely) detects the number of logical processors on the
 * system, sets the number of threads accordingly and then affixes each
 * thread (affinity) to a specific logical processor.
 * Note: This doesn't support more than 64 logical processors...
 */
BOOL SetThreadAffinity(void)
{
	DWORD i;
	DWORD_PTR affinity, dummy;

	num_threads = 0;
	if (!GetProcessAffinityMask(GetCurrentProcess(), &affinity, &dummy))
		return FALSE;

	num_threads = popcnt64(affinity);
	if (num_threads <= SPARE_THREADS)
		return FALSE;
	num_threads -= SPARE_THREADS;

	thread_affinity = calloc((size_t)num_threads, sizeof(DWORD_PTR));
	if (thread_affinity == NULL) {
		fprintf(stderr, "Could not alloc thread_affinity.\n");
		return FALSE;
	}

	// Spread the affinity evenly (1 thread per logical processor)
	for (i = 0; i < num_threads; i++) {
		thread_affinity[i] |= affinity & (-1LL * affinity);
		affinity ^= affinity & (-1LL * affinity);
	}
	return TRUE;
}

// Individual thread for the task that is to be executed in parallel
DWORD WINAPI ParallelTaskThread(void* param)
{
	uint32_t i = (uint32_t)(uintptr_t)param;
	do {
		// Signal that we're ready to service requests
		if (!SetEvent(thread_ready[i])) {
			printf("Failed to signal readiness for thread #%02d\n", i);
			return 1;
		}

		// Wait for requests
		if (WaitForSingleObject(data_ready[i], WAIT_TIME) != WAIT_OBJECT_0) {
			printf("Failed to get data ready event for thread #%02d\n", i);
			return 1;
		}

		// Check for exit condition
		if (thread_data[i] == 0) {
			printf("Thread #%02d exiting\n", i);
			return 0;
		}

		// Process data
		printf("Thread #%02d received data %d\n", i, thread_data[i]);

		// Dummy loop
		for (uint32_t j = 0; (j < 25) && (!cancel_requested); j++)
			Sleep(100);

	} while (1);
}

DWORD WINAPI ControlThread(void* param)
{
	DWORD r = 1;
	HANDLE* task_thread;

	if ((num_threads == 0) || (thread_affinity == NULL))
		ExitThread(r);

	task_thread = calloc(num_threads, sizeof(HANDLE));
	data_ready = calloc(num_threads, sizeof(HANDLE));
	thread_ready = calloc(num_threads, sizeof(HANDLE));
	thread_data = calloc(num_threads, sizeof(uint32_t));
	if ((task_thread == NULL) || (data_ready == NULL) || (thread_ready == NULL) || (thread_data == NULL)) {
		fprintf(stderr, "Alloc error.\n");
		goto out;
	}

	printf("Creating %d threads...\n", num_threads);

	for (uint32_t i = 0; i < num_threads; i++) {
		// NB: Can't use a single manual-reset event for data_ready as we
		// wouldn't be able to ensure the event is reset before the thread
		// gets into its next wait loop
		data_ready[i] = CreateEvent(NULL, FALSE, FALSE, NULL);
		thread_ready[i] = CreateEvent(NULL, FALSE, FALSE, NULL);
		if ((data_ready[i] == NULL) || (thread_ready[i] == NULL)) {
			printf("Unable to create checksum thread event\n");
			goto out;
		}
		task_thread[i] = CreateThread(NULL, 0, ParallelTaskThread, (LPVOID)(uintptr_t)i, 0, NULL);
		if (task_thread[i] == NULL) {
			printf("Unable to start thread #%02d\n", i);
			goto out;
		}
		SetThreadPriority(task_thread[i], THREAD_PRIORITY_ABOVE_NORMAL);
		if (thread_affinity[i] != 0)
			SetThreadAffinityMask(task_thread[i], thread_affinity[i]);
	}

	for (uint32_t iteration = 1; (iteration <= MAX_ITERATIONS) && !cancel_requested; iteration++) {
		// Wait for threads to signal they are ready to process data
		DWORD i = WaitForMultipleObjects(num_threads, thread_ready, FALSE, WAIT_TIME);
		if (i >= WAIT_OBJECT_0 + num_threads) {
			printf("Failed to wait on individual task thread\n");
			goto out;
		}
		// Populate some data
		thread_data[i] = iteration;
		// Signal the waiting threads
		if (!SetEvent(data_ready[i])) {
			printf("Could not signal thread #%02d\n", i);
			goto out;
		}
	}

	// If we just exited the call, give some time for the last thread to read the data
	Sleep(250);

	// Clear data and signal all the threads to exit
	memset(thread_data, 0, sizeof(uint32_t) * num_threads);
	for (DWORD i = 0; i < num_threads; i++)
		SetEvent(data_ready[i]);

	// Wait for thread exit
	if (WaitForMultipleObjects(num_threads, task_thread, TRUE, WAIT_TIME) == WAIT_OBJECT_0) {
		memset(task_thread, 0, sizeof(HANDLE) * num_threads);
	} else {
		printf("Threads did not finalize\n");
		goto out;
	}
	r = 0;

out:
	for (uint32_t i = 0; i < num_threads; i++) {
		if (task_thread[i] != NULL)
			TerminateThread(task_thread[i], 1);
		if (data_ready[i] != NULL)
			CloseHandle(data_ready[i]);
		if (thread_ready[i] != NULL)
			CloseHandle(thread_ready[i]);
	}
	free(data_ready);
	free(thread_ready);
	free(task_thread);
	free(thread_data);
	ExitThread(r);
}

// Important: If debugging this in Visual Studio, you will get an
// exception when pressing Ctrl-C as the debugger is set to break
// then. See https://stackoverflow.com/a/13207212/1069307.
static BOOL WINAPI ConsoleHandler(DWORD dwCtrlType)
{
	switch (dwCtrlType) {
	case CTRL_C_EVENT:
		printf("Ctrl-C received\n");
		cancel_requested = TRUE;
		return TRUE;
	default:
		return FALSE;
	}
}

int main_utf8(int argc, char** argv)
{
	int r = 1;
	HANDLE control_thread;

	fprintf(stderr, "%s %s © 2020 Pete Batard <pete@akeo.ie>\n\n", appname(argv[0]), APP_VERSION_STR);
	fprintf(stderr, "This program is free software; you can redistribute it and/or modify it under \n");
	fprintf(stderr, "the terms of the GNU General Public License as published by the Free Software \n");
	fprintf(stderr, "Foundation; either version 3 of the License or any later version.\n\n");
	fprintf(stderr, "Official project and latest downloads at: https://github.com/pbatard/base-parallel\n\n");

	if (!SetThreadAffinity()) {
		fprintf(stderr, "Could not set thread_affinity.\n");
		goto out;
	}

	control_thread = CreateThread(NULL, 0, ControlThread, NULL, 0, NULL);
	if (control_thread == NULL) {
		fprintf(stderr, "Could not create control thread.\n");
		goto out;
	}
	SetThreadPriority(control_thread, THREAD_PRIORITY_ABOVE_NORMAL);
	SetConsoleCtrlHandler((PHANDLER_ROUTINE)ConsoleHandler, TRUE);
	if (WaitForSingleObject(control_thread, INFINITE) != WAIT_OBJECT_0) {
		fprintf(stderr, "Control thread exited unexpectedly.\n");
		goto out;
	}
	r = 0;

out:
	free(thread_affinity);
	return r;
}

int wmain(int argc, wchar_t** argv16)
{
	SetConsoleOutputCP(CP_UTF8);
	char** argv = calloc(argc, sizeof(char*));
	if (argv == NULL)
		return -1;
	for (int i = 0; i < argc; i++)
		argv[i] = wchar_to_utf8(argv16[i]);
	int r = main_utf8(argc, argv);
	for (int i = 0; i < argc; i++)
		free(argv[i]);
	free(argv);
#ifdef _DEBUG
	_CrtDumpMemoryLeaks();
#endif
	return r;
}
