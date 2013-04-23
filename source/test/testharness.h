/*****************************************************************************
 * Copyright (C) 2013 x265 project
 *
 * Authors: Steve Borho <steve@borho.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111, USA.
 *
 * This program is also available under a commercial proprietary license.
 * For more information, contact us at licensing@multicorewareinc.com.
 *****************************************************************************/

#ifndef _TESTHARNESS_H_
#define _TESTHARNESS_H_ 1

#include "primitives.h"
#include <stdint.h>

#if HIGH_BIT_DEPTH
#define BIT_DEPTH 10
#else
#define BIT_DEPTH 8
#endif
#define PIXEL_MAX ((1 << BIT_DEPTH) - 1)

class TestHarness
{
public:

    TestHarness() {}

    virtual ~TestHarness() {}

    virtual bool testCorrectness(const x265::EncoderPrimitives& ref, const x265::EncoderPrimitives& opt) = 0;

    virtual void measureSpeed(const x265::EncoderPrimitives& ref, const x265::EncoderPrimitives& opt) = 0;
};

class Timer
{
public:

    Timer() {}

    virtual ~Timer() {}

    static Timer *CreateTimer();

    virtual void Start() = 0;

    virtual void Stop() = 0;

    virtual uint64_t Elapsed() = 0;

    virtual void Release() = 0;
};

#define REPORT_SPEEDUP(ITERATIONS, RUNOPT, RUNREF) \
{ \
    t->Start(); for (int X=0; X < ITERATIONS; X++) { RUNOPT; } t->Stop(); \
    uint64_t optelapsed = t->Elapsed(); \
    t->Start(); for (int X=0; X < ITERATIONS; X++) { RUNREF; } t->Stop(); \
    uint64_t refelapsed = t->Elapsed(); \
    printf("\t%3.2fx\n", (double)refelapsed/optelapsed); \
}

#endif // ifndef _TESTHARNESS_H_
