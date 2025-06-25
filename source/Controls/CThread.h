/****************************************************************************
 * Copyright (C) 2013 Dimok
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
 ****************************************************************************/
#ifndef CTHREAD_H_
#define CTHREAD_H_

#include <gccore.h>
#include <malloc.h>
#include <unistd.h>

class CThread
{
public:
	//! constructor
	CThread(int iPriority = 60, int iStackSize = 8192)
		: lwpThread(LWP_THREAD_NULL)
		, ThreadStack(NULL)
	{
		//! allocate the stack
		ThreadStack = (u8 *) memalign(32, iStackSize);

		if(ThreadStack)
		{
			//! create the thread
			LWP_CreateThread (&lwpThread, threadCallback, this, ThreadStack, iStackSize, iPriority);
			//! wait for the thread to reach the start hook point
			do { usleep(1000); } while(isThreadRunning());
		}
	}

	//! destructor
	virtual ~CThread() { shutdownThread(); }

	//! start thread execution
	virtual void startThread() { resumeThread(); }
	//! Get thread ID
	virtual lwp_t getThread() const { return lwpThread; }
	//! Thread entry function
	virtual void executeThread(void) = 0;
	//! Suspend thread
	virtual void suspendThread(void) { if(lwpThread != LWP_THREAD_NULL) LWP_SuspendThread(lwpThread); }
	//! Resume thread
	virtual void resumeThread(void) { if(lwpThread != LWP_THREAD_NULL) LWP_ResumeThread(lwpThread); }
	//! Set thread priority
	virtual void setThreadPriority(int prio) { if(lwpThread != LWP_THREAD_NULL) LWP_SetThreadPriority(lwpThread, prio); }
	//! Check if thread is suspended
	virtual bool isThreadRunning(void) const { if(lwpThread != LWP_THREAD_NULL) return !LWP_ThreadIsSuspended(lwpThread); else return false; }
	//! Shutdown thread
	virtual void shutdownThread(void)
	{
		//! wait for thread to finish
		if(lwpThread != LWP_THREAD_NULL)
		{
			resumeThread();
			LWP_JoinThread(lwpThread, NULL);
		}
		//! free the thread stack buffer
		if(ThreadStack)
			free(ThreadStack);

		lwpThread = LWP_THREAD_NULL;
		ThreadStack = NULL;
	}
private:
	static void *threadCallback(void *arg)
	{
		//! Initially start/hook thread point, stop here
		((CThread *) arg)->suspendThread();
		//! After call to start() continue with the internal function
		((CThread *) arg)->executeThread();
		return NULL;
	}

	lwp_t lwpThread;
	u8 *ThreadStack;
};

#endif
