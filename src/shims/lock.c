/*
 * Copyright (c) 2016 Apple Inc. All rights reserved.
 *
 * @APPLE_APACHE_LICENSE_HEADER_START@
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * @APPLE_APACHE_LICENSE_HEADER_END@
 */

#include "internal.h"

#define _dlock_syscall_switch(err, syscall, ...) \
	for (;;) { \
		int err; \
		switch ((err = ((syscall) < 0 ? errno : 0))) { \
		case EINTR: continue; \
		__VA_ARGS__ \
		} \
		break; \
	}

#if TARGET_OS_MAC
_Static_assert(DLOCK_LOCK_DATA_CONTENTION == ULF_WAIT_WORKQ_DATA_CONTENTION,
		"values should be the same");

#if !HAVE_UL_UNFAIR_LOCK
DISPATCH_ALWAYS_INLINE
static inline void
_dispatch_thread_switch(dispatch_lock value, dispatch_lock_options_t flags,
		uint32_t timeout)
{
	int option;
	if (flags & DLOCK_LOCK_DATA_CONTENTION) {
		option = SWITCH_OPTION_OSLOCK_DEPRESS;
	} else {
		option = SWITCH_OPTION_DEPRESS;
	}
	thread_switch(_dispatch_lock_owner(value), option, timeout);
}
#endif // HAVE_UL_UNFAIR_LOCK
#endif

#pragma mark - semaphores

#if USE_MACH_SEM
#if __has_include(<os/semaphore_private.h>)
#include <os/semaphore_private.h>
#define DISPATCH_USE_OS_SEMAPHORE_CACHE 1
#else
#define DISPATCH_USE_OS_SEMAPHORE_CACHE 0
#endif

#define DISPATCH_SEMAPHORE_VERIFY_KR(x) do { \
		DISPATCH_VERIFY_MIG(x); \
		if (unlikely((x) == KERN_INVALID_NAME)) { \
			DISPATCH_CLIENT_CRASH((x), \
				"Use-after-free of dispatch_semaphore_t or dispatch_group_t"); \
		} else if (unlikely(x)) { \
			DISPATCH_INTERNAL_CRASH((x), "mach semaphore API failure"); \
		} \
	} while (0)

void
_dispatch_sema4_create_slow(_dispatch_sema4_t *s4, int policy)
{
	semaphore_t tmp = MACH_PORT_NULL;

	_dispatch_fork_becomes_unsafe();

	// lazily allocate the semaphore port

	// Someday:
	// 1) Switch to a doubly-linked FIFO in user-space.
	// 2) User-space timers for the timeout.

#if DISPATCH_USE_OS_SEMAPHORE_CACHE
	if (policy == _DSEMA4_POLICY_FIFO) {
		tmp = (_dispatch_sema4_t)os_get_cached_semaphore();
		if (!os_atomic_cmpxchg(s4, MACH_PORT_NULL, tmp, relaxed)) {
			os_put_cached_semaphore((os_semaphore_t)tmp);
		}
		return;
	}
#endif

	kern_return_t kr = semaphore_create(mach_task_self(), &tmp, policy, 0);
	DISPATCH_SEMAPHORE_VERIFY_KR(kr);

	if (!os_atomic_cmpxchg(s4, MACH_PORT_NULL, tmp, relaxed)) {
		kr = semaphore_destroy(mach_task_self(), tmp);
		DISPATCH_SEMAPHORE_VERIFY_KR(kr);
	}
}

void
_dispatch_sema4_dispose_slow(_dispatch_sema4_t *sema, int policy)
{
	semaphore_t sema_port = *sema;
	*sema = MACH_PORT_DEAD;
#if DISPATCH_USE_OS_SEMAPHORE_CACHE
	if (policy == _DSEMA4_POLICY_FIFO) {
		return os_put_cached_semaphore((os_semaphore_t)sema_port);
	}
#endif
	kern_return_t kr = semaphore_destroy(mach_task_self(), sema_port);
	DISPATCH_SEMAPHORE_VERIFY_KR(kr);
}

void
_dispatch_sema4_signal(_dispatch_sema4_t *sema, long count)
{
	do {
		kern_return_t kr = semaphore_signal(*sema);
		DISPATCH_SEMAPHORE_VERIFY_KR(kr);
	} while (--count);
}

void
_dispatch_sema4_wait(_dispatch_sema4_t *sema)
{
	kern_return_t kr;
	do {
		kr = semaphore_wait(*sema);
	} while (kr == KERN_ABORTED);
	DISPATCH_SEMAPHORE_VERIFY_KR(kr);
}

bool
_dispatch_sema4_timedwait(_dispatch_sema4_t *sema, dispatch_time_t timeout)
{
	mach_timespec_t _timeout;
	kern_return_t kr;

	do {
		uint64_t nsec = _dispatch_timeout(timeout);
		_timeout.tv_sec = (typeof(_timeout.tv_sec))(nsec / NSEC_PER_SEC);
		_timeout.tv_nsec = (typeof(_timeout.tv_nsec))(nsec % NSEC_PER_SEC);
		kr = slowpath(semaphore_timedwait(*sema, _timeout));
	} while (kr == KERN_ABORTED);

	if (kr == KERN_OPERATION_TIMED_OUT) {
		return true;
	}
	DISPATCH_SEMAPHORE_VERIFY_KR(kr);
	return false;
}
#elif USE_POSIX_SEM
#define DISPATCH_SEMAPHORE_VERIFY_RET(x) do { \
		if (unlikely((x) == -1)) { \
			DISPATCH_INTERNAL_CRASH(errno, "POSIX semaphore API failure"); \
		} \
	} while (0)

void
_dispatch_sema4_init(_dispatch_sema4_t *sema, int policy DISPATCH_UNUSED)
{
	int rc = sem_init(sema, 0, 0);
	DISPATCH_SEMAPHORE_VERIFY_RET(rc);
}

void
_dispatch_sema4_dispose_slow(_dispatch_sema4_t *sema, int policy DISPATCH_UNUSED)
{
	int rc = sem_destroy(sema);
	DISPATCH_SEMAPHORE_VERIFY_RET(rc);
}

void
_dispatch_sema4_signal(_dispatch_sema4_t *sema, long count)
{
	do {
		int ret = sem_post(sema);
		DISPATCH_SEMAPHORE_VERIFY_RET(ret);
	} while (--count);
}

void
_dispatch_sema4_wait(_dispatch_sema4_t *sema)
{
	int ret = sem_wait(sema);
	DISPATCH_SEMAPHORE_VERIFY_RET(ret);
}

bool
_dispatch_sema4_timedwait(_dispatch_sema4_t *sema, dispatch_time_t timeout)
{
	struct timespec _timeout;
	int ret;

	do {
		uint64_t nsec = _dispatch_time_nanoseconds_since_epoch(timeout);
		_timeout.tv_sec = (typeof(_timeout.tv_sec))(nsec / NSEC_PER_SEC);
		_timeout.tv_nsec = (typeof(_timeout.tv_nsec))(nsec % NSEC_PER_SEC);
		ret = slowpath(sem_timedwait(sema, &_timeout));
	} while (ret == -1 && errno == EINTR);

	if (ret == -1 && errno == ETIMEDOUT) {
		return true;
	}
	DISPATCH_SEMAPHORE_VERIFY_RET(ret);
	return false;
}
#elif USE_WIN32_SEM
// rdar://problem/8428132
static DWORD best_resolution = 1; // 1ms

static DWORD
_push_timer_resolution(DWORD ms)
{
	MMRESULT res;
	static dispatch_once_t once;

	if (ms > 16) {
		// only update timer resolution if smaller than default 15.6ms
		// zero means not updated
		return 0;
	}

	// aim for the best resolution we can accomplish
	dispatch_once(&once, ^{
		TIMECAPS tc;
		MMRESULT res;
		res = timeGetDevCaps(&tc, sizeof(tc));
		if (res == MMSYSERR_NOERROR) {
			best_resolution = min(max(tc.wPeriodMin, best_resolution),
					tc.wPeriodMax);
		}
	});

	res = timeBeginPeriod(best_resolution);
	if (res == TIMERR_NOERROR) {
		return best_resolution;
	}
	// zero means not updated
	return 0;
}

// match ms parameter to result from _push_timer_resolution
DISPATCH_ALWAYS_INLINE
static inline void
_pop_timer_resolution(DWORD ms)
{
	if (ms) timeEndPeriod(ms);
}

void
_dispatch_sema4_create_slow(_dispatch_sema4_t *s4, int policy DISPATCH_UNUSED)
{
	HANDLE tmp;

	// lazily allocate the semaphore port

	while (!dispatch_assume(tmp = CreateSemaphore(NULL, 0, LONG_MAX, NULL))) {
		_dispatch_temporary_resource_shortage();
	}

	if (!os_atomic_cmpxchg(s4, 0, tmp, relaxed)) {
		CloseHandle(tmp);
	}
}

void
_dispatch_sema4_dispose_slow(_dispatch_sema4_t *sema, int policy DISPATCH_UNUSED)
{
	HANDLE sema_handle = *sema;
	CloseHandle(sema_handle);
	*sema = 0;
}

void
_dispatch_sema4_signal(_dispatch_sema4_t *sema, long count)
{
	int ret = ReleaseSemaphore(*sema, count, NULL);
	dispatch_assume(ret);
}

void
_dispatch_sema4_wait(_dispatch_sema4_t *sema)
{
	WaitForSingleObject(*sema, INFINITE);
}

bool
_dispatch_sema4_timedwait(_dispatch_sema4_t *sema, dispatch_time_t timeout)
{
	uint64_t nsec;
	DWORD msec;
	DWORD resolution;
	DWORD wait_result;

	nsec = _dispatch_timeout(timeout);
	msec = (DWORD)(nsec / (uint64_t)1000000);
	resolution = _push_timer_resolution(msec);
	wait_result = WaitForSingleObject(dsema->dsema_handle, msec);
	_pop_timer_resolution(resolution);
	return wait_result == WAIT_TIMEOUT;
}
#else
#error "port has to implement _dispatch_sema4_t"
#endif

#pragma mark - ulock wrappers
#if HAVE_UL_COMPARE_AND_WAIT

static int
_dispatch_ulock_wait(uint32_t *uaddr, uint32_t val, uint32_t timeout,
		uint32_t flags)
{
	int rc;
	_dlock_syscall_switch(err,
		rc = __ulock_wait(UL_COMPARE_AND_WAIT | flags, uaddr, val, timeout),
		case 0: return rc > 0 ? ENOTEMPTY : 0;
		case ETIMEDOUT: case EFAULT: return err;
		case EOWNERDEAD: DISPATCH_CLIENT_CRASH(*uaddr,
				"corruption of lock owner");
		default: DISPATCH_INTERNAL_CRASH(err, "ulock_wait() failed");
	);
}

static void
_dispatch_ulock_wake(uint32_t *uaddr, uint32_t flags)
{
	_dlock_syscall_switch(err,
		__ulock_wake(UL_COMPARE_AND_WAIT | flags, uaddr, 0),
		case 0: case ENOENT: break;
		default: DISPATCH_INTERNAL_CRASH(err, "ulock_wake() failed");
	);
}

#endif
#if HAVE_UL_UNFAIR_LOCK

// returns 0, ETIMEDOUT, ENOTEMPTY, EFAULT
static int
_dispatch_unfair_lock_wait(uint32_t *uaddr, uint32_t val, uint32_t timeout,
		dispatch_lock_options_t flags)
{
	int rc;
	_dlock_syscall_switch(err,
		rc = __ulock_wait(UL_UNFAIR_LOCK | flags, uaddr, val, timeout),
		case 0: return rc > 0 ? ENOTEMPTY : 0;
		case ETIMEDOUT: case EFAULT: return err;
		case EOWNERDEAD: DISPATCH_CLIENT_CRASH(*uaddr,
				"corruption of lock owner");
		default: DISPATCH_INTERNAL_CRASH(err, "ulock_wait() failed");
	);
}

static void
_dispatch_unfair_lock_wake(uint32_t *uaddr, uint32_t flags)
{
	_dlock_syscall_switch(err, __ulock_wake(UL_UNFAIR_LOCK | flags, uaddr, 0),
		case 0: case ENOENT: break;
		default: DISPATCH_INTERNAL_CRASH(err, "ulock_wake() failed");
	);
}

#endif
#pragma mark - futex wrappers
#if HAVE_FUTEX
#include <sys/time.h>
#ifdef __ANDROID__
#include <sys/syscall.h>
#else
#include <syscall.h>
#endif /* __ANDROID__ */

DISPATCH_ALWAYS_INLINE
static inline int
_dispatch_futex(uint32_t *uaddr, int op, uint32_t val,
		const struct timespec *timeout, uint32_t *uaddr2, uint32_t val3,
		int opflags)
{
	return syscall(SYS_futex, uaddr, op | opflags, val, timeout, uaddr2, val3);
}

static int
_dispatch_futex_wait(uint32_t *uaddr, uint32_t val,
		const struct timespec *timeout, int opflags)
{
	_dlock_syscall_switch(err,
		_dispatch_futex(uaddr, FUTEX_WAIT, val, timeout, NULL, 0, opflags),
		case 0: case EWOULDBLOCK: case ETIMEDOUT: return err;
		default: DISPATCH_CLIENT_CRASH(err, "futex_wait() failed");
	);
}

static void
_dispatch_futex_wake(uint32_t *uaddr, int wake, int opflags)
{
	int rc;
	_dlock_syscall_switch(err,
		rc = _dispatch_futex(uaddr, FUTEX_WAKE, wake, NULL, NULL, 0, opflags),
		case 0: return;
		default: DISPATCH_CLIENT_CRASH(err, "futex_wake() failed");
	);
}

static void
_dispatch_futex_lock_pi(uint32_t *uaddr, struct timespec *timeout, int detect,
	      int opflags)
{
	_dlock_syscall_switch(err,
		_dispatch_futex(uaddr, FUTEX_LOCK_PI, detect, timeout,
				NULL, 0, opflags),
		case 0: return;
		default: DISPATCH_CLIENT_CRASH(errno, "futex_lock_pi() failed");
	);
}

static void
_dispatch_futex_unlock_pi(uint32_t *uaddr, int opflags)
{
	_dlock_syscall_switch(err,
		_dispatch_futex(uaddr, FUTEX_UNLOCK_PI, 0, NULL, NULL, 0, opflags),
		case 0: return;
		default: DISPATCH_CLIENT_CRASH(errno, "futex_unlock_pi() failed");
	);
}

#endif
#pragma mark - wait for address

void
_dispatch_wait_on_address(uint32_t volatile *address, uint32_t value,
		dispatch_lock_options_t flags)
{
#if HAVE_UL_COMPARE_AND_WAIT
	_dispatch_ulock_wait((uint32_t *)address, value, 0, flags);
#elif HAVE_FUTEX
	_dispatch_futex_wait((uint32_t *)address, value, NULL, FUTEX_PRIVATE_FLAG);
#else
	mach_msg_timeout_t timeout = 1;
	while (os_atomic_load(address, relaxed) == value) {
		thread_switch(MACH_PORT_NULL, SWITCH_OPTION_WAIT, timeout++);
	}
#endif
	(void)flags;
}

void
_dispatch_wake_by_address(uint32_t volatile *address)
{
#if HAVE_UL_COMPARE_AND_WAIT
	_dispatch_ulock_wake((uint32_t *)address, ULF_WAKE_ALL);
#elif HAVE_FUTEX
	_dispatch_futex_wake((uint32_t *)address, INT_MAX, FUTEX_PRIVATE_FLAG);
#else
	(void)address;
#endif
}

#pragma mark - thread event

void
_dispatch_thread_event_signal_slow(dispatch_thread_event_t dte)
{
#if HAVE_UL_COMPARE_AND_WAIT
	_dispatch_ulock_wake(&dte->dte_value, 0);
#elif HAVE_FUTEX
	_dispatch_futex_wake(&dte->dte_value, 1, FUTEX_PRIVATE_FLAG);
#else
	_dispatch_sema4_signal(&dte->dte_sema, 1);
#endif
}

void
_dispatch_thread_event_wait_slow(dispatch_thread_event_t dte)
{
#if HAVE_UL_COMPARE_AND_WAIT || HAVE_FUTEX
	for (;;) {
		uint32_t value = os_atomic_load(&dte->dte_value, acquire);
		if (likely(value == 0)) return;
		if (unlikely(value != UINT32_MAX)) {
			DISPATCH_CLIENT_CRASH(value, "Corrupt thread event value");
		}
#if HAVE_UL_COMPARE_AND_WAIT
		int rc = _dispatch_ulock_wait(&dte->dte_value, UINT32_MAX, 0, 0);
		dispatch_assert(rc == 0 || rc == EFAULT);
#elif HAVE_FUTEX
		_dispatch_futex_wait(&dte->dte_value, UINT32_MAX,
				NULL, FUTEX_PRIVATE_FLAG);
#endif
	}
#else
	_dispatch_sema4_wait(&dte->dte_sema);
#endif
}

#pragma mark - unfair lock

#if HAVE_UL_UNFAIR_LOCK
void
_dispatch_unfair_lock_lock_slow(dispatch_unfair_lock_t dul,
		dispatch_lock_options_t flags)
{
	dispatch_lock value_self = _dispatch_lock_value_for_self();
	dispatch_lock old_value, new_value, next = value_self;
	int rc;

	for (;;) {
		os_atomic_rmw_loop(&dul->dul_lock, old_value, new_value, acquire, {
			if (likely(!_dispatch_lock_is_locked(old_value))) {
				new_value = next;
			} else {
				new_value = old_value | DLOCK_WAITERS_BIT;
				if (new_value == old_value) os_atomic_rmw_loop_give_up(break);
			}
		});
		if (unlikely(_dispatch_lock_is_locked_by(old_value, value_self))) {
			DISPATCH_CLIENT_CRASH(0, "trying to lock recursively");
		}
		if (new_value == next) {
			return;
		}
		rc = _dispatch_unfair_lock_wait(&dul->dul_lock, new_value, 0, flags);
		if (rc == ENOTEMPTY) {
			next = value_self | DLOCK_WAITERS_BIT;
		} else {
			next = value_self;
		}
	}
}
#elif HAVE_FUTEX
void
_dispatch_unfair_lock_lock_slow(dispatch_unfair_lock_t dul,
		dispatch_lock_options_t flags)
{
	(void)flags;
	_dispatch_futex_lock_pi(&dul->dul_lock, NULL, 1, FUTEX_PRIVATE_FLAG);
}
#else
void
_dispatch_unfair_lock_lock_slow(dispatch_unfair_lock_t dul,
		dispatch_lock_options_t flags)
{
	dispatch_lock cur, value_self = _dispatch_lock_value_for_self();
	uint32_t timeout = 1;

	while (unlikely(!os_atomic_cmpxchgv(&dul->dul_lock,
			DLOCK_OWNER_NULL, value_self, &cur, acquire))) {
		if (unlikely(_dispatch_lock_is_locked_by(cur, self))) {
			DISPATCH_CLIENT_CRASH(0, "trying to lock recursively");
		}
		_dispatch_thread_switch(cur, flags, timeout++);
	}
}
#endif

void
_dispatch_unfair_lock_unlock_slow(dispatch_unfair_lock_t dul, dispatch_lock cur)
{
	if (unlikely(!_dispatch_lock_is_locked_by_self(cur))) {
		DISPATCH_CLIENT_CRASH(cur, "lock not owned by current thread");
	}

#if HAVE_UL_UNFAIR_LOCK
	if (_dispatch_lock_has_waiters(cur)) {
		_dispatch_unfair_lock_wake(&dul->dul_lock, 0);
	}
#elif HAVE_FUTEX
	// futex_unlock_pi() handles both OWNER_DIED which we abuse & WAITERS
	_dispatch_futex_unlock_pi(&dul->dul_lock, FUTEX_PRIVATE_FLAG);
#else
	(void)dul;
#endif
}

#pragma mark - gate lock

void
_dispatch_gate_wait_slow(dispatch_gate_t dgl, dispatch_lock value,
		dispatch_lock_options_t flags)
{
	dispatch_lock self = _dispatch_lock_value_for_self();
	dispatch_lock old_value, new_value;
	uint32_t timeout = 1;

	for (;;) {
		os_atomic_rmw_loop(&dgl->dgl_lock, old_value, new_value, acquire, {
			if (likely(old_value == value)) {
				os_atomic_rmw_loop_give_up_with_fence(acquire, return);
			}
			new_value = old_value | DLOCK_WAITERS_BIT;
			if (new_value == old_value) os_atomic_rmw_loop_give_up(break);
		});
		if (unlikely(_dispatch_lock_is_locked_by(old_value, self))) {
			DISPATCH_CLIENT_CRASH(0, "trying to lock recursively");
		}
#if HAVE_UL_UNFAIR_LOCK
		_dispatch_unfair_lock_wait(&dgl->dgl_lock, new_value, 0, flags);
#elif HAVE_FUTEX
		_dispatch_futex_wait(&dgl->dgl_lock, tid_new, NULL, FUTEX_PRIVATE_FLAG);
#else
		_dispatch_thread_switch(tid_new, flags, timeout++);
#endif
		(void)timeout;
	}
}

void
_dispatch_gate_broadcast_slow(dispatch_gate_t dgl, dispatch_lock cur)
{
	if (unlikely(!_dispatch_lock_is_locked_by_self(cur))) {
		DISPATCH_CLIENT_CRASH(cur, "lock not owned by current thread");
	}

#if HAVE_UL_UNFAIR_LOCK
	_dispatch_unfair_lock_wake(&dgl->dgl_lock, ULF_WAKE_ALL);
#elif HAVE_FUTEX
	_dispatch_futex_wake(&dgl->dgl_lock, INT_MAX, FUTEX_PRIVATE_FLAG);
#else
	(void)dgl;
#endif
}
