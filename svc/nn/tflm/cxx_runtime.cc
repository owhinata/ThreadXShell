/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cxx_runtime.cc
 * @brief   The whole C++ runtime the tflm backend brings (owhinata/wio-lite-ai#9 phase
 * 2c, owhinata/stm32f746g-disco#86; one file for wio-lite-ai and f746g-disco since
 * issue #161).
 *
 * Compiled only into the `tflm` static library, so a `null` build never sees a line of
 * it.  Its job is to keep the C++ runtime surface of the image down to the few
 * functions on this page.  It owns no storage.
 *
 * [!] WHY OUR OWN operator new EXISTS, WHEN NOTHING SHOULD CALL IT.  With
 * TF_LITE_STATIC_MEMORY the interpreter serves every allocation from the arena with a
 * bump allocator and never reaches the heap.  But flatbuffers and the odd STL fragment
 * still REFERENCE operator new, and a reference is all the linker needs.  Left
 * undefined, it would be resolved from libstdc++_nano -- a prebuilt archive, compiled
 * WITH exceptions no matter what flags we use.  Its operator new throws
 * std::bad_alloc, so extracting it drags in __cxa_throw, the _Unwind_* personality
 * machinery and bad_alloc's type-info: tens of KB of code that -fno-exceptions
 * guarantees can never execute.  Defining our own here means the linker resolves the
 * symbol from this object and never opens that archive member.  (wio-lite-ai's
 * cmake/check_cxx_runtime.py verifies the outcome after every link.)
 *
 * The same argument covers the nothrow forms below: libstdc++'s
 * `operator new(size_t, nothrow_t)` is implemented as a try/catch around the throwing
 * one, so referencing IT pulls the unwinder just as surely.
 *
 * Not covered, deliberately: the C++17 over-aligned forms
 * (`operator new(size_t, align_val_t)`).  Nothing in TFLM new's an over-aligned type
 * today, and if that ever changes the link resolves it from libstdc++_nano -- which
 * check_cxx_runtime.py reports with the exception symbols named.
 *
 * The other three pieces of the runtime are removed by flags rather than replaced:
 * __cxa_guard_* by -fno-threadsafe-statics, __cxa_atexit/__dso_handle by
 * -fno-use-cxa-atexit (nothing here ever returns from main, so a destructor registry
 * would be RAM spent on code that cannot run), and the unwind tables by
 * -fno-unwind-tables.
 */
#include <cstddef>
#include <cstdlib>
#include <new>

/*
 * Routed to malloc rather than returning nullptr: with TF_LITE_STATIC_MEMORY nothing
 * should reach these at all, and if something does, the useful outcome is that it
 * works.  (The call counters that used to sit here went with issue #161: nothing ever
 * printed them.)
 *
 * These deliberately do NOT run the new_handler loop the standard describes.  There is
 * no handler to install here, and the loop would only turn an out-of-memory into a
 * hang.  malloc failing means the board's heap is exhausted, which `free` reports and
 * which no retry will fix.
 *
 * [!] THE THROWING FORMS MUST NOT RETURN NULL, so they spin instead.  The standard says
 * the non-nothrow operator new either returns storage or throws ([basic.stc.dynamic]);
 * with -fno-exceptions it cannot throw, and returning null would hand a null pointer to
 * a caller that -- correctly, by the contract -- never checks it.  The fault would then
 * land at some later dereference inside flatbuffers or a kernel, with nothing left to
 * say the heap was the cause.  Spinning keeps the program counter AT the allocation, so
 * SWD shows the answer immediately, and a watchdog turns it into a recorded reset
 * rather than a silent wedge.  Neither outcome is good; this one is diagnosable.
 *
 * The nothrow forms DO return null, because there the contract says they may and the
 * caller is required to check.
 */
static void *nn_tflm_alloc_or_spin(std::size_t n)
{
	void *p = malloc(n);

	if (!p) {
		/* Out of heap inside the backend -- see above. */
		for (;;) { }
	}
	return p;
}

void *operator new(std::size_t n)
{
	return nn_tflm_alloc_or_spin(n);
}

void *operator new[](std::size_t n)
{
	return nn_tflm_alloc_or_spin(n);
}

void *operator new(std::size_t n, const std::nothrow_t &) noexcept
{
	return malloc(n);
}

void *operator new[](std::size_t n, const std::nothrow_t &) noexcept
{
	return malloc(n);
}

void operator delete(void *p) noexcept                       { free(p); }
void operator delete[](void *p) noexcept                     { free(p); }
void operator delete(void *p, std::size_t) noexcept          { free(p); }   /* C++14 sized */
void operator delete[](void *p, std::size_t) noexcept        { free(p); }
void operator delete(void *p, const std::nothrow_t &) noexcept   { free(p); }
void operator delete[](void *p, const std::nothrow_t &) noexcept { free(p); }

/*
 * Reached only if a pure virtual is called during construction or destruction of an
 * abstract base -- i.e. never, in code that works.  It is defined here so the link
 * never falls back to libsupc++ for it, and it spins rather than calling abort()
 * because abort() would pull newlib's exit path (and, through -specs=nosys, a _kill
 * that cannot work).
 */
extern "C" void __cxa_pure_virtual(void)
{
	for (;;) { }
}
