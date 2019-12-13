/*
 *  Copyright (C) 2016  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */
#include "boxedwine.h"

#ifdef __TEST
#include <stdlib.h>
#include <stdio.h>
#include <SDL.h>

#include "../emulation/softmmu/soft_memory.h"
#include "../emulation/hardmmu/hard_memory.h"
#include "../emulation/cpu/x64/x64CPU.h"

#include "testCPU.h"
#include "testMMX.h"
#include "testSSE.h"
#include "testSSE2.h"

static int cseip;

#define G(rm) ((rm >> 3) & 7)
#define E(rm) (rm & 7)

#if defined (BOXEDWINE_MSVC) && !defined (BOXEDWINE_64)   
__m128i floatTo128(float f1, float f2, float f3, float f4) {
    Test_Float t1, t2, t3, t4;
    t1.f = f1;
    t2.f = f2;
    t3.f = f3;
    t4.f = f4;
    return _mm_setr_epi32(t1.i, t2.i, t3.i, t4.i);
}
#endif

U32 getMilliesSinceStart() {
    return 0;
}

static KThread* thread;
static Memory* memory;
CPU* cpu;
static KProcess* process;
static int didFail;
static int totalFails;

void failed(const char* msg, ...) {
    didFail = 1;
    totalFails++;
}

void assertTrue(int b) {
    if (!b) {
        failed("Assert failed");
    }
}

void setup() {
    if (!memory) {      
        KProcess* process = new KProcess(KSystem::nextThreadId++);
        memory = new Memory();
        process->memory = memory;
        KThread* thread = new KThread(KSystem::nextThreadId++, process);
        cpu = thread->cpu;
        thread->memory = memory;
        KThread::setCurrentThread(thread);
        process->memory->allocPages((STACK_ADDRESS >> K_PAGE_SHIFT)-17, 17, PAGE_READ|PAGE_WRITE, 0, 0, 0);
        process->memory->allocPages(CODE_ADDRESS >> K_PAGE_SHIFT, 17, PAGE_READ|PAGE_WRITE|PAGE_EXEC, 0, 0, 0);
        process->memory->allocPages(HEAP_ADDRESS >> K_PAGE_SHIFT, 17, PAGE_READ|PAGE_WRITE, 0, 0, 0);
    }

    for (int i=0;i<6;i++) {
        cpu->seg[i].address = 0;
        cpu->seg[i].value = 0;
    }
    cpu->seg[CS].address = CODE_ADDRESS;
    cpu->seg[DS].address = HEAP_ADDRESS;
    cpu->seg[SS].address = STACK_ADDRESS-K_PAGE_SIZE*17;
    cpu->thread->process->hasSetSeg[CS] = true;
    cpu->thread->process->hasSetSeg[DS] = true;
    cpu->thread->process->hasSetSeg[SS] = true;

    zeroMemory(CODE_ADDRESS, K_PAGE_SIZE*17);
    zeroMemory(STACK_ADDRESS-K_PAGE_SIZE*17, K_PAGE_SIZE*17);
    zeroMemory(HEAP_ADDRESS, K_PAGE_SIZE*17);

    ESP=4096;
}

void pushCode8(int value) {
    writeb(cseip, value);
    cseip++;
}

void pushCode16(int value) {
    writew(cseip, value);
    cseip+=2;
}

void pushCode32(int value) {
    writed(cseip, value);
    cseip+=4;
}

void newInstruction(int flags) {
    cseip=CODE_ADDRESS;
    cpu->lazyFlags = FLAGS_NONE;
    cpu->flags = flags;
    if (flags & DF)
        cpu->df = -1;
    else
        cpu->df = 1;
    //cpu.blocks.clear();
    EAX=0;
    ECX=0;
    EDX=0;
    EBP=0;
    ESP=4096;
    EBP=0;
    ESI=0;
    EDI=0;
    cpu->eip.u32=0;   
}

void newInstruction(int instruction, int flags) {    
    newInstruction(flags);
    if (instruction>0xFF)
        pushCode8(0x0F);
    pushCode8(instruction & 0xFF);
}

void newInstructionWithRM(int instruction, int rm, int flags) {
    newInstruction(instruction, flags);
    pushCode8(rm);
}

void runTestCPU() {    
#ifdef BOXEDWINE_X64    
    pushCode8(0xcd);
    pushCode8(0x97); // will cause TEST specific return code to be inserted
    ((x64CPU*)cpu)->translateEip(cpu->eip.u32);
#else
    pushCode8(0x70); // jump causes the decoder to stop building the block
    pushCode8(0);
    pushCode8(0x70); // jump will fetch the next block as well
    pushCode8(0);
    cpu->nextBlock = cpu->getNextBlock();    
#endif
    cpu->run();
#ifdef BOXEDWINE_64BIT_MMU
    KThread::currentThread()->memory->clearCodePageFromCache(CODE_ADDRESS>>K_PAGE_SHIFT);    
#endif
#ifdef BOXEDWINE_X64
    x64CPU* c = (x64CPU*)cpu;
    for (int i=0;i<8;i++) {
        c->reg_mmx[i].q = *((U64*)(c->fpuState+32+i*16));
    }
    for (int i=0;i<8;i++) {
        c->xmm[i].pi.u64[0] = *((U64*)(c->fpuState+160+i*16));
        c->xmm[i].pi.u64[1] = *((U64*)(c->fpuState+160+i*16+8));
    }
#endif
}

struct Data {
    int valid;
    U32 var1;
    U32 var2;
    U32 result;
    U32 resultvar2;
    U32 flags;
    U32 constant;    
    int fCF;
    int fOF;
    int fZF;
    int fSF;
    int dontUseResultAndCheckSFZF;
    int useResultvar2;
    int constantWidth;
    bool hasOF;
    int fAF;
    U32 hasAF;
    U32 hasCF;
    U32 hasZF;
    U32 hasSF;
};

#define endData() {0}
#define allocData(var1, var2, result, flags, fCF, fOF) { 1, var1, var2, result, 0, flags, 0, fCF, fOF, 0, 0, 0, 0, 0, true,  0, 0, 1, 0, 0 }
#define allocDataNoOF(var1, var2, result, flags, fCF) {  1, var1, var2, result, 0, flags, 0, fCF, 0,   0, 0, 0, 0, 0, false, 0, 0, 1, 0, 0 }
#define allocDataFlags(var1, var2, fCF, fOF, fSF, fZF) { 1, var1, var2, 0, 0, 0, 0, fCF, fOF, fZF, fSF, 1, 0, 0, true, 0, 0, 1, 1, 1 }
#define allocDataFlagsWithAF(var1, var2, result, flags, fCF, fOF, fSF, fZF, fAF, hasOF, hasZF, hasSF) { 1, var1, var2, result, 0, flags, 0, fCF, fOF, fZF, fSF, 0, 0, 0, hasOF, fAF, 1, 1, hasZF, hasSF }
#define allocDataConst(var1, var2, result, constant, constantWidth, flags, fCF, fOF) { 1, var1, var2, result, 0, flags, constant, fCF, fOF, 0, 0, 0, 0, constantWidth, true, 0, 0, 1, 0, 0 }
#define allocDataConstNoOF(var1, var2, result, constant, constantWidth, flags, fCF) { 1, var1, var2, result, 0, flags, constant, fCF, 0, 0, 0, 0, 0, constantWidth, false, 0, 0, 1, 0, 0 }
#define allocDatavar2(var1, var2, resultvar1, resultvar2) { 1, var1, var2, resultvar1, resultvar2, 0, 0, 0, 0, 0, 0, 1, 1, 0, true, 0, 0, 1, 0, 0 }
#define allocDataConstvar2(var1, var2, result, flags, fCF, fOF, constant, var2Result) { 1, var1, var2, result, var2Result, flags, constant, fCF, fOF, 0, 0, 0, 1, 0, true, 0, 0, 1, 0, 0 }
#define allocDataNoFlags(var1, var2, result) {1, var1, var2, result, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, false, 0, 0, 0, 0, 0}

void pushConstant(struct Data* data) {
    if (data->constantWidth==8) {
        pushCode8(data->constant);
    } else if (data->constantWidth==16) {
        pushCode16(data->constant);
    } else if (data->constantWidth==32) {
        pushCode32(data->constant);
    }
}

void assertResult(struct Data* data, CPU* cpu, int instruction, U32 resultvar1, U32 resultvar2, int r1, int r2, U32 address, int bits) {
    if (data->useResultvar2 && data->resultvar2!=resultvar2) {
        failed("instruction: %d var2: %d != %d", instruction, resultvar2, data->resultvar2);
    }
    if (!data->dontUseResultAndCheckSFZF && data->result != resultvar1) {
        failed("instruction: %d var1: %d != %d", instruction, resultvar1, data->result);
    }
    if (data->hasCF && cpu->getCF() != (U32)data->fCF) {
        cpu->getCF();
        failed("instruction: %d CF", instruction);
    }
    if (data->hasOF && (cpu->getOF()!=0) != data->fOF) {
        cpu->getOF();
        failed("instruction: %d OF", instruction);
    }
    if (data->hasAF && (cpu->getAF()!=0) != data->fAF) {
        cpu->getAF();
        failed("instruction: %d AF", instruction);
    }
    if (data->hasSF && (cpu->getSF()!=0) != data->fSF) {
        cpu->getSF();
        failed("instruction: %d SF", instruction);
    }
    if (data->hasZF && (cpu->getZF()!=0) != data->fZF) {
        cpu->getZF();
        failed("instruction: %d ZF", instruction);
    }
    if (data->dontUseResultAndCheckSFZF) {
        if ((cpu->getSF()!=0) != data->fSF) {
            failed("instruction: %d SF", instruction);
        }
        if ((cpu->getZF()!=0) != data->fZF) {
            failed("instruction: %d ZF", instruction);
        }
    }
    if (bits==8 || bits==16) {
        if (r1>=0) {
            if ((cpu->reg[r1].u32 & 0xFFFF0000) != (DEFAULT & 0xFFFF0000)) {
                failed("instruction: %d reg overwrite %d", instruction, r1);
            }
        }
        if (r2>=0) {
            if ((cpu->reg[r2].u32 & 0xFFFF0000) != (DEFAULT & 0xFFFF0000)) {
                failed("instruction: %d reg overwrite %d", instruction, r2);
            }
        }
    }
    if (bits == 8) {
        if (address!=0) {
            if ((readd(address) & 0xFFFFFF00) != (DEFAULT & 0xFFFFFF00)) {
                failed("instruction: %d memory overwrite %d", instruction, address);
            }
        }
    }
    if (bits==16) {
        if (address!=0) {
            if ((readd(address) & 0xFFFF0000) != (DEFAULT & 0xFFFF0000)) {
                failed("instruction: %d memory overwrite %d", instruction, address);
            }
        }
    }
}
#define E8(rm) (E(rm) % 4)
#define G8(rm) (G(rm) % 4)

void Eb(int instruction, int which, struct Data* data) {	
    while (data->valid) {
        int eb;
        int rm;
        U32 result;

        for (eb = 0; eb < 8; eb++) {
            U8* reg;

            rm = eb | (which << 3) | 0xC0;
            newInstructionWithRM(instruction, rm, data->flags);
            reg = cpu->reg8[E(rm)];
            cpu->reg[E8(rm)].u32 = DEFAULT;
            *reg = (U8)data->var1;
            runTestCPU();
            assertResult(data, cpu, instruction, *reg, 0, E8(rm), -1, 0, 8);
        }

        rm = (which << 3);
        if (cpu->big)
            rm += 5;
        else
            rm += 6;
        newInstructionWithRM(instruction, rm, data->flags);
        if (cpu->big)
            pushCode32(200);
        else
            pushCode16(200);
        writed(cpu->seg[DS].address + 200, DEFAULT);
        writeb(cpu->seg[DS].address + 200, data->var1);
        runTestCPU();
        result = readb(cpu->seg[DS].address + 200);
        assertResult(data, cpu, instruction, result, 0, -1, -1, cpu->seg[DS].address + 200, 8);
        data++;
    }
}

void EbAlAx(int instruction, int which, struct Data* data, int useAX) {
    while (data->valid) {
        int eb;
        int rm;

        for (eb = 0; eb < 8; eb++) {            
            U8* reg;

            if (eb==0 || eb==4)
                continue;

            rm = eb | (which << 3) | 0xC0;
            reg = cpu->reg8[E(rm)];			
            newInstructionWithRM(instruction, rm, data->flags);			
            cpu->reg[E8(rm)].u32 = DEFAULT;
            EAX = DEFAULT;
            *reg = (U8)data->var2;
            if (useAX)
                AX = data->var1;
            else
                AL = data->var1;
            runTestCPU();
            assertResult(data, cpu, instruction, AX, 0, 0, -1, 0, 8);
        }

        rm = (which << 3);
        if (cpu->big)
            rm += 5;
        else
            rm += 6;
        newInstructionWithRM(instruction, rm, data->flags);
        if (cpu->big)
            pushCode32(200);
        else
            pushCode16(200);
        EAX = DEFAULT;
        if (useAX)
            AX = data->var1;
        else
            AL = data->var1;
        writed(cpu->seg[DS].address + 200, DEFAULT);
        writeb(cpu->seg[DS].address + 200, data->var2);
        runTestCPU();
        assertResult(data, cpu, instruction, AX, 0, 0, -1, 0, 16);
        data++;
    }
}

void EwAxDx(int instruction, int which, struct Data* data, int useDX) {
    while (data->valid) {
        int rm;
        int ew;

        for (ew = 0; ew < 8; ew++) {
            Reg* reg;

            if (ew==0)
                continue;
            if (useDX && ew==2)
                continue;
            rm = ew | (which << 3) | 0xC0;
            newInstructionWithRM(instruction, rm, data->flags);
            reg = &cpu->reg[E(rm)];
            EAX = DEFAULT;
            EDX = DEFAULT;
            reg->u32 = DEFAULT;
            AX = data->var2;
            DX = data->var1;
            reg->u16=data->constant;
            runTestCPU();
            assertResult(data, cpu, instruction, AX, DX, 0, 2, 0, 16);
        }

        rm = (which << 3);
        if (cpu->big)
            rm += 5;
        else
            rm += 6;
        newInstructionWithRM(instruction, rm, data->flags);
        if (cpu->big)
            pushCode32(200);
        else
            pushCode16(200);
        EAX = DEFAULT;
        EDX = DEFAULT;
        AX = data->var2;
        DX = data->var1;
        writed(cpu->seg[DS].address + 200, DEFAULT);
        writew(cpu->seg[DS].address + 200, data->constant);
        runTestCPU();
        assertResult(data, cpu, instruction, AX, DX, 0, 2, 0, 16);
        data++;
    }
}

void EdEaxEdx(int instruction, int which, struct Data* data, int useEdx) {
    while (data->valid) {
        int ed;
        int rm;

        for (ed = 0; ed < 8; ed++) {
            Reg* reg;

            if (ed==0)
                continue;
            if (useEdx && ed==2)
                continue;
            rm = ed | (which << 3) | 0xC0;
            newInstructionWithRM(instruction, rm, data->flags);
            reg = &cpu->reg[E(rm)];
            reg->u32 = DEFAULT;
            EAX = data->var2;
            EDX = data->var1;
            reg->u32 = data->constant;
            runTestCPU();
            assertResult(data, cpu, instruction, EAX, EDX, 0, 2, 0, 32);
        }

        rm = (which << 3);
        if (cpu->big)
            rm += 5;
        else
            rm += 6;
        newInstructionWithRM(instruction, rm, data->flags);
        if (cpu->big)
            pushCode32(200);
        else
            pushCode16(200);
        EAX=data->var2;
        EDX=data->var1;
        writed(cpu->seg[DS].address + 200, DEFAULT);
        writed(cpu->seg[DS].address + 200, data->constant);
        runTestCPU();
        assertResult(data, cpu, instruction, EAX, EDX, 0, 2, 0, 32);
        data++;
    }
}

void EbCl(int instruction, int which, struct Data* data) {
    while (data->valid) {
        int rm;
        U32 result;
        int eb;

        for (eb = 0; eb < 8; eb++) {			
            U8* reg;

            if (eb==1)
                continue;
            rm = eb | (which << 3) | 0xC0;
            newInstructionWithRM(instruction, rm, data->flags);
            reg = cpu->reg8[E(rm)];
            cpu->reg[E8(rm)].u32 = DEFAULT;
            ECX = DEFAULT;
            *reg=data->var1;
            CL = data->var2;
            runTestCPU();
            assertResult(data, cpu, instruction, *reg, 0, E8(rm), -1, 0, 8);
        }

        rm = (which << 3);
        if (cpu->big)
            rm += 5;
        else
            rm += 6;
        newInstructionWithRM(instruction, rm, data->flags);
        ECX = DEFAULT;
        CL = data->var2;
        if (cpu->big)
            pushCode32(200);
        else
            pushCode16(200);
        writed(cpu->seg[DS].address + 200, DEFAULT);
        writeb(cpu->seg[DS].address + 200, data->var1);
        runTestCPU();
        result = readb(cpu->seg[DS].address + 200);
        assertResult(data, cpu, instruction, result, 0, -1, -1, cpu->seg[DS].address + 200, 8);
        data++;
    }
}

void EbIb(int instruction, int which, struct Data* data) {
    while (data->valid) {
        int eb;
        int rm;
        U32 result;

        for (eb = 0; eb < 8; eb++) {
            U8* e;

            rm = eb | (which << 3) | 0xC0;
            newInstructionWithRM(instruction, rm, data->flags);
            pushCode8(data->var2);
            e = cpu->reg8[E(rm)];
            cpu->reg[E8(rm)].u32 = DEFAULT;
            *e=data->var1;
            runTestCPU();
            assertResult(data, cpu, instruction, *e, 0, E8(rm), -1, 0, 8);
        }

        rm = (which << 3);
        if (cpu->big)
            rm += 5;
        else
            rm += 6;
        newInstructionWithRM(instruction, rm, data->flags);
        if (cpu->big)
            pushCode32(200);
        else
            pushCode16(200);
        pushCode8(data->var2);
        writed(cpu->seg[DS].address + 200, DEFAULT);
        writeb(cpu->seg[DS].address + 200, data->var1);
        runTestCPU();
        result = readb(cpu->seg[DS].address + 200);
        assertResult(data, cpu, instruction, result, 0, -1, -1, cpu->seg[DS].address + 200, 8);
        data++;
    }
}

void EbRegIb(int instruction, U8* e, int rm, struct Data* data) {
    while (data->valid) {
        newInstruction(instruction, 0);
        pushCode8(data->var2);
        cpu->reg[E8(rm)].u32 = DEFAULT;
        *e=data->var1;
        runTestCPU();
        assertResult(data, cpu, instruction, *e, 0, E8(rm), -1, 0, 8);
        data++;
    }
}

void EbGb(int instruction, struct Data* data) {
    while (data->valid) {
        int eb;
        int gb;
        int rm;

        for (eb = 0; eb < 8; eb++) {
            for (gb = 0; gb < 8; gb++) {
                U8* e;
                U8* g;

                if (eb == gb)
                    continue;
                rm = eb | (gb << 3) | 0xC0;
                newInstructionWithRM(instruction, rm, data->flags);
                pushConstant(data);
                e = cpu->reg8[E(rm)];
                g = cpu->reg8[G(rm)];
                cpu->reg[E8(rm)].u32 = DEFAULT;
                cpu->reg[G8(rm)].u32 = DEFAULT;
                *e=data->var1;
                *g=data->var2;
                runTestCPU();                
                assertResult(data, cpu, instruction, *e, *g, E8(rm), G8(rm), 0, 8);
            }
        }

        for (gb = 0; gb < 8; gb++) {
            U8* g;
            U32 result;

            rm = (gb << 3);
            if (cpu->big)
                rm += 5;
            else
                rm += 6;
            newInstructionWithRM(instruction, rm, data->flags);
            if (cpu->big)
                pushCode32(200);
            else
                pushCode16(200);
            pushConstant(data);
            writed(cpu->seg[DS].address + 200, DEFAULT);
            writeb(cpu->seg[DS].address + 200, data->var1);
            g = cpu->reg8[G(rm)];
            cpu->reg[G8(rm)].u32 = DEFAULT;
            *g=data->var2;
            runTestCPU();
            result = readb(cpu->seg[DS].address + 200);
            assertResult(data, cpu, instruction, result, *g, G8(rm), -1, cpu->seg[DS].address + 200, 8);
        }
        data++;
    }
}

void GbEb(int instruction, struct Data* data) {
    while (data->valid) {
        int eb;
        int gb;
        int rm;

        for (eb = 0; eb < 8; eb++) {
            for (gb = 0; gb < 8; gb++) {
                U8* e;
                U8* g;

                if (eb == gb)
                    continue;
                rm = eb | (gb << 3) | 0xC0;
                newInstructionWithRM(instruction, rm, data->flags);
                pushConstant(data);
                e = cpu->reg8[E(rm)];
                g = cpu->reg8[G(rm)];
                cpu->reg[E8(rm)].u32 = DEFAULT;
                cpu->reg[G8(rm)].u32 = DEFAULT;
                *e=data->var2;
                *g=data->var1;
                runTestCPU();
                assertResult(data, cpu, instruction, *g, *e, E8(rm), G8(rm), 0, 8);
            }
        }

        for (gb = 0; gb < 8; gb++) {
            U8* g;

            rm = (gb << 3);
            if (cpu->big)
                rm += 5;
            else
                rm += 6;
            newInstructionWithRM(instruction, rm, data->flags);
            if (cpu->big)
                pushCode32(200);
            else
                pushCode16(200);
            pushConstant(data);
            writed(cpu->seg[DS].address + 200, DEFAULT);
            writeb(cpu->seg[DS].address + 200, data->var2);
            g = cpu->reg8[G(rm)];
            cpu->reg[G8(rm)].u32 = DEFAULT;
            *g=data->var1;
            runTestCPU();
            assertResult(data, cpu, instruction, *g, readb(cpu->seg[DS].address + 200), G8(rm), -1, cpu->seg[DS].address + 200, 8);
        }
        data++;
    }
}

void Ew(int instruction, int which, struct Data* data) {
    while (data->valid) {
        int ew;
        int rm;
        U32 result;

        for (ew = 0; ew < 8; ew++) {
            Reg* reg;

            rm = ew | (which << 3) | 0xC0;
            newInstructionWithRM(instruction, rm, data->flags);
            reg = &cpu->reg[E(rm)];
            reg->u32 = DEFAULT;
            reg->u16=data->var1;
            runTestCPU();
            assertResult(data, cpu, instruction, reg->u16, 0, E(rm), -1, 0, 16);
        }

        rm = (which << 3);
        if (cpu->big)
            rm += 5;
        else
            rm += 6;
        newInstructionWithRM(instruction, rm, data->flags);
        if (cpu->big)
            pushCode32(200);
        else
            pushCode16(200);
        writed(cpu->seg[DS].address + 200, DEFAULT);
        writew(cpu->seg[DS].address + 200, data->var1);
        runTestCPU();
        result = readw(cpu->seg[DS].address + 200);
        assertResult(data, cpu, instruction, result, 0, -1, -1, cpu->seg[DS].address + 200, 16);
        data++;
    }
}

void EwCl(int instruction, int which, struct Data* data) {
    while (data->valid) {
        int ew;
        int rm;
        U32 result;

        for (ew = 0; ew < 8; ew++) {
            Reg* reg;

            if (ew==1)
                continue;
            rm = ew | (which << 3) | 0xC0;
            newInstructionWithRM(instruction, rm, data->flags);
            reg = &cpu->reg[E(rm)];
            reg->u32 = DEFAULT;
            ECX = DEFAULT;
            reg->u16=data->var1;
            CL = data->var2;
            runTestCPU();
            assertResult(data, cpu, instruction, reg->u16, 0, E(rm), -1, 0, 16);
        }

        rm = (which << 3);
        if (cpu->big)
            rm += 5;
        else
            rm += 6;
        newInstructionWithRM(instruction, rm, data->flags);
        ECX = DEFAULT;
        CL = data->var2;
        if (cpu->big)
            pushCode32(200);
        else
            pushCode16(200);
        writed(cpu->seg[DS].address + 200, DEFAULT);
        writew(cpu->seg[DS].address + 200, data->var1);
        runTestCPU();
        result = readw(cpu->seg[DS].address + 200);
        assertResult(data, cpu, instruction, result, 0, -1, -1, cpu->seg[DS].address + 200, 16);
        data++;
    }
}

void EwIx(int instruction, int which, struct Data* data) {
    while (data->valid) {
        int ew;
        int rm;
        U32 result;

        if ((S8)(data->var2 & 0xFF) != (S16)data->var2) {
            data++;
            continue;
        }
        for (ew = 0; ew < 8; ew++) {
            Reg* e;

            rm = ew | (which << 3) | 0xC0;
            newInstructionWithRM(instruction, rm, data->flags);
            pushCode8(data->var2);
            e = &cpu->reg[E(rm)];
            e->u32 = DEFAULT;
            e->u16 = data->var1;
            runTestCPU();
            assertResult(data, cpu, instruction, e->u16, 0, E(rm), -1, 0, 16);
        }

        rm = (which << 3);
        if (cpu->big)
            rm += 5;
        else
            rm += 6;
        newInstructionWithRM(instruction, rm, data->flags);
        if (cpu->big)
            pushCode32(200);
        else
            pushCode16(200);
        pushCode8(data->var2);
        writed(cpu->seg[DS].address + 200, DEFAULT);
        writew(cpu->seg[DS].address + 200, data->var1);
        runTestCPU();
        result = readw(cpu->seg[DS].address + 200);
        assertResult(data, cpu, instruction, result, 0, -1, -1, cpu->seg[DS].address + 200, 16);
        data++;
    }
}

void EwRegIw(int instruction, Reg* e, int rm, struct Data* data) {
    while (data->valid) {
        newInstruction(instruction, 0);
        pushCode16(data->var2);
        e->u32 = DEFAULT;
        e->u16 = data->var1;
        runTestCPU();
        assertResult(data, cpu, instruction, e->u16, 0, E(rm), -1, 0, 16);
        data++;
    }
}

void EwRegIb(int instruction, int ew, struct Data* data) {
    while (data->valid) {
        Reg* e = &cpu->reg[ew];
        newInstruction(instruction, 0);
        pushCode8(data->var2);
        e->u32 = DEFAULT;
        e->u16 = data->var1;
        runTestCPU();
        assertResult(data, cpu, instruction, e->u16, 0, ew, -1, 0, 16);
        data++;
    }
}

void EwIb(int instruction, int which, struct Data* data) {
    while (data->valid) {
        int ew;
        int rm;
        U32 result;

        for (ew = 0; ew < 8; ew++) {
            Reg* e;

            rm = ew | (which << 3) | 0xC0;
            newInstructionWithRM(instruction, rm, data->flags);
            pushCode8(data->var2);
            e = &cpu->reg[E(rm)];
            e->u32 = DEFAULT;
            e->u16 = data->var1;
            runTestCPU();
            assertResult(data, cpu, instruction, e->u16, 0, E(rm), -1, 0, 16);
        }

        rm = (which << 3);
        if (cpu->big)
            rm += 5;
        else
            rm += 6;
        newInstructionWithRM(instruction, rm, data->flags);
        if (cpu->big)
            pushCode32(200);
        else
            pushCode16(200);
        pushCode8(data->var2);
        writed(cpu->seg[DS].address + 200, DEFAULT);
        writew(cpu->seg[DS].address + 200, data->var1);
        runTestCPU();
        result = readw(cpu->seg[DS].address + 200);
        assertResult(data, cpu, instruction, result, 0, -1, -1, cpu->seg[DS].address + 200, 16);
        data++;
    }
}

void EwIw(int instruction, int which, struct Data* data) {
    while (data->valid) {
        int ew;
        int rm;
        U32 result;

        for (ew = 0; ew < 8; ew++) {
            Reg* e;

            rm = ew | (which << 3) | 0xC0;
            newInstructionWithRM(instruction, rm, data->flags);
            pushCode16(data->var2);
            e = &cpu->reg[E(rm)];
            e->u32 = DEFAULT;
            e->u16=data->var1;
            runTestCPU();
            assertResult(data, cpu, instruction, e->u16, 0, E(rm), -1, 0, 16);
        }

        rm = (which << 3);
        if (cpu->big)
            rm += 5;
        else
            rm += 6;
        newInstructionWithRM(instruction, rm, data->flags);
        if (cpu->big)
            pushCode32(200);
        else
            pushCode16(200);
        pushCode16(data->var2);
        writed(cpu->seg[DS].address + 200, DEFAULT);
        writew(cpu->seg[DS].address + 200, data->var1);
        runTestCPU();
        result = readw(cpu->seg[DS].address + 200);
        assertResult(data, cpu, instruction, result, 0, -1, -1, cpu->seg[DS].address + 200, 16);
        data++;
    }
}

void EwGw(int instruction, struct Data* data) {
    while (data->valid) {
        int ew;
        int gw;
        int rm;

        for (ew = 0; ew < 8; ew++) {
            for (gw = 0; gw < 8; gw++) {
                Reg* e;
                Reg* g;

                if (ew == gw)
                    continue;
                rm = ew | (gw << 3) | 0xC0;
                newInstructionWithRM(instruction, rm, data->flags);
                pushConstant(data);
                e = &cpu->reg[E(rm)];
                g = &cpu->reg[G(rm)];
                e->u32 = DEFAULT;
                g->u32 = DEFAULT;
                e->u16 = data->var1;
                g->u16 = data->var2;
                runTestCPU();
                assertResult(data, cpu, instruction, e->u16, g->u16, E(rm), G(rm), 0, 16);
            }
        }

        for (gw = 0; gw < 8; gw++) {
            Reg* g;
            U32 result;

            rm = (gw << 3);
            if (cpu->big)
                rm += 5;
            else
                rm += 6;
            newInstructionWithRM(instruction, rm, data->flags);
            if (cpu->big)
                pushCode32(200);
            else
                pushCode16(200);
            pushConstant(data);
            writed(cpu->seg[DS].address + 200, DEFAULT);
            writew(cpu->seg[DS].address + 200, data->var1);
            g = &cpu->reg[G(rm)];
            g->u32 = DEFAULT;
            g->u16 = data->var2;
            runTestCPU();
            result = readw(cpu->seg[DS].address + 200);
            assertResult(data, cpu, instruction, result, g->u16, G(rm), -1, cpu->seg[DS].address + 200, 16);
        }
        data++;
    }
}

void EwGwCl(int instruction, struct Data* data) {
    while (data->valid) {
        int ew;
        int gw;
        int rm;

        for (ew = 0; ew < 8; ew++) {
            for (gw = 0; gw < 8; gw++) {
                Reg* e;
                Reg* g;

                if (ew == gw || ew==1 || gw==1)
                    continue;
                rm = ew | (gw << 3) | 0xC0;
                newInstructionWithRM(instruction, rm, data->flags);
                ECX=DEFAULT;
                CL=data->constant;
                e = &cpu->reg[E(rm)];
                g = &cpu->reg[G(rm)];
                e->u32 = DEFAULT;
                g->u32 = DEFAULT;
                e->u16 = data->var1;
                g->u16 = data->var2;
                runTestCPU();
                assertResult(data, cpu, instruction, e->u16, g->u16, E(rm), G(rm), 0, 16);
            }
        }

        for (gw = 0; gw < 8; gw++) {
            Reg* g;
            U32 result;

            if (gw==1)
                continue;
            rm = (gw << 3);
            if (cpu->big)
                rm += 5;
            else
                rm += 6;
            newInstructionWithRM(instruction, rm, data->flags);
            if (cpu->big)
                pushCode32(200);
            else
                pushCode16(200);
            ECX=DEFAULT;
            CL=data->constant;
            writed(cpu->seg[DS].address + 200, DEFAULT);
            writew(cpu->seg[DS].address + 200, data->var1);
            g = &cpu->reg[G(rm)];
            g->u32 = DEFAULT;
            g->u16 = data->var2;
            runTestCPU();
            result = readw(cpu->seg[DS].address + 200);
            assertResult(data, cpu, instruction, result, g->u16, G(rm), -1, cpu->seg[DS].address + 200, 16);
        }
        data++;
    }
}

void EwGwEffective(int instruction, struct Data* data) {
    while (data->valid) {
        int ew;
        int gw;
        int rm;

        for (ew = 0; ew < 8; ew++) {
            for (gw = 0; gw < 8; gw++) {
                Reg* e;
                Reg* g;

                if (ew == gw)
                    continue;
                rm = ew | (gw << 3) | 0xC0;
                newInstructionWithRM(instruction, rm, data->flags);
                pushConstant(data);
                e = &cpu->reg[E(rm)];
                g = &cpu->reg[G(rm)];
                e->u32 = DEFAULT;
                g->u32 = DEFAULT;
                e->u16 = data->var1;
                g->u16 = data->var2;
                runTestCPU();
                assertResult(data, cpu, instruction, e->u16, g->u16, E(rm), G(rm), 0, 16);
            }
        }

        for (gw = 0; gw < 8; gw++) {
            Reg* g;
            U32 result;
            int offset = 0;

            rm = (gw << 3);
            if (cpu->big) {
                offset = (data->var2 >> 5)*4;
                rm += 5; 
            } else {
                offset = (data->var2 >> 4)*2;
                rm += 6;
            }
            newInstructionWithRM(instruction, rm, data->flags);
            if (cpu->big)
                pushCode32(200);
            else
                pushCode16(200);
            pushConstant(data);
            writed(cpu->seg[DS].address + 200+offset, DEFAULT);
            writew(cpu->seg[DS].address + 200+offset, data->var1);
            g = &cpu->reg[G(rm)];
            g->u32 = DEFAULT;
            g->u16 = data->var2;
            runTestCPU();
            result = readw(cpu->seg[DS].address + 200 + offset);
            assertResult(data, cpu, instruction, result, g->u16, G(rm), -1, cpu->seg[DS].address + 200 + offset, 16);
        }
        data++;
    }
}

void EdGdEffective(int instruction, struct Data* data) {
    while (data->valid) {
        int ed;
        int gd;
        int rm;

        for (ed = 0; ed < 8; ed++) {
            for (gd = 0; gd < 8; gd++) {
                Reg* e;
                Reg* g;

                if (ed == gd)
                    continue;
                rm = ed | (gd << 3) | 0xC0;
                newInstructionWithRM(instruction, rm, data->flags);
                pushConstant(data);
                e = &cpu->reg[E(rm)];
                g = &cpu->reg[G(rm)];
                e->u32 = data->var1;
                g->u32 = data->var2;
                runTestCPU();
                assertResult(data, cpu, instruction, e->u32, g->u32, E(rm), G(rm), 0, 32);
            }
        }

        for (gd = 0; gd < 8; gd++) {
            Reg* g;
            U32 result;
            int offset = 0;

            rm = (gd << 3);
            if (cpu->big) {
                offset = (data->var2 >> 5)*4;
                rm += 5; 
            } else {
                offset = (data->var2 >> 4)*2;
                rm += 6;
            }
            newInstructionWithRM(instruction, rm, data->flags);
            if (cpu->big)
                pushCode32(200);
            else
                pushCode16(200);
            pushConstant(data);
            writed(cpu->seg[DS].address + 200+offset, data->var1);
            g = &cpu->reg[G(rm)];
            g->u32 = data->var2;
            runTestCPU();
            result = readd(cpu->seg[DS].address + 200 + offset);
            assertResult(data, cpu, instruction, result, g->u32, G(rm), -1, cpu->seg[DS].address + 200 + offset, 32);
        }
        data++;
    }
}

void EwSw(int instruction, struct Data* data) {
    while (data->valid) {
        int ew;
        int gw;
        int rm;

        for (ew = 0; ew < 8; ew++) {
            for (gw = 0; gw < 6; gw++) {
                Reg* e;
                U32* g;

                rm = ew | (gw << 3) | 0xC0;
                newInstructionWithRM(instruction, rm, data->flags);
                pushConstant(data);
                e = &cpu->reg[ew];
                g = &cpu->seg[gw].value;
                e->u32 = DEFAULT;
                e->u16 = data->var1;
                *g = data->var2;
                runTestCPU();
                assertResult(data, cpu, instruction, e->u16, *g, ew, -1, 0, 16);
            }
        }

        for (gw = 0; gw < 6; gw++) {
            U32* g;
            U32 result;

            rm = (gw << 3);
            if (cpu->big)
                rm += 5;
            else
                rm += 6;
            newInstructionWithRM(instruction, rm, data->flags);
            if (cpu->big)
                pushCode32(200);
            else
                pushCode16(200);
            pushConstant(data);
            writed(cpu->seg[DS].address + 200, DEFAULT);
            writew(cpu->seg[DS].address + 200, data->var1);
            g = &cpu->seg[gw].value;
            *g = data->var2;
            runTestCPU();
            result = readw(cpu->seg[DS].address + 200);
            assertResult(data, cpu, instruction, result, *g, -1, -1, cpu->seg[DS].address + 200, 16);
        }
        data++;
    }
}

void EdSw(int instruction, struct Data* data) {
    while (data->valid) {
        int ew;
        int gw;
        int rm;

        for (ew = 0; ew < 8; ew++) {
            for (gw = 0; gw < 6; gw++) {
                Reg* e;
                U32* g;

                rm = ew | (gw << 3) | 0xC0;
                newInstructionWithRM(instruction, rm, data->flags);
                pushConstant(data);
                e = &cpu->reg[ew];
                g = &cpu->seg[gw].value;
                e->u32 = data->var1;
                *g = data->var2;
                runTestCPU();
                assertResult(data, cpu, instruction, e->u16, *g, ew, -1, 0, 32);
            }
        }

        for (gw = 0; gw < 6; gw++) {
            U32* g;
            U32 result;

            rm = (gw << 3);
            if (cpu->big)
                rm += 5;
            else
                rm += 6;
            newInstructionWithRM(instruction, rm, data->flags);
            if (cpu->big)
                pushCode32(200);
            else
                pushCode16(200);
            pushConstant(data);
            writed(cpu->seg[DS].address + 200, DEFAULT);
            writew(cpu->seg[DS].address + 200, data->var1);
            g = &cpu->seg[gw].value;
            *g = data->var2;
            runTestCPU();
            result = readw(cpu->seg[DS].address + 200);
            assertResult(data, cpu, instruction, result, *g, -1, -1, cpu->seg[DS].address + 200, 32);
        }
        data++;
    }
}

void GwEw(int instruction, struct Data* data) {
    while (data->valid) {
        int ew;
        int gw;
        int rm;

        for (ew = 0; ew < 8; ew++) {
            for (gw = 0; gw < 8; gw++) {
                Reg* e;
                Reg* g;

                if (ew == gw)
                    continue;
                rm = ew | (gw << 3) | 0xC0;
                newInstructionWithRM(instruction, rm, data->flags);
                pushConstant(data);
                e = &cpu->reg[ew];
                g = &cpu->reg[gw];
                e->u32 = DEFAULT;
                g->u32 = DEFAULT;
                e->u16=data->var2;
                g->u16=data->var1;
                runTestCPU();
                assertResult(data, cpu, instruction, g->u16, e->u16, ew, gw, 0, 16);
            }
        }

        for (gw = 0; gw < 8; gw++) {
            U32 result;
            Reg* g;

            rm = (gw << 3);
            if (cpu->big)
                rm += 5;
            else
                rm += 6;
            newInstructionWithRM(instruction, rm, data->flags);
            if (cpu->big)
                pushCode32(200);
            else
                pushCode16(200);
            pushConstant(data);
            writed(cpu->seg[DS].address + 200, DEFAULT);
            writew(cpu->seg[DS].address + 200, data->var2);
            g = &cpu->reg[gw];
            g->u32 = DEFAULT;
            g->u16 = data->var1;
            runTestCPU();
            result = readw(cpu->seg[DS].address + 200);
            assertResult(data, cpu, instruction, g->u16, result, gw, -1, cpu->seg[DS].address + 200, 16);
        }
        data++;
    }
}

void Ed(int instruction, int which, struct Data* data) {
    while (data->valid) {
        int ed;
        int rm;
        U32 result;

        for (ed = 0; ed < 8; ed++) {
            Reg* reg;

            rm = ed | (which << 3) | 0xC0;
            newInstructionWithRM(instruction, rm, data->flags);
            reg = &cpu->reg[ed];
            reg->u32 = data->var1;
            runTestCPU();
            assertResult(data, cpu, instruction, reg->u32, 0, ed, -1, 0, 32);
        }

        rm = (which << 3);
        if (cpu->big)
            rm += 5;
        else
            rm += 6;
        newInstructionWithRM(instruction, rm, data->flags);
        if (cpu->big)
            pushCode32(200);
        else
            pushCode16(200);
        writed(cpu->seg[DS].address + 200, data->var1);
        runTestCPU();
        result = readd(cpu->seg[DS].address + 200);
        assertResult(data, cpu, instruction, result, 0, -1, -1, cpu->seg[DS].address + 200, 32);
        data++;
    }
}

void EdCl(int instruction, int which, struct Data* data) {
    while (data->valid) {
        int ed;
        int rm;
        U32 result;

        for (ed = 0; ed < 8; ed++) {
            Reg* reg;

            if (ed==1)
                continue;
            rm = ed | (which << 3) | 0xC0;
            newInstructionWithRM(instruction, rm, data->flags);
            reg = &cpu->reg[ed];
            ECX = DEFAULT;
            reg->u32 = data->var1;
            CL=data->var2;
            runTestCPU();
            assertResult(data, cpu, instruction, reg->u32, 0, ed, -1, 0, 32);
        }

        rm = (which << 3);
        if (cpu->big)
            rm += 5;
        else
            rm += 6;
        newInstructionWithRM(instruction, rm, data->flags);
        ECX = DEFAULT;
        CL = data->var2;
        if (cpu->big)
            pushCode32(200);
        else
            pushCode16(200);
        writed(cpu->seg[DS].address + 200, data->var1);
        runTestCPU();
        result = readd(cpu->seg[DS].address + 200);
        assertResult(data, cpu, instruction, result, 0, -1, -1, cpu->seg[DS].address + 200, 32);
        data++;
    }
}

void EdIx(int instruction, int which, struct Data* data) {
    while (data->valid) {
        int ed;
        int rm;
        U32 result;

        if ((S8)(data->var2 & 0xFF) != (S32)data->var2) {
            data++;
            continue;
        }
        for (ed = 0; ed < 8; ed++) {
            Reg* e;

            rm = ed | (which << 3) | 0xC0;
            newInstructionWithRM(instruction, rm, data->flags);
            pushCode8(data->var2);
            e = &cpu->reg[ed];
            e->u32 = data->var1;
            runTestCPU();
            assertResult(data, cpu, instruction, e->u32, 0, -1, -1, 0, 0);
        }

        rm = (which << 3);
        if (cpu->big)
            rm += 5;
        else
            rm += 6;
        newInstructionWithRM(instruction, rm, data->flags);
        if (cpu->big)
            pushCode32(200);
        else
            pushCode16(200);
        pushCode8(data->var2);
        writed(cpu->seg[DS].address + 200, data->var1);
        runTestCPU();
        result = readd(cpu->seg[DS].address + 200);
        assertResult(data, cpu, instruction, result, 0, -1, -1, 0, 0);
        data++;
    }
}

void EdIb(int instruction, int which, struct Data* data) {
    while (data->valid) {
        int ed;
        int rm;
        U32 result;

        for (ed = 0; ed < 8; ed++) {
            Reg* e;

            rm = ed | (which << 3) | 0xC0;
            newInstructionWithRM(instruction, rm, data->flags);
            pushCode8(data->var2);
            e = &cpu->reg[ed];
            e->u32 = data->var1;
            runTestCPU();
            assertResult(data, cpu, instruction, e->u32, 0, ed, -1, 0, 32);
        }

        rm = (which << 3);
        if (cpu->big)
            rm += 5;
        else
            rm += 6;
        newInstructionWithRM(instruction, rm, data->flags);
        if (cpu->big)
            pushCode32(200);
        else
            pushCode16(200);
        pushCode8(data->var2);
        writed(cpu->seg[DS].address + 200, DEFAULT);
        writed(cpu->seg[DS].address + 200, data->var1);
        runTestCPU();
        result = readd(cpu->seg[DS].address + 200);
        assertResult(data, cpu, instruction, result, 0, -1, -1, cpu->seg[DS].address + 200, 32);
        data++;
    }
}

void EdRegId(int instruction, Reg* e, int ed, struct Data* data) {
    while (data->valid) {
        newInstruction(instruction, 0);
        pushCode32(data->var2);
        e->u32=data->var1;
        runTestCPU();
        assertResult(data, cpu, instruction, e->u32, 0, ed, -1, 0, 32);
        data++;
    }
}

void EdId(int instruction, int which, struct Data* data) {
    while (data->valid) {
        int ed;
        int rm;
        U32 result;

        for (ed = 0; ed < 8; ed++) {
            Reg* e;

            rm = ed | (which << 3) | 0xC0;
            newInstructionWithRM(instruction, rm, data->flags);
            pushCode32(data->var2);
            e = &cpu->reg[ed];
            e->u32 = data->var1;
            runTestCPU();
            assertResult(data, cpu, instruction, e->u32, 0, -1, -1, 0, 0);
        }

        rm = (which << 3);
        if (cpu->big)
            rm += 5;
        else
            rm += 6;
        newInstructionWithRM(instruction, rm, data->flags);
        if (cpu->big)
            pushCode32(200);
        else
            pushCode16(200);
        pushCode32(data->var2);
        writed(cpu->seg[DS].address + 200, data->var1);
        runTestCPU();
        result = readd(cpu->seg[DS].address + 200);
        assertResult(data, cpu, instruction, result, 0, -1, -1, 0, 0);
        data++;
    }
}

void EdGd(int instruction, struct Data* data) {
    while (data->valid) {
        int ed;
        int gd;
        int rm;

        for (ed = 0; ed < 8; ed++) {
            for (gd = 0; gd < 8; gd++) {
                Reg* e;
                Reg* g;

                if (ed == gd)
                    continue;
                rm = ed | (gd << 3) | 0xC0;
                newInstructionWithRM(instruction, rm, data->flags);
                pushConstant(data);
                e = &cpu->reg[ed];
                g = &cpu->reg[gd];
                e->u32 = data->var1;
                g->u32 = data->var2;
                runTestCPU();
                assertResult(data, cpu, instruction, e->u32, g->u32, -1, -1, 0, 0);
            }
        }

        for (gd = 0; gd < 8; gd++) {
            Reg* g;
            U32 result;

            rm = (gd << 3);
            if (cpu->big)
                rm += 5;
            else
                rm += 6;
            newInstructionWithRM(instruction, rm, data->flags);
            if (cpu->big)
                pushCode32(200);
            else
                pushCode16(200);
            pushConstant(data);
            writed(cpu->seg[DS].address + 200, data->var1);
            g = &cpu->reg[gd];
            g->u32 = data->var2;
            runTestCPU();
            result = readd(cpu->seg[DS].address + 200);
            assertResult(data, cpu, instruction, result, g->u32, -1, -1, 0, 0);
        }
        data++;
    }
}

void EdGdEax(int instruction, struct Data* data) {
    while (data->valid) {
        int ed;
        int gd;
        int rm;

        for (ed = 1; ed < 8; ed++) {
            for (gd = 1; gd < 8; gd++) {
                Reg* e;
                Reg* g;

                if (ed == gd)
                    continue;
                rm = ed | (gd << 3) | 0xC0;
                newInstructionWithRM(instruction, rm, data->flags);
                pushConstant(data);
                e = &cpu->reg[ed];
                g = &cpu->reg[gd];
                e->u32 = data->var1;
                g->u32 = data->var2;
                EAX = data->constant;
                runTestCPU();
                assertResult(data, cpu, instruction, e->u32, EAX, -1, -1, 0, 0);
            }
        }

        for (gd = 1; gd < 8; gd++) {
            Reg* g;
            U32 result;

            rm = (gd << 3);
            if (cpu->big)
                rm += 5;
            else
                rm += 6;
            newInstructionWithRM(instruction, rm, data->flags);
            if (cpu->big)
                pushCode32(200);
            else
                pushCode16(200);
            pushConstant(data);
            writed(cpu->seg[DS].address + 200, data->var1);
            g = &cpu->reg[gd];
            g->u32 = data->var2;
            EAX = data->constant;
            runTestCPU();
            result = readd(cpu->seg[DS].address + 200);
            assertResult(data, cpu, instruction, result, EAX, -1, -1, 0, 0);
        }
        data++;
    }
}

void EdGdCl(int instruction, struct Data* data) {
    while (data->valid) {
        int ed;
        int gd;
        int rm;

        for (ed = 0; ed < 8; ed++) {
            for (gd = 0; gd < 8; gd++) {
                Reg* e;
                Reg* g;

                if (ed == gd || ed==1 || gd==1)
                    continue;
                rm = ed | (gd << 3) | 0xC0;
                newInstructionWithRM(instruction, rm, data->flags);
                ECX = DEFAULT;
                CL = data->constant;
                e = &cpu->reg[ed];
                g = &cpu->reg[gd];
                e->u32 = data->var1;
                g->u32 = data->var2;
                runTestCPU();
                assertResult(data, cpu, instruction, e->u32, g->u32, -1, -1, 0, 0);
            }
        }

        for (gd = 0; gd < 8; gd++) {
            Reg* g;
            U32 result;

            if (gd==1)
                continue;
            rm = (gd << 3);
            if (cpu->big)
                rm += 5;
            else
                rm += 6;
            newInstructionWithRM(instruction, rm, data->flags);
            if (cpu->big)
                pushCode32(200);
            else
                pushCode16(200);
            ECX = DEFAULT;
            CL = data->constant;
            writed(cpu->seg[DS].address + 200, data->var1);
            g = &cpu->reg[gd];
            g->u32 = data->var2;
            runTestCPU();
            result = readd(cpu->seg[DS].address + 200);
            assertResult(data, cpu, instruction, result, g->u32, -1, -1, 0, 0);
        }
        data++;
    }
}

void GdEd(int instruction, struct Data* data) {
    while (data->valid) {
        int ed;
        int gd;
        int rm;

        for (ed = 0; ed < 8; ed++) {
            for (gd = 0; gd < 8; gd++) {
                Reg* e;
                Reg* g;

                if (ed == gd)
                    continue;
                rm = ed | (gd << 3) | 0xC0;
                newInstructionWithRM(instruction, rm, data->flags);
                pushConstant(data);
                e = &cpu->reg[ed];
                g = &cpu->reg[gd];
                e->u32 = data->var2;
                g->u32 = data->var1;
                runTestCPU();
                assertResult(data, cpu, instruction, g->u32, e->u32, -1, -1, 0, 0);
            }
        }

        for (gd = 0; gd < 8; gd++) {
            Reg* g;
            U32 result;

            rm = (gd << 3);
            if (cpu->big)
                rm += 5;
            else
                rm += 6;
            newInstructionWithRM(instruction, rm, data->flags);
            if (cpu->big)
                pushCode32(200);
            else
                pushCode16(200);
            pushConstant(data);
            writed(cpu->seg[DS].address + 200, data->var2);
            g = &cpu->reg[gd];
            g->u32 = data->var1;
            runTestCPU();
            result = readd(cpu->seg[DS].address + 200);
            assertResult(data, cpu, instruction, g->u32, result, -1, -1, 0, 0);
        }
        data++;
    }
}

void AlIb(int instruction, struct Data* data) {
    while (data->valid) {
        newInstruction(instruction, data->flags);
        pushCode8(data->var2);
        EAX = DEFAULT;
        AL = data->var1;
        runTestCPU();
        assertResult(data, cpu, instruction, AL, 0, 0, -1, 0, 8);
        data++;
    }
}

void AxIw(int instruction, struct Data* data) {
    while (data->valid) {
        newInstruction(instruction, data->flags);
        pushCode16(data->var2);
        EAX = DEFAULT;
        AX = data->var1;
        runTestCPU();
        assertResult(data, cpu, instruction, AX, 0, 0, -1, 0, 16);
        data++;
    }
}

void EaxId(int instruction, struct Data* data) {
    while (data->valid) {
        newInstruction(instruction, data->flags);
        pushCode32(data->var2);
        EAX = data->var1;
        runTestCPU();
        assertResult(data, cpu, instruction, EAX, 0, -1, -1, 0, 0);
        data++;
    }
}

void EwReg(int instruction, int ew, struct Data* data) {
    while (data->valid) {
        Reg* reg = &cpu->reg[ew];
        newInstruction(instruction, data->flags);
        reg->u32 = DEFAULT;
        reg->u16 = data->var1;
        runTestCPU();
        assertResult(data, cpu, instruction, reg->u16, 0, ew, -1, 0, 16);
        data++;
    }
}

void EbReg(int instruction, int eb, struct Data* data) {
    while (data->valid) {
        Reg* reg = &cpu->reg[E8(eb)];
        U8* e = cpu->reg8[E(eb)];

        newInstruction(instruction, data->flags);
        reg->u32 = DEFAULT;
        *e = data->var1;
        runTestCPU();
        assertResult(data, cpu, instruction, reg->u8, 0, eb, -1, 0, 8);
        data++;
    }
}

void LeaGw() {
    int i;
    int rm;
    Reg* reg;

    for (i=0;i<8;i++) {
        if (i==6) {
            continue;
        }
        newInstruction(0x8d, 0);
        rm = i<<3 | 0x44;
        pushCode8(rm);
        pushCode8(8);
        reg = &cpu->reg[i];
        reg->u32 = DEFAULT;
        ESI = DEFAULT;
        SI = 0xABCD;
        runTestCPU();
        assertTrue(reg->u16==(0xABCD+8));
        assertTrue((reg->u32 & 0xFFFF0000)==(DEFAULT & 0xFFFF0000));
    }
}

void LeaGd() {
    int i;
    int rm;
    Reg* reg;

    for (i=0;i<8;i++) {
        if (i==3) {
            continue;
        }
        newInstruction(0x8d, 0);
        rm = i<<3 | 0x43;
        pushCode8(rm);
        pushCode8(8);
        reg = &cpu->reg[i];
        EBX=0xABCD1234;
        runTestCPU();
        assertTrue(reg->u32==(0xABCD1234+8));
    }
}

void EdReg(int instruction, int ed, struct Data* data) {
    while (data->valid) {
        Reg* reg = &cpu->reg[ed];
        newInstruction(instruction, data->flags);
        reg->u32 = data->var1;
        runTestCPU();
        assertResult(data, cpu, instruction, reg->u32, 0, -1, -1, 0, 0);
        data++;
    }
}

void Reg16Reg16(int instruction, struct Data* data, Reg* r1, Reg* r2) {
    while (data->valid) {
        newInstruction(instruction, data->flags);
        r1->u32 = DEFAULT;
        r1->u16 = data->var1;
        r2->u32 = DEFAULT;
        r2->u16 = data->var2;
        runTestCPU();
        assertResult(data, cpu, instruction, r1->u16, r2->u16, -1, -1, 0, 16);
        data++;
    }
}

void Reg32Reg32(int instruction, struct Data* data, Reg* r1, Reg* r2) {
    while (data->valid) {
        newInstruction(instruction, data->flags);
        r1->u32 = data->var1;
        r2->u32 = data->var2;
        runTestCPU();
        assertResult(data, cpu, instruction, r1->u32, r2->u32, -1, -1, 0, 0);
        data++;
    }
}

void push16Reg(int instruction, Reg* reg) {
    newInstruction(instruction, 0);
    U32 value = 0xDDDD1234;
    ESP-=2;
    if (reg==&cpu->reg[4]) {
        value = reg->u16;
    } else {
        reg->u32 = value;
    }    
    writew(cpu->seg[SS].address+ESP, 0xAAAA);
    writew(cpu->seg[SS].address+ESP-2, 0xCCCC);
    writew(cpu->seg[SS].address+ESP-4, 0xBBBB);
    runTestCPU();
    assertTrue(ESP==4092);
    assertTrue(readw(cpu->seg[SS].address+ESP)==(U16)value);
    assertTrue(readw(cpu->seg[SS].address+ESP+2)==0xAAAA);
    assertTrue(readw(cpu->seg[SS].address+ESP-2)==0xBBBB);
}

void Pushf(int instruction) {
    newInstruction(instruction, 0);
    cpu->flags = FMASK_TEST;
    ESP-=2;
    writew(cpu->seg[SS].address+ESP, 0xAAAA);
    writew(cpu->seg[SS].address+ESP-2, 0xCCCC);
    writew(cpu->seg[SS].address+ESP-4, 0xBBBB);
    runTestCPU();
    assertTrue(ESP==4092);
    assertTrue((readw(cpu->seg[SS].address+ESP) & (FMASK_TEST|2))==(FMASK_TEST|2)); // bit 1 is always set
    assertTrue(readw(cpu->seg[SS].address+ESP+2)==0xAAAA);
    assertTrue(readw(cpu->seg[SS].address+ESP-2)==0xBBBB);
}

void push32Reg(int instruction, Reg* reg) {
    newInstruction(instruction, 0);
    U32 value = 0x56781234;
    ESP-=4;
    if (reg==&cpu->reg[4]) {
        value = reg->u32;
    } else {
        reg->u32 = value;
    }
    writed(cpu->seg[SS].address+ESP, 0xAAAAAAAA);
    writed(cpu->seg[SS].address+ESP-4, 0xCCCCCCCC);
    writed(cpu->seg[SS].address+ESP-8, 0xBBBBBBBB);
    runTestCPU();
    assertTrue(ESP==4088);
    assertTrue(readd(cpu->seg[SS].address+ESP)==value);
    assertTrue(readd(cpu->seg[SS].address+ESP+4)==0xAAAAAAAA);
    assertTrue(readd(cpu->seg[SS].address+ESP-4)==0xBBBBBBBB);
}

void Pushfd(int instruction) {
    newInstruction(instruction, 0);
    cpu->flags = FMASK_TEST;
    ESP-=4;
    writed(cpu->seg[SS].address+ESP, 0xAAAAAAAA);
    writed(cpu->seg[SS].address+ESP-4, 0xCCCCCCCC);
    writed(cpu->seg[SS].address+ESP-8, 0xBBBBBBBB);
    runTestCPU();
    assertTrue(ESP==4088);
    assertTrue((readd(cpu->seg[SS].address+ESP) & (FMASK_TEST|2))==(FMASK_TEST | 2)); // bit 1 is always set
    assertTrue(readd(cpu->seg[SS].address+ESP+4)==0xAAAAAAAA);
    assertTrue(readd(cpu->seg[SS].address+ESP-4)==0xBBBBBBBB);
}

void Pop16(int instruction, Reg* reg) {
    newInstruction(instruction, 0);
    SP-=2;
    reg->u32=0xDDDDDDDD;
    writew(cpu->seg[SS].address+SP, 0xAAAA);
    writew(cpu->seg[SS].address+SP-2, 0x1234);
    writew(cpu->seg[SS].address+SP-4, 0xBBBB);
    SP-=2;
    runTestCPU();
    assertTrue(SP==4094);
    assertTrue(reg->u32 == 0xDDDD1234);
    assertTrue(readw(cpu->seg[SS].address+SP)==0xAAAA);
    assertTrue(readw(cpu->seg[SS].address+SP-4)==0xBBBB);
}

void Pop16_SP(int instruction, Reg* reg) {
    newInstruction(instruction, 0);
    SP-=2;
    U16 value = SP-2;
    cpu->stackMask=0xffff;
    cpu->stackNotMask=0xffff0000;
    cpu->thread->process->hasSetSeg[SS]=true;
    reg->h16=0xDDDD;
    writew(cpu->seg[SS].address+SP, 0xAAAA);
    writew(cpu->seg[SS].address+SP-2, value);
    writew(cpu->seg[SS].address+SP-4, 0xBBBB);
    SP-=2;
    runTestCPU();
    assertTrue(SP==4092);
    assertTrue(reg->u32 == ((U32)(0xDDDD << 16) | value));
    assertTrue(readw(cpu->seg[SS].address+4094)==0xAAAA);
    assertTrue(readw(cpu->seg[SS].address+4090)==0xBBBB);
    cpu->stackMask=0xffffffff;
    cpu->stackNotMask=0;
}

void Popf(int instruction) {
    newInstruction(instruction, 0);
    ESP-=2;
    cpu->flags=0;
    writew(cpu->seg[SS].address+ESP, 0xAAAA);
    writew(cpu->seg[SS].address+ESP-2, FMASK_TEST);
    writew(cpu->seg[SS].address+ESP-4, 0xBBBB);
    ESP-=2;
    runTestCPU();
    assertTrue(ESP==4094);
    assertTrue((cpu->flags & (FMASK_TEST | 2)) == (FMASK_TEST | 2));
    assertTrue(readw(cpu->seg[SS].address+ESP)==0xAAAA);
    assertTrue(readw(cpu->seg[SS].address+ESP-4)==0xBBBB);
}

void Pop32(int instruction, Reg* reg) {
    newInstruction(instruction, 0);
    ESP-=4;
    writed(cpu->seg[SS].address+ESP, 0xAAAAAAAA);
    writed(cpu->seg[SS].address+ESP-4, 0x56781234);
    writed(cpu->seg[SS].address+ESP-8, 0xBBBBBBBB);
    ESP-=4;
    runTestCPU();
    assertTrue(ESP==4092);
    assertTrue(reg->u32 == 0x56781234);
    assertTrue(readd(cpu->seg[SS].address+ESP)==0xAAAAAAAA);
    assertTrue(readd(cpu->seg[SS].address+ESP-8)==0xBBBBBBBB);
}

void Pop32_SP(int instruction, Reg* reg) {
    newInstruction(instruction, 0);
    ESP-=4;
    writed(cpu->seg[SS].address+ESP, 0xAAAAAAAA);
    writed(cpu->seg[SS].address+ESP-4, ESP-4);
    writed(cpu->seg[SS].address+ESP-8, 0xBBBBBBBB);
    ESP-=4;
    runTestCPU();
    assertTrue(ESP==4088);
    assertTrue(readd(cpu->seg[SS].address+4092)==0xAAAAAAAA);
    assertTrue(readd(cpu->seg[SS].address+4084)==0xBBBBBBBB);
}

void Popfd(int instruction) {
    newInstruction(instruction, 0);
    ESP-=4;
    cpu->flags=0;
    writed(cpu->seg[SS].address+ESP, 0xAAAAAAAA);
    writed(cpu->seg[SS].address+ESP-4, FMASK_TEST);
    writed(cpu->seg[SS].address+ESP-8, 0xBBBBBBBB);
    ESP-=4;
    runTestCPU();
    assertTrue(ESP==4092);
    assertTrue((cpu->flags & (FMASK_TEST|2)) == (FMASK_TEST | 2));
    assertTrue(readd(cpu->seg[SS].address+ESP)==0xAAAAAAAA);
    assertTrue(readd(cpu->seg[SS].address+ESP-8)==0xBBBBBBBB);
}

void flags(int instruction, struct Data* data, Reg* reg) {
    while (data->valid) {
        U32 mask = FMASK_TEST & 0xFF;
        newInstruction(instruction, 0);
        cpu->flags = (data->var1 & FMASK_TEST);
        data->result &= FMASK_TEST | 2;
        reg->u32 = data->var2;
        runTestCPU();
        assertTrue(reg->u32 == data->resultvar2);
        assertTrue(((cpu->flags & FMASK_TEST) | 2) == ((data->result & ~mask) | (data->result & mask) | 2));
        data++;
    }
}

void PopEw() {
    int i;
    Reg* reg;

    for (i=0;i<8;i++) {
        if (i==4)
            continue;
        newInstruction(0x8f, 0);
        pushCode8(i|0xC0);
        reg = &cpu->reg[i];
        ESP-=2;
        reg->u32=0xDDDDDDDD;
        writew(cpu->seg[SS].address+ESP, 0xAAAA);
        writew(cpu->seg[SS].address+ESP-2, 0x1234);
        writew(cpu->seg[SS].address+ESP-4, 0xBBBB);
        ESP-=2;
        runTestCPU();
        assertTrue(ESP==4094);
        assertTrue(reg->u32 == 0xDDDD1234);
        assertTrue(readw(cpu->seg[SS].address+ESP)==0xAAAA);
        assertTrue(readw(cpu->seg[SS].address+ESP-4)==0xBBBB);
    }

    newInstruction(0x8f, 0);
    pushCode8(6);
    pushCode16(200);
    writed(cpu->seg[DS].address + 200, DEFAULT);

    ESP-=2;
    writew(cpu->seg[SS].address+ESP, 0xAAAA);
    writew(cpu->seg[SS].address+ESP-2, 0x1234);
    writew(cpu->seg[SS].address+ESP-4, 0xBBBB);
    ESP-=2;
    runTestCPU();

    assertTrue(ESP==4094);
    assertTrue(readd(cpu->seg[DS].address + 200) == ((DEFAULT & 0xFFFF0000) | 0x1234));
    assertTrue(readw(cpu->seg[SS].address+ESP)==0xAAAA);
    assertTrue(readw(cpu->seg[SS].address+ESP-4)==0xBBBB);
}

void PopEd() {
    int i;
    Reg* reg;

    for (i=0;i<8;i++) {
        if (i==4)
            continue;
        newInstruction(0x8f, 0);
        pushCode8(i|0xC0);
        reg = &cpu->reg[i];
        ESP-=4;
        writed(cpu->seg[SS].address+ESP, 0xAAAAAAAA);
        writed(cpu->seg[SS].address+ESP-4, 0x56781234);
        writed(cpu->seg[SS].address+ESP-8, 0xBBBBBBBB);
        ESP-=4;
        runTestCPU();
        assertTrue(ESP==4092);
        assertTrue(reg->u32 == 0x56781234);
        assertTrue(readd(cpu->seg[SS].address+ESP)==0xAAAAAAAA);
        assertTrue(readd(cpu->seg[SS].address+ESP-8)==0xBBBBBBBB);
    }

    newInstruction(0x8f, 0);
    pushCode8(5);
    pushCode32(200);
    writed(cpu->seg[DS].address + 200, DEFAULT);

    ESP-=4;
    writed(cpu->seg[SS].address + ESP, 0xAAAAAAAA);
    writed(cpu->seg[SS].address + ESP - 4, 0x56781234);
    writed(cpu->seg[SS].address + ESP - 8, 0xBBBBBBBB);
    ESP-=4;
    runTestCPU();

    assertTrue(ESP==4092);
    assertTrue(readd(cpu->seg[DS].address + 200) == 0x56781234);
    assertTrue(readd(cpu->seg[SS].address + ESP)==0xAAAAAAAA);
    assertTrue(readd(cpu->seg[SS].address + ESP - 8)==0xBBBBBBBB);
}

void push16(int instruction) {
    newInstruction(instruction, 0);
    pushCode16(0x1234);
    ESP-=2;
    writew(cpu->seg[SS].address+ESP, 0xAAAA);
    writew(cpu->seg[SS].address+ESP-2, 0xCCCC);
    writew(cpu->seg[SS].address+ESP-4, 0xBBBB);
    runTestCPU();
    assertTrue(ESP==4092);
    assertTrue(readw(cpu->seg[SS].address+ESP)==0x1234);
    assertTrue(readw(cpu->seg[SS].address+ESP+2)==0xAAAA);
    assertTrue(readw(cpu->seg[SS].address+ESP-2)==0xBBBB);
}

void push32(int instruction) {
    newInstruction(instruction, 0);
    pushCode32(0x56781234);
    ESP-=4;
    writed(cpu->seg[SS].address+ESP, 0xAAAAAAAA);
    writed(cpu->seg[SS].address+ESP-4, 0xCCCCCCCC);
    writed(cpu->seg[SS].address+ESP-8, 0xBBBBBBBB);
    runTestCPU();
    assertTrue(ESP==4088);
    assertTrue(readd(cpu->seg[SS].address+ESP)==0x56781234);
    assertTrue(readd(cpu->seg[SS].address+ESP+4)==0xAAAAAAAA);
    assertTrue(readd(cpu->seg[SS].address+ESP-4)==0xBBBBBBBB);
}

void push16s8(int instruction) {
    newInstruction(instruction, 0);
    pushCode8(0xFC); // -4
    ESP-=2;
    writew(cpu->seg[SS].address+ESP, 0xAAAA);
    writew(cpu->seg[SS].address+ESP-2, 0xCCCC);
    writew(cpu->seg[SS].address+ESP-4, 0xBBBB);
    runTestCPU();
    assertTrue(ESP==4092);
    assertTrue(readw(cpu->seg[SS].address+ESP)==0xFFFC);
    assertTrue(readw(cpu->seg[SS].address+ESP+2)==0xAAAA);
    assertTrue(readw(cpu->seg[SS].address+ESP-2)==0xBBBB);
}

void push32s8(int instruction) {
    newInstruction(instruction, 0);
    pushCode8(0xFC); // -4
    ESP-=4;
    writed(cpu->seg[SS].address+ESP, 0xAAAAAAAA);
    writed(cpu->seg[SS].address+ESP-4, 0xCCCCCCCC);
    writed(cpu->seg[SS].address+ESP-8, 0xBBBBBBBB);
    runTestCPU();
    assertTrue(ESP==4088);
    assertTrue(readd(cpu->seg[SS].address+ESP)==0xFFFFFFFC);
    assertTrue(readd(cpu->seg[SS].address+ESP+4)==0xAAAAAAAA);
    assertTrue(readd(cpu->seg[SS].address+ESP-4)==0xBBBBBBBB);
}        

#define false 0
#define true 1


static struct Data addb[] = {
        allocData(1, 2, 3, 0, false, false),
        allocData(0xFF, 1, 0, 0, true, false),
        allocData(1, 0xFF, 0, 0, true, false),
        allocData(0, 0, 0, 0, false, false),
        allocData(0xFF, 0xFF, 0xFE, 0, true, false),
        allocData(64, 64, 128, 0, false, true), // overflow indicates that the sign changed
        endData()
};

static struct Data addw[] = {
        allocData(1000, 2002, 3002, 0, false, false),
        allocData(0xFFFF, 1, 0, 0, true, false),
        allocData(1, 0xFFFF, 0, 0, true, false),
        allocData(0, 0, 0, 0, false, false),
        allocData(0xFFFF, 0xFFFF, 0xFFFE, 0, true, false),
        allocData(16384, 16384, 32768, 0, false, true), // overflow indicates that the sign changed
        endData()
};

static struct Data addd[] = {
        allocData(100000, 200200, 300200, 0, false, false),
        allocData(0xFFFFFFFF, 1, 0, 0, true, false),
        allocData(1, 0xFFFFFFFF, 0, 0, true, false),
        allocData(0, 0, 0, 0, false, false),
        allocData(0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFE, 0, true, false),
        allocData(0x40000000, 0x40000000, 0x80000000, 0, false, true), // overflow indicates that the sign changed
        endData()
};

static struct Data orb[] = {
        allocData(1, 2, 3, 0, false, false),
        allocData(0xFF, 0, 0xFF, 0, false, false),
        allocData(0, 0xFF, 0xFF, 0, false, false),
        allocData(0, 0, 0, 0, false, false),
        allocData(0xF0, 0x0F, 0xFF, 0, false, false),
        endData()
};

static struct Data orw[] = {
        allocData(1000, 2002, 2042, 0, false, false),
        allocData(0xFFFF, 0, 0xFFFF, 0, false, false),
        allocData(0, 0xFFFF, 0xFFFF, 0, false, false),
        allocData(0, 0, 0, 0, false, false),
        allocData(0xF00F, 0x0FF0, 0xFFFF, 0, false, false),
        endData()
};

static struct Data ord[] = {
        allocData(100000, 200200, 233128, 0, false, false),
        allocData(0xFFFFFFFF, 0, 0xFFFFFFFF, 0, false, false),
        allocData(0, 0xFFFFFFFF, 0xFFFFFFFF, 0, false, false),
        allocData(0, 0, 0, 0, false, false),
        allocData(0xF0F0F0F0, 0x0F0F0F0F, 0xFFFFFFFF, 0, false, false),
        endData()
};

static struct Data adcb[] = {
        allocData(1, 2, 3, 0, false, false),
        allocData(0xFF, 1, 0, 0, true, false),
        allocData(1, 0xFF, 0, 0, true, false),
        allocData(0, 0, 0, 0, false, false),
        allocData(0xFF, 0xFF, 0xFE, 0, true, false),
        allocData(64, 64, 128, 0, false, true), // overflow indicates that the sign changed
        allocData(1, 2, 4, CF, false, false),
        allocData(0xFF, 1, 1, CF, true, false),
        allocData(1, 0xFF, 1, CF, true, false),
        allocData(0, 0, 1, CF, false, false),
        allocData(0xFF, 0xFF, 0xFF, CF, true, false),
        allocData(64, 64, 129, CF, false, true), // overflow indicates that the sign changed
        endData()
};

static struct Data adcw[] = {
        allocData(1000, 2002, 3002, 0, false, false),
        allocData(0xFFFF, 1, 0, 0, true, false),
        allocData(1, 0xFFFF, 0, 0, true, false),
        allocData(0, 0, 0, 0, false, false),
        allocData(0xFFFF, 0xFFFF, 0xFFFE, 0, true, false),
        allocData(16384, 16384, 32768, 0, false, true), // overflow indicates that the sign changed
        allocData(1000, 2002, 3003, CF, false, false),
        allocData(0xFFFF, 1, 1, CF, true, false),
        allocData(1, 0xFFFF, 1, CF, true, false),
        allocData(0, 0, 1, CF, false, false),
        allocData(0xFFFF, 0xFFFF, 0xFFFF, CF, true, false),
        allocData(16384, 16384, 32769, CF, false, true), // overflow indicates that the sign changed
        endData()
};

static struct Data adcd[] = {
        allocData(100000, 200200, 300200, 0, false, false),
        allocData(0xFFFFFFFF, 1, 0, 0, true, false),
        allocData(1, 0xFFFFFFFF, 0, 0, true, false),
        allocData(0, 0, 0, 0, false, false),
        allocData(0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFE, 0, true, false),
        allocData(0x40000000, 0x40000000, 0x80000000, 0, false, true), // overflow indicates that the sign changed
        allocData(100000, 200200, 300201, CF, false, false),
        allocData(0xFFFFFFFF, 1, 1, CF, true, false),
        allocData(1, 0xFFFFFFFF, 1, CF, true, false),
        allocData(0, 0, 1, CF, false, false),
        allocData(0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, CF, true, false),
        allocData(0x40000000, 0x40000000, 0x80000001, CF, false, true), // overflow indicates that the sign changed
        endData()
};

static struct Data sbbb[] = {
        allocData(1, 2, 0xFF, 0, true, false),
        allocData(2, 1, 1, 0, false, false),
        allocData(0xFF, 0, 0xFF, 0, false, false),
        allocData(0, 0xFF, 1, 0, true, false),
        allocData(0, 0, 0, 0, false, false),
        allocData(0x80, 0x10, 0x70, 0, false, true),
        allocData(1, 2, 0xFE, CF, true, false),
        allocData(2, 1, 0, CF, false, false),
        allocData(0xFF, 0, 0xFE, CF, false, false),
        allocData(0, 0xFF, 0, CF, true, false),
        allocData(0, 0, 0xFF, CF, true, false),
        allocData(0x80, 0x10, 0x6F, CF, false, true),
        endData()
};

static struct Data sbbw[] = {
        allocData(0xAAAA, 0xBBBB, 0xEEEF, 0, true, false),
        allocData(0xBBBB, 0xAAAA, 0x1111, 0, false, false),
        allocData(0xFFFF, 0, 0xFFFF, 0, false, false),
        allocData(0, 0xFFFF, 1, 0, true, false),
        allocData(0, 0, 0, 0, false, false),
        allocData(0x8000, 0x1000, 0x7000, 0, false, true),
        allocData(0xAAAA, 0xBBBB, 0xEEEE, CF, true, false),
        allocData(0xBBBB, 0xAAAA, 0x1110, CF, false, false),
        allocData(0xFFFF, 0, 0xFFFE, CF, false, false),
        allocData(0, 0xFFFF, 0, CF, true, false),
        allocData(0, 0, 0xFFFF, CF, true, false),
        allocData(0x8000, 0x1000, 0x6FFF, CF, false, true),
        endData()
};

static struct Data sbbd[] = {
        allocData(0xAAAAAAAA, 0xBBBBBBBB, 0xEEEEEEEF, 0, true, false),
        allocData(0xBBBBBBBB, 0xAAAAAAAA, 0x11111111, 0, false, false),
        allocData(0xFFFFFFFF, 0, 0xFFFFFFFF, 0, false, false),
        allocData(0, 0xFFFFFFFF, 1, 0, true, false),
        allocData(0, 0, 0, 0, false, false),
        allocData(0x80000000, 0x10000000, 0x70000000, 0, false, true),
        allocData(0xAAAAAAAA, 0xBBBBBBBB, 0xEEEEEEEE, CF, true, false),
        allocData(0xBBBBBBBB, 0xAAAAAAAA, 0x11111110, CF, false, false),
        allocData(0xFFFFFFFF, 0, 0xFFFFFFFE, CF, false, false),
        allocData(0, 0xFFFFFFFF, 0, CF, true, false),
        allocData(0, 0, 0xFFFFFFFF, CF, true, false),
        allocData(0x80000000, 0x10000000, 0x6FFFFFFF, CF, false, true),
        endData()
};

static struct Data andb[] = {
        allocData(1, 2, 0, 0, false, false),
        allocData(0xFF, 1, 1, 0, false, false),
        allocData(1, 0xFF, 1, 0, false, false),
        allocData(0, 0, 0, 0, false, false),
        allocData(0xFF, 0xFF, 0xFF, 0, false, false),
        allocData(0xF0, 0x0F, 0, 0, false, false),
        endData()
};

static struct Data andw[] = {
        allocData(0x1000, 0x2000, 0, 0, false, false),
        allocData(0xFFFF, 0x100, 0x100, 0, false, false),
        allocData(0x100, 0xFFFF, 0x100, 0, false, false),
        allocData(0, 0, 0, 0, false, false),
        allocData(0xFFFF, 0xFFFF, 0xFFFF, 0, false, false),
        allocData(0xFF00, 0x00FF, 0, 0, false, false),
        endData()
};

static struct Data andd[] = {
        allocData(0x10000000, 0x20000000, 0, 0, false, false),
        allocData(0xFFFFFFFF, 0x1000000, 0x1000000, 0, false, false),
        allocData(0x1000000, 0xFFFFFFFF, 0x1000000, 0, false, false),
        allocData(0, 0, 0, 0, false, false),
        allocData(0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0, false, false),
        allocData(0xFF00FF00, 0x00FF00FF, 0, 0, false, false),
        endData()
};

static struct Data daa[] = {
    allocDataFlagsWithAF(0x00, 0, 0x00, 0, 0, 0, 0, true, false, false, true, true),
    allocDataFlagsWithAF(0x80, 0, 0x80, 0, 0, 0, true, 0, false, false, true, true),
    allocDataFlagsWithAF(0x03, 0, 0x09, AF|ZF|SF, 0, 0, 0, 0, true, false, true, true), // ZF and SF should be cleared
    allocDataFlagsWithAF(0x06, 0, 0x0C, AF, 0, 0, 0, 0, true, false, true, true),
    allocDataFlagsWithAF(0x07, 0, 0x0D, AF, 0, 0, 0, 0, true, false, true, true),
    allocDataFlagsWithAF(0x59, 0, 0x5F, AF, 0, 0, 0, 0, true, false, true, true),
    allocDataFlagsWithAF(0x60, 0, 0x66, AF, 0, 0, 0, 0, true, false, true, true),
    allocDataFlagsWithAF(0x9F, 0, 0x05, AF, true, 0, 0, 0, true, false, true, true),
    allocDataFlagsWithAF(0xA0, 0, 0x06, AF, true, 0, 0, 0, true, false, true, true),
    allocDataFlagsWithAF(0x03, 0, 0x03, 0, 0, 0, 0, 0, 0, false, true, true),
    allocDataFlagsWithAF(0x06, 0, 0x06, 0, 0, 0, 0, 0, 0, false, true, true),
    allocDataFlagsWithAF(0x03, 0, 0x63, CF, true, 0, 0, 0, 0, false, true, true),
    allocDataFlagsWithAF(0x06, 0, 0x66, CF, true, 0, 0, 0, 0, false, true, true),
    allocDataFlagsWithAF(0x03, 0, 0x69, CF|AF, true, 0, 0, 0, true, false, true, true),
    allocDataFlagsWithAF(0x06, 0, 0x6C, CF|AF, true, 0, 0, 0, true, false, true, true),    
    endData()
};

static struct Data das[] = {
    allocDataFlagsWithAF(0x03, 0, 0xFD, AF, true, 0, true, 0, true, false, true, true),
    allocDataFlagsWithAF(0x06, 0, 0x00, AF, 0, 0, 0, true, true, false, true, true),
    allocDataFlagsWithAF(0x07, 0, 0x01, AF, 0, 0, 0, 0, true, false, true, true),
    allocDataFlagsWithAF(0x59, 0, 0x53, AF, 0, 0, 0, 0, true, false, true, true),
    allocDataFlagsWithAF(0x60, 0, 0x5A, AF, 0, 0, 0, 0, true, false, true, true),
    allocDataFlagsWithAF(0x9F, 0, 0x39, AF, true, 0, 0, 0, true, false, true, true),
    allocDataFlagsWithAF(0xA0, 0, 0x3A, AF, true, 0, 0, 0, true, false, true, true),
    allocDataFlagsWithAF(0x03, 0, 0x03, 0, 0, 0, 0, 0 ,0, false, true, true),
    allocDataFlagsWithAF(0x06, 0, 0x06, 0, 0, 0, 0, 0, 0, false, true, true),
    allocDataFlagsWithAF(0x03, 0, 0xA3, CF, true, 0, true, 0 ,0, false, true, true),
    allocDataFlagsWithAF(0x06, 0, 0xA6, CF, true, 0, true, 0, 0, false, true, true),
    allocDataFlagsWithAF(0x03, 0, 0x9D, CF|AF, true, 0, true, 0, true, false, true, true),
    endData()
};

static struct Data aaa[] = {
    allocDataFlagsWithAF(0x0205, 0, 0x030B, AF, true, 0, 0, 0, true, false, false, false),
    allocDataFlagsWithAF(0x0306, 0, 0x040C, AF, true, 0, 0, 0, true, false, false, false),
    allocDataFlagsWithAF(0x040A, 0, 0x0500, AF, true, 0, 0, 0, true, false, false, false),
    allocDataFlagsWithAF(0x05FA, 0, 0x0700, AF, true, 0, 0, 0, true, false, false, false),
    allocDataFlagsWithAF(0x0205, 0, 0x0205, 0, 0, 0, 0, 0, 0, false, false, false),
    allocDataFlagsWithAF(0x0306, 0, 0x0306, 0, 0, 0, 0, 0, 0, false, false, false),
    allocDataFlagsWithAF(0x040A, 0, 0x0500, 0, true, 0, 0, 0, true, false, false, false),
    allocDataFlagsWithAF(0x05FA, 0, 0x0700, 0, true, 0, 0, 0, true, false, false, false),
    endData()
};

static struct Data aas[] = {
    allocDataFlagsWithAF(0x0205, 0, 0x000F, AF, true, 0, 0, 0, true, false, false, false),
    allocDataFlagsWithAF(0x0306, 0, 0x0200, AF, true, 0, 0, 0, true, false, false, false),
    allocDataFlagsWithAF(0x040A, 0, 0x0304, AF, true, 0, 0, 0, true, false, false, false),
    allocDataFlagsWithAF(0x05FA, 0, 0x0404, AF, true, 0, 0, 0, true, false, false, false),
    allocDataFlagsWithAF(0x0205, 0, 0x0205, 0, false, 0, 0, 0, false, false, false, false),
    allocDataFlagsWithAF(0x0306, 0, 0x0306, 0, false, 0, 0, 0, false, false, false, false),
    allocDataFlagsWithAF(0x040A, 0, 0x0304, 0, true, 0, 0, 0, true, false, false, false),
    allocDataFlagsWithAF(0x05FA, 0, 0x0404, 0, true, 0, 0, 0, true, false, false, false),
    endData()
};

static struct Data aam[] = {
    allocDataNoFlags(0x0547, 10, 0x0701),
    endData()
};

static struct Data aad[] = {
    allocDataNoFlags(0x0407, 10, 0x002F),
    endData()
};

static struct Data subb[] = {
        allocData(1, 2, 0xFF, 0, true, false),
        allocData(2, 1, 1, 0, false, false),
        allocData(0xFF, 0, 0xFF, 0, false, false),
        allocData(0, 0xFF, 1, 0, true, false),
        allocData(0, 0, 0, 0, false, false),
        allocData(0x80, 0x10, 0x70, 0, false, true),
        endData()
};

static struct Data subw[] = {
        allocData(0xAAAA, 0xBBBB, 0xEEEF, 0, true, false),
        allocData(0xBBBB, 0xAAAA, 0x1111, 0, false, false),
        allocData(0xFFFF, 0, 0xFFFF, 0, false, false),
        allocData(0, 0xFFFF, 1, 0, true, false),
        allocData(0, 0, 0, 0, false, false),
        allocData(0x8000, 0x1000, 0x7000, 0, false, true),
        endData()
};

static struct Data subd[] = {
        allocData(0xAAAAAAAA, 0xBBBBBBBB, 0xEEEEEEEF, 0, true, false),
        allocData(0xBBBBBBBB, 0xAAAAAAAA, 0x11111111, 0, false, false),
        allocData(0xFFFFFFFF, 0, 0xFFFFFFFF, 0, false, false),
        allocData(0, 0xFFFFFFFF, 1, 0, true, false),
        allocData(0, 0, 0, 0, false, false),
        allocData(0x80000000, 0x10000000, 0x70000000, 0, false, true),
        endData()
};

static struct Data xorb[] = {
        allocData(1, 2, 3, 0, false, false),
        allocData(0xFF, 0, 0xFF, 0, false, false),
        allocData(0, 0xFF, 0xFF, 0, false, false),
        allocData(0, 0, 0, 0, false, false),
        allocData(0xF0, 0xFF, 0x0F, 0, false, false),
        endData()
};

static struct Data xorw[] = {
        allocData(0x1000, 0x2000, 0x3000, 0, false, false),
        allocData(0xFFFF, 0, 0xFFFF, 0, false, false),
        allocData(0, 0xFFFF, 0xFFFF, 0, false, false),
        allocData(0, 0, 0, 0, false, false),
        allocData(0xF00F, 0xFFFF, 0x0FF0, 0, false, false),
        endData()
};

static struct Data xord[] = {
        allocData(0x100000, 0x200000, 0x300000, 0, false, false),
        allocData(0xFFFFFFFF, 0, 0xFFFFFFFF, 0, false, false),
        allocData(0, 0xFFFFFFFF, 0xFFFFFFFF, 0, false, false),
        allocData(0, 0, 0, 0, false, false),
        allocData(0xFF00FF00, 0x00FFFFFF, 0xFFFF00FF, 0, false, false),
        endData()
};

static struct Data cmpb[] = {
        allocDataFlags(1, 2, true, false, true, false),
        allocDataFlags(2, 1, false, false, false, false),
        allocDataFlags(1, 1, false, false, false, true),
        allocDataFlags(0, 0, false, false, false, true),
        allocDataFlags(0xD0, 0xB0, false, false, false, false),
        allocDataFlags(0xB0, 0xD0, true, false, true, false),
        allocDataFlags(0xB0, 0xB0, false, false, false, true),
        allocDataFlags(0xB0, 1, false, false, true, false),
        allocDataFlags(0x90, 0x30, false, true, false, false),
        allocDataFlags(0x30, 0x90, true, true, true, false),
        endData()
};

static struct Data cmpw[] = {
        allocDataFlags(0xAAAA, 0xBBBB, true, false, true, false),
        allocDataFlags(0xBBBB, 0xAAAA, false, false, false, false),
        allocDataFlags(0xAAAA, 0xAAAA, false, false, false, true),
        allocDataFlags(0, 0, false, false, false, true),
        allocDataFlags(0xD000, 0xB000, false, false, false, false),
        allocDataFlags(0xB000, 0xD000, true, false, true, false),
        allocDataFlags(0xB000, 0xB000, false, false, false, true),
        allocDataFlags(0xB000, 1, false, false, true, false),
        allocDataFlags(0x9000, 0x3000, false, true, false, false),
        allocDataFlags(0x3000, 0x9000, true, true, true, false),
        endData()
};

static struct Data cmpd[] = {
        allocDataFlags(0xAAAA0000, 0xBBBB0000, true, false, true, false),
        allocDataFlags(0xBBBB0000, 0xAAAA0000, false, false, false, false),
        allocDataFlags(0xAAAA0000, 0xAAAA0000, false, false, false, true),
        allocDataFlags(0, 0, false, false, false, true),
        allocDataFlags(0xD0000000, 0xB0000000, false, false, false, false),
        allocDataFlags(0xB0000000, 0xD0000000, true, false, true, false),
        allocDataFlags(0xB0000000, 0xB0000000, false, false, false, true),
        allocDataFlags(0xB0000000, 1, false, false, true, false),
        allocDataFlags(0x90000000, 0x30000000, false, true, false, false),
        allocDataFlags(0x30000000, 0x90000000, true, true, true, false),
        endData()
};

static struct Data testb[] = {
        allocDataFlags(1, 2, false, false, false, true),
        allocDataFlags(2, 1, false, false, false, true),
        allocDataFlags(1, 1, false, false, false, false),
        allocDataFlags(0, 0, false, false, false, true),
        allocDataFlags(0xD0, 0xB0, false, false, true, false),
        allocDataFlags(0xB0, 0xD0, false, false, true, false),
        allocDataFlags(0xB0, 0xB0, false, false, true, false),
        allocDataFlags(0xB0, 1, false, false, false, true),
        allocDataFlags(0x90, 0x30, false, false, false, false),
        allocDataFlags(0x30, 0x90, false, false, false, false),
        endData()
};

static struct Data testw[] = {
        allocDataFlags(0xAAAA, 0xBBBB, false, false, true, false),
        allocDataFlags(0xBBBB, 0xAAAA, false, false, true, false),
        allocDataFlags(0xAAAA, 0xAAAA, false, false, true, false),
        allocDataFlags(0, 0, false, false, false, true),
        allocDataFlags(0xD000, 0xB000, false, false, true, false),
        allocDataFlags(0xB000, 0xD000, false, false, true, false),
        allocDataFlags(0xB000, 0xB000, false, false, true, false),
        allocDataFlags(0xB000, 1, false, false, false, true),
        allocDataFlags(0x9000, 0x3000, false, false, false, false),
        allocDataFlags(0x3000, 0x9000, false, false, false, false),
        endData()
};

static struct Data testd[] = {
        allocDataFlags(0xAAAA0000, 0xBBBB0000, false, false, true, false),
        allocDataFlags(0xBBBB0000, 0xAAAA0000, false, false, true, false),
        allocDataFlags(0xAAAA0000, 0xAAAA0000, false, false, true, false),
        allocDataFlags(0, 0, false, false, false, true),
        allocDataFlags(0xD0000000, 0xB0000000, false, false, true, false),
        allocDataFlags(0xB0000000, 0xD0000000, false, false, true, false),
        allocDataFlags(0xB0000000, 0xB0000000, false, false, true, false),
        allocDataFlags(0xB0000000, 1, false, false, false, true),
        allocDataFlags(0x90000000, 0x30000000, false, false, false, false),
        allocDataFlags(0x30000000, 0x90000000, false, false, false, false),
        endData()
};

static struct Data incb[] = {
        allocData(0, 0, 1, 0, false, false),
        allocData(0, 0, 1, CF, true, false), // it should keep the previous carry flag
        allocData(0x80, 0, 0x81, 0, false, false),
        allocData(0x7F, 0, 0x80, 0, false, true),
        endData()
};

static struct Data incw[] = {
        allocData(0, 0, 1, 0, false, false),
        allocData(0, 0, 1, CF, true, false), // it should keep the previous carry flag
        allocData(0x8000, 0, 0x8001, 0, false, false),
        allocData(0x7FFF, 0, 0x8000, 0, false, true),
        allocData(0xFFFF, 0, 0, 0, false, false), // carry flag is not set
        endData()
};

static struct Data incd[] = {
        allocData(0, 0, 1, 0, false, false),
        allocData(0, 0, 1, CF, true, false), // it should keep the previous carry flag
        allocData(0x80000000, 0, 0x80000001, 0, false, false),
        allocData(0x7FFFFFFF, 0, 0x80000000, 0, false, true),
        allocData(0xFFFFFFFF, 0, 0, 0, false, false), // carry flag is not set
        endData()
};

static struct Data decb[] = {
        allocData(2, 0, 1, 0, false, false),
        allocData(1, 0, 0, CF, true, false), // it should keep the previous carry flag
        allocData(0x80, 0, 0x7F, 0, false, true),
        allocData(0, 0, 0xFF, 0, false, false),
        endData()
};

static struct Data decw[] = {
        allocData(2, 0, 1, 0, false, false),
        allocData(1, 0, 0, CF, true, false), // it should keep the previous carry flag
        allocData(0x8000, 0, 0x7FFF, 0, false, true),
        allocData(0, 0, 0xFFFF, 0, false, false),
        endData()
};

static struct Data decd[] = {
        allocData(2, 0, 1, 0, false, false),
        allocData(1, 0, 0, CF, true, false), // it should keep the previous carry flag
        allocData(0x80000000, 0, 0x7FFFFFFF, 0, false, true),
        allocData(0, 0, 0xFFFFFFFF, 0, false, false),
        endData()
};

static struct Data imulw[] = {
        allocDataConst(0, 2, 4, 2, 16, 0, false, false),
        allocDataConst(0, 0xFFFE, 0xFFFC, 2, 16, 0, false, false), // -2 * 2 = -4
        allocDataConst(0, 0xFFFE, 4, 0xFFFE, 16, CF|OF, false, false), // -2 * -2 = 4 (also, make sure it clears the flags)
        allocDataConst(0, 300, 0x5F90, 300, 16, 0, true, true), // 300 x 300 = 0x15F90
        allocDataConst(0, (U32)(-300), 0xA070, 300, 16, 0, true, true),
        endData()
};

static struct Data imuld[] = {
        allocDataConst(0, 2, 4, 2, 32, 0, false, false),
        allocDataConst(0, 0xFFFFFFFE, 0xFFFFFFFC, 2, 32, 0, false, false), // -2 * 2 = -4
        allocDataConst(0, 0xFFFFFFFE, 4, 0xFFFFFFFE, 32, CF|OF, false, false), // -2 * -2 = 4 (also, make sure it clears the flags)
        allocDataConst(0, 300000, 0xF08EB000, 400000, 32, 0, true, true), // = 1BF08EB000
        allocDataConst(0, (U32)(-300000), 0x0F715000, 400000, 32, 0, true, true),
        endData()
};

static struct Data imulw_s8[] = {
        allocDataConst(0, 2, 4, 2, 8, 0, false, false),
        allocDataConst(0, 0xFFFE, 0xFFFC, 2, 8, 0, false, false), // -2 * 2 = -4
        allocDataConst(0, 0xFFFE, 4, 0xFE, 8, CF|OF, false, false), // -2 * -2 = 4 (also, make sure it clears the flags)
        allocDataConst(0, 3000, 0xD048, 127, 8, 0, true, true), // 3000 x 127 = 0x5D048
        allocDataConst(0, (U32)(-3000), 0x2FB8, 127, 8, 0, true, true),
        endData()
};

static struct Data imuld_s8[] = {
        allocDataConst(0, 2, 4, 2, 8, 0, false, false),
        allocDataConst(0, 0xFFFFFFFE, 0xFFFFFFFC, 2, 8, 0, false, false), // -2 * 2 = -4
        allocDataConst(0, 0xFFFFFFFE, 4, 0xFE, 8, CF|OF, false, false), // -2 * -2 = 4 (also, make sure it clears the flags)
        allocDataConst(0, 300000000, 0xDEEFDD00, 127, 8, 0, true, true), // = 8DEEFDD00
        allocDataConst(0, (U32)(-300000000), 0x21102300, 127, 8, 0, true, true),
        endData()
};

static struct Data xchgb[] = {
        allocDatavar2(2, 1, 1, 2),
        allocDatavar2(0, 0, 0, 0),
        allocDatavar2(0xAB, 0xFC, 0xFC, 0xAB),
        endData()
};

static struct Data xchgw[] = {
        allocDatavar2(2, 1, 1, 2),
        allocDatavar2(0, 0, 0, 0),
        allocDatavar2(0xAB38, 0xFC15, 0xFC15, 0xAB38),
        endData()
};

static struct Data xchgd[] = {
        allocDatavar2(2, 1, 1, 2),
        allocDatavar2(0, 0, 0, 0),
        allocDatavar2(0xAB38, 0xFC150146, 0xFC150146, 0xAB38),
        endData()
};

static struct Data movb[] = {
        allocData(0, 1, 1, 0, false, false),
        allocData(0, 0, 0, 0, false, false),
        allocData(0, 0xFF, 0xFF, 0, false, false),
        allocData(0, 0x7F, 0x7F, 0, false, false),
        endData()
};

static struct Data movw[] = {
        allocData(0, 1, 1, 0, false, false),
        allocData(0, 0, 0, 0, false, false),
        allocData(0, 0xFFFF, 0xFFFF, 0, false, false),
        allocData(0, 0x7FFF, 0x7FFF, 0, false, false),
        endData()
};

static struct Data movd[] = {
        allocData(0, 1, 1, 0, false, false),
        allocData(0, 0, 0, 0, false, false),
        allocData(0, 0xFFFF, 0xFFFF, 0, false, false),
        allocData(0, 0x7FFF, 0x7FFF, 0, false, false),
        allocData(0, 0xFFFFFFFF, 0xFFFFFFFF, 0, false, false),
        allocData(0, 0x7FFFFFFF, 0x7FFFFFFF, 0, false, false),
        allocData(0, 0x08080808, 0x08080808, 0, false, false),
        endData()
};

static struct Data cbw[] = {
        allocData(0x1234, 0, 0x0034, 0, false, false),
        allocData(0x12FE, 0, 0xFFFE, 0, false, false),
        endData()
};

static struct Data cwde[] = {
        allocData(0x12345678, 0, 0x5678, 0, false, false),
        allocData(0x1234FFFE, 0, 0xFFFFFFFE, 0, false, false),
        endData()
};

static struct Data cwd[] = {
        allocDatavar2(0x1234, 0x1234, 0x1234, 0),
        allocDatavar2(0xFFFE, 0x1234, 0xFFFE, 0xFFFF),
        endData()
};

static struct Data cdq[] = {
        allocDatavar2(0x12345678, 0x12345678, 0x12345678, 0),
        allocDatavar2(0xFFFFFFFE, 0x12345678, 0xFFFFFFFE, 0xFFFFFFFF),
        endData()
};

static struct Data sahf[] = {
        allocDatavar2(0x12345600, 0x0000FF00, 0x123456D5, 0x0000FF00),
        allocDatavar2(0xFFFFFFFF, 0x00000000, 0xFFFFFF2A, 0x00000000),
        endData()
};

static struct Data lahf[] = {
        allocDatavar2(0x123456FF, 0x00000000, 0x123456FF, 0x0000D700),
        allocDatavar2(0xFFFFFF02, 0xFFFFFFFF, 0xFFFFFF02, 0xFFFF02FF),
        endData()
};

/*
__asm clc;
__asm mov al, 1;
__asm rol al, 0;
__asm setc bl;
al=1, bl=0

__asm clc;
__asm mov al, 1;
__asm rol al, 8;
__asm setc bl;
al=1, bl=1
*/

static struct Data rolb[] = {
        allocData(0x40, 1, 0x80, 0, false, true),
        allocData(0x01, 1, 0x02, 0, false, false),
        allocData(0x80, 1, 0x01, 0, true, true),
        allocDataNoOF(0x30, 4, 0x03, 0, true),
        allocDataNoOF(0x30, 12, 0x03, 0, true),
        allocData(0x01, 0, 0x01, 0, false, false),
        allocDataNoOF(0x01, 8, 0x01, 0, true),
        allocDataNoOF(0x80, 8, 0x80, 0, false),
        allocDataNoOF(0x01, 9, 0x02, 0, false),
        allocDataNoOF(0x01, 32, 0x01, 0, false),
        endData()
};

static struct Data rolb_1[] = {
        allocData(0x40, 1, 0x80, 0, false, true),
        allocData(0x01, 1, 0x02, 0, false, false),
        allocData(0x80, 1, 0x01, 0, true, true),
        endData()
};

static struct Data rorb[] = {
        allocData(0x02, 1, 0x01, 0, false, false),
        allocData(0x80, 1, 0x40, 0, false, true),
        allocData(0x01, 1, 0x80, 0, true, true),
        allocDataNoOF(0x03, 4, 0x30, 0, false),
        allocDataNoOF(0x03, 12, 0x30, 0, false),
        allocData(0x01, 0, 0x01, 0, false, false),
        allocDataNoOF(0x01, 8, 0x01, 0, false),
        allocDataNoOF(0x80, 8, 0x80, 0, true),
        allocDataNoOF(0x80, 9, 0x40, 0, false),
        allocDataNoOF(0x80, 32, 0x80, 0, false),
        endData()
};

static struct Data rorb_1[] = {
        allocData(0x02, 1, 0x01, 0, false, false),
        allocData(0x80, 1, 0x40, 0, false, true),
        allocData(0x01, 1, 0x80, 0, true, true),
        endData()
};

static struct Data rclb[] = {
        allocData(0x40, 1, 0x80, 0, false, true),
        allocData(0x01, 1, 0x02, 0, false, false),
        allocData(0x80, 1, 0x00, 0, true, true),
        allocDataNoOF(0x30, 5, 0x03, 0, false),
        allocDataNoOF(0x30, 14, 0x03, 0, false),
        allocData(0x01, 0, 0x01, 0, false, false),
        allocDataNoOF(0x01, 9, 0x01, 0, false),
        allocDataNoOF(0x80, 9, 0x80, 0, false),
        allocDataNoOF(0x01, 10, 0x02, 0, false),
        allocDataNoOF(0x01, 32, 0x01, 0, false),
        allocData(0x00, 1, 0x01, CF, false, false),
        allocDataNoOF(0x80, 2, 0x03, CF, false),
        endData()
};

static struct Data rclb_1[] = {
        allocData(0x40, 1, 0x80, 0, false, true),
        allocData(0x01, 1, 0x02, 0, false, false),
        allocData(0x80, 1, 0x00, 0, true, true),
        allocData(0x00, 1, 0x01, CF, false, false),
        endData()
};

static struct Data rcrb[] = {
        allocData(0x02, 1, 0x01, 0, false, false),
        allocData(0x80, 1, 0x40, 0, false, true),
        allocData(0x01, 1, 0x00, 0, true, false),
        allocDataNoOF(0x03, 5, 0x30, 0, false),
        allocDataNoOF(0x03, 14, 0x30, 0, false),
        allocData(0x01, 0, 0x01, 0, false, false),
        allocDataNoOF(0x01, 9, 0x01, 0, false),
        allocDataNoOF(0x80, 9, 0x80, 0, false),
        allocDataNoOF(0x80, 10, 0x40, 0, false),
        allocDataNoOF(0x80, 32, 0x80, 0, false),
        allocData(0x00, 1, 0x80, CF, false, true),
        allocDataNoOF(0x01, 2, 0xC0, CF, false),
        endData()
};

static struct Data rcrb_1[] = {
        allocData(0x02, 1, 0x01, 0, false, false),
        allocData(0x80, 1, 0x40, 0, false, true),
        allocData(0x01, 1, 0x00, 0, true, false),
        allocData(0x00, 1, 0x80, CF, false, true),
        endData()
};

static struct Data shlb[] = {
        allocData(0x40, 1, 0x80, 0, false, true),
        allocData(0x01, 1, 0x02, 0, false, false),
        allocData(0x80, 1, 0x00, 0, true, true),
        allocData(0x03, 4, 0x30, 0, false, false),
        allocData(0x03, 12, 0x00, 0, false, false),
        allocData(0x01, 0, 0x01, 0, false, false),
        allocData(0x01, 32, 0x01, 0, false, false),
        endData()
};

static struct Data shlb_1[] = {
        allocData(0x40, 1, 0x80, 0, false, true),
        allocData(0x01, 1, 0x02, 0, false, false),
        allocData(0x80, 1, 0x00, 0, true, true),
        endData()
};

static struct Data shrb[] = {
        allocData(0x40, 1, 0x20, 0, false, false),
        allocData(0x02, 1, 0x01, 0, false, false),
        allocData(0x80, 1, 0x40, 0, false, true),
        allocData(0x01, 1, 0x00, 0, true, false),
        allocData(0x30, 4, 0x03, 0, false, false),
        allocData(0x03, 12, 0x00, 0, false, false),
        allocData(0x01, 0, 0x01, 0, false, false),
        allocData(0x01, 32, 0x01, 0, false, false),
        endData()
};

static struct Data shrb_1[] = {
        allocData(0x40, 1, 0x20, 0, false, false),
        allocData(0x02, 1, 0x01, 0, false, false),
        allocData(0x80, 1, 0x40, 0, false, true),
        allocData(0x01, 1, 0x00, 0, true, false),
        endData()
};

static struct Data sarb[] = {
        allocData(0x40, 1, 0x20, 0, false, false),
        allocData(0x02, 1, 0x01, 0, false, false),
        allocData(0x80, 1, 0xC0, 0, false, false),
        allocData(0xC0, 7, 0xFF, 0, true, false),
        allocData(0x01, 1, 0x00, 0, true, false),
        allocData(0x30, 4, 0x03, 0, false, false),
        allocData(0x03, 12, 0x00, 0, false, false),
        allocData(0x01, 0, 0x01, 0, false, false),
        allocData(0x01, 32, 0x01, 0, false, false),
        endData()
};

static struct Data sarb_1[] = {
        allocData(0x40, 1, 0x20, 0, false, false),
        allocData(0x02, 1, 0x01, 0, false, false),
        allocData(0x80, 1, 0xC0, 0, false, false),
        endData()
};

static struct Data rolw[] = {
        allocData(0x4000, 1, 0x8000, 0, false, true),
        allocData(0x0001, 1, 0x0002, 0, false, false),
        allocData(0x8000, 1, 0x0001, 0, true, true),
        allocDataNoOF(0x3000, 8, 0x0030, 0, false),
        allocDataNoOF(0x3000, 12, 0x0300, 0, false),
        allocData(0x0101, 0, 0x0101, 0, false, false),
        allocDataNoOF(0x0101, 16, 0x0101, 0, true),
        allocDataNoOF(0x8080, 16, 0x8080, 0, false),
        allocDataNoOF(0x0101, 17, 0x0202, 0, false),
        allocDataNoOF(0x0101, 32, 0x0101, 0, false),
        endData()
};

static struct Data rolw_1[] = {
        allocData(0x4000, 1, 0x8000, 0, false, true),
        allocData(0x0001, 1, 0x0002, 0, false, false),
        allocData(0x8000, 1, 0x0001, 0, true, true),
        endData()
};

static struct Data rorw[] = {
        allocData(0x0002, 1, 0x0001, 0, false, false),
        allocData(0x8000, 1, 0x4000, 0, false, true),
        allocData(0x0001, 1, 0x8000, 0, true, true),
        allocDataNoOF(0x0300, 8, 0x0003, 0, false),
        allocDataNoOF(0x0300, 24, 0x003, 0, false),
        allocData(0x0101, 0, 0x0101, 0, false, false),
        allocDataNoOF(0x0101, 16, 0x0101, 0, false),
        allocDataNoOF(0x8000, 16, 0x8000, 0, true),
        allocDataNoOF(0x8080, 17, 0x4040, 0, false),
        allocDataNoOF(0x8080, 32, 0x8080, 0, false),
        endData()
};

static struct Data rorw_1[] = {
        allocData(0x0002, 1, 0x0001, 0, false, false),
        allocData(0x8000, 1, 0x4000, 0, false, true),
        allocData(0x0001, 1, 0x8000, 0, true, true),
        endData()
};

static struct Data rclw[] = {
        allocData(0x4000, 1, 0x8000, 0, false, true),
        allocData(0x0101, 1, 0x0202, 0, false, false),
        allocData(0x8000, 1, 0x0000, 0, true, true),
        allocDataNoOF(0x3000, 13, 0x0300, 0, false),
        allocDataNoOF(0x3000, 30, 0x0300, 0, false),
        allocData(0x0101, 0, 0x0101, 0, false, false),
        allocDataNoOF(0x0103, 17, 0x0103, 0, false),
        allocDataNoOF(0x8070, 17, 0x8070, 0, false),
        allocDataNoOF(0x0101, 18, 0x0202, 0, false),
        allocDataNoOF(0x0102, 32, 0x0102, 0, false),
        allocData(0x0000, 1, 0x0001, CF, false, false),
        allocDataNoOF(0x8000, 2, 0x0003, CF, false),
        endData()
};

static struct Data rclw_1[] = {
        allocData(0x4000, 1, 0x8000, 0, false, true),
        allocData(0x0101, 1, 0x0202, 0, false, false),
        allocData(0x8000, 1, 0x0000, 0, true, true),
        allocData(0x0000, 1, 0x0001, CF, false, false),
        endData()
};

static struct Data rcrw[] = {
        allocData(0x0202, 1, 0x0101, 0, false, false),
        allocData(0x8080, 1, 0x4040, 0, false, true),
        allocData(0x0001, 1, 0x0000, 0, true, false),
        allocDataNoOF(0x03, 5, 0x3000, 0, false),
        allocDataNoOF(0x03, 22, 0x3000, 0, false),
        allocData(0x0100, 0, 0x0100, 0, false, false),
        allocDataNoOF(0x0100, 17, 0x0100, 0, false),
        allocDataNoOF(0x8000, 17, 0x8000, 0, false),
        allocDataNoOF(0x8000, 18, 0x4000, 0, false),
        allocDataNoOF(0x8070, 32, 0x8070, 0, false),
        allocData(0x0000, 1, 0x8000, CF, false, true),
        allocDataNoOF(0x0001, 2, 0xC000, CF, false),
        endData()
};

static struct Data rcrw_1[] = {
        allocData(0x0202, 1, 0x0101, 0, false, false),
        allocData(0x8080, 1, 0x4040, 0, false, true),
        allocData(0x0001, 1, 0x0000, 0, true, false),
        allocData(0x0000, 1, 0x8000, CF, false, true),
        endData()
};

static struct Data shlw[] = {
        allocData(0x4040, 1, 0x8080, 0, false, true),
        allocData(0x0101, 1, 0x0202, 0, false, false),
        allocData(0x8000, 1, 0x00, 0, true, true),
        allocData(0x0003, 8, 0x0300, 0, false, false),
        allocData(0x0003, 20, 0x00, 0, false, false),
        allocData(0x0102, 0, 0x0102, 0, false, false),
        allocData(0x0102, 32, 0x0102, 0, false, false),
        endData()
};

static struct Data shlw_1[] = {
        allocData(0x4040, 1, 0x8080, 0, false, true),
        allocData(0x0101, 1, 0x0202, 0, false, false),
        allocData(0x8000, 1, 0x00, 0, true, true),
        endData()
};

static struct Data shrw[] = {
        allocData(0x4020, 1, 0x2010, 0, false, false),
        allocData(0x0802, 1, 0x0401, 0, false, false),
        allocData(0x8000, 1, 0x4000, 0, false, true),
        allocData(0x0001, 1, 0x0000, 0, true, false),
        allocData(0x3000, 12, 0x0003, 0, false, false),
        allocData(0x0300, 20, 0x0000, 0, false, false),
        allocData(0x0102, 0, 0x0102, 0, false, false),
        allocData(0x0102, 32, 0x0102, 0, false, false),
        endData()
};

static struct Data shrw_1[] = {
        allocData(0x4020, 1, 0x2010, 0, false, false),
        allocData(0x0802, 1, 0x0401, 0, false, false),
        allocData(0x8000, 1, 0x4000, 0, false, true),
        allocData(0x0001, 1, 0x0000, 0, true, false),
        endData()
};

static struct Data sarw[] = {
        allocData(0x4020, 1, 0x2010, 0, false, false),
        allocData(0x0204, 1, 0x0102, 0, false, false),
        allocData(0x8000, 1, 0xC000, 0, false, false),
        allocData(0xC000, 15, 0xFFFF, 0, true, false),
        allocData(0x0001, 1, 0x0000, 0, true, false),
        allocData(0x3000, 12, 0x0003, 0, false, false),
        allocData(0x3000, 28, 0x0000, 0, false, false),
        allocData(0x0102, 0, 0x0102, 0, false, false),
        allocData(0x0102, 32, 0x0102, 0, false, false),
        endData()
};

static struct Data sarw_1[] = {
        allocData(0x4020, 1, 0x2010, 0, false, false),
        allocData(0x0204, 1, 0x0102, 0, false, false),
        allocData(0x8000, 1, 0xC000, 0, false, false),
        allocData(0x0001, 1, 0x0000, 0, true, false),
        endData()
};

static struct Data rold[] = {
        allocData(0x40000000, 1, 0x80000000, 0, false, true),
        allocData(0x00000001, 1, 0x00000002, 0, false, false),
        allocData(0x80000000, 1, 0x00000001, 0, true, true),
        allocDataNoOF(0x30000000, 24, 0x00300000, 0, false),
        allocData(0x01010101, 0, 0x01010101, 0, false, false),
        allocDataNoOF(0x01010101, 32, 0x01010101, 0, false),
        allocDataNoOF(0x80808080, 32, 0x80808080, 0, false),
        allocDataNoOF(0x01010101, 33, 0x02020202, 0, false),
        endData()
};

static struct Data rold_1[] = {
        allocData(0x40000000, 1, 0x80000000, 0, false, true),
        allocData(0x00000001, 1, 0x00000002, 0, false, false),
        allocData(0x80000000, 1, 0x00000001, 0, true, true),
        endData()
};

static struct Data rord[] = {
        allocData(0x00020000, 1, 0x00010000, 0, false, false),
        allocData(0x80000000, 1, 0x40000000, 0, false, true),
        allocData(0x00000001, 1, 0x80000000, 0, true, true),
        allocDataNoOF(0x00000003, 8, 0x03000000, 0, false),
        allocDataNoOF(0x03000000, 40, 0x00030000, 0, false),
        allocData(0x01020304, 0, 0x01020304, 0, false, false),
        allocDataNoOF(0x01020304, 32, 0x01020304, 0, false),
        allocDataNoOF(0x80000000, 32, 0x80000000, 0, false),
        allocDataNoOF(0x80808080, 33, 0x40404040, 0, false),
        endData()
};

static struct Data rord_1[] = {
        allocData(0x00020000, 1, 0x00010000, 0, false, false),
        allocData(0x80000000, 1, 0x40000000, 0, false, true),
        allocData(0x00000001, 1, 0x80000000, 0, true, true),
        endData()
};

static struct Data rcld[] = {
        allocData(0x40000000, 1, 0x80000000, 0, false, true),
        allocData(0x01010101, 1, 0x02020202, 0, false, false),
        allocData(0x80000000, 1, 0x00000000, 0, true, true),
        allocDataNoOF(0x30000000, 29, 0x03000000, 0, false),
        allocDataNoOF(0x30000000, 61, 0x03000000, 0, false),
        allocData(0x01020304, 0, 0x01020304, 0, false, false),
        allocDataNoOF(0x01010101, 33, 0x02020202, 0, false),
        allocData(0x00000000, 1, 0x00000001, CF, false, false),
        allocDataNoOF(0x80000000, 2, 0x00000003, CF, false),
        endData()
};

static struct Data rcld_1[] = {
        allocData(0x40000000, 1, 0x80000000, 0, false, true),
        allocData(0x01010101, 1, 0x02020202, 0, false, false),
        allocData(0x80000000, 1, 0x00000000, 0, true, true),
        allocData(0x00000000, 1, 0x00000001, CF, false, false),
        endData()
};

static struct Data rcrd[] = {
        allocData(0x02020202, 1, 0x01010101, 0, false, false),
        allocData(0x80808080, 1, 0x40404040, 0, false, true),
        allocData(0x00000001, 1, 0x00000000, 0, true, false),
        allocDataNoOF(0x00000003, 5, 0x30000000, 0, false),
        allocDataNoOF(0x00000003, 37, 0x30000000, 0, false),
        allocData(0x01020304, 0, 0x01020304, 0, false, false),
        allocData(0x00000000, 1, 0x80000000, CF, false, true),
        allocDataNoOF(0x00000001, 2, 0xC0000000, CF, false),
        endData()
};

static struct Data rcrd_1[] = {
        allocData(0x02020202, 1, 0x01010101, 0, false, false),
        allocData(0x80808080, 1, 0x40404040, 0, false, true),
        allocData(0x00000001, 1, 0x00000000, 0, true, false),
        allocData(0x00000000, 1, 0x80000000, CF, false, true),
        endData()
    };

static struct Data shld[] = {
        allocData(0x40404040, 1, 0x80808080, 0, false, true),
        allocData(0x01010101, 1, 0x02020202, 0, false, false),
        allocData(0x80000000, 1, 0x00000000, 0, true, true),
        allocData(0x00000003, 16, 0x00030000, 0, false, false),
        allocData(0x00030000, 20, 0x00000000, 0, false, false),
        allocData(0x01020304, 0, 0x01020304, 0, false, false),
        allocData(0x01020304, 32, 0x01020304, 0, false, false),
        endData()
};

static struct Data shld_1[] = {
        allocData(0x40404040, 1, 0x80808080, 0, false, true),
        allocData(0x01010101, 1, 0x02020202, 0, false, false),
        allocData(0x80000000, 1, 0x00000000, 0, true, true),
        endData()
};

static struct Data shrd[] = {
        allocData(0x00804020, 1, 0x00402010, 0, false, false),
        allocData(0x80000000, 1, 0x40000000, 0, false, true),
        allocData(0x00000001, 1, 0x00000000, 0, true, false),
        allocData(0x30000000, 28, 0x00000003, 0, false, false),
        allocData(0x30000000, 30, 0x00000000, 0, true, false),
        allocData(0x01020304, 0, 0x01020304, 0, false, false),
        allocData(0x01020304, 32, 0x01020304, 0, false, false),
        endData()
};

static struct Data shrd_1[] = {
        allocData(0x00804020, 1, 0x00402010, 0, false, false),
        allocData(0x80000000, 1, 0x40000000, 0, false, true),
        allocData(0x00000001, 1, 0x00000000, 0, true, false),
        endData()
};

static struct Data sard[] = {
        allocData(0x00804020, 1, 0x00402010, 0, false, false),
        allocData(0x80000000, 1, 0xC0000000, 0, false, false),
        allocData(0xC0000000, 31, 0xFFFFFFFF, 0, true, false),
        allocData(0x00000001, 1, 0x00000000, 0, true, false),
        allocData(0x3000, 12, 0x0003, 0, false, false),
        allocData(0x30000000, 28, 0x00000003, 0, false, false),
        allocData(0x30000000, 30, 0x00000000, 0, true, false),
        allocData(0x01020304, 0, 0x01020304, 0, false, false),
        allocData(0x01020304, 32, 0x01020304, 0, false, false),
        endData()
};

static struct Data sard_1[] = {
        allocData(0x00804020, 1, 0x00402010, 0, false, false),
        allocData(0x80000000, 1, 0xC0000000, 0, false, false),
        allocData(0x00000001, 1, 0x00000000, 0, true, false),
        endData()
};

static struct Data salc[] = {
        allocData(10, 0, 0, 0, false, false),
        allocData(10, 0, 0xFF, CF, true, false),
        endData()
};

static struct Data cmc[] = {
        allocData(0, 0, 0, 0, true, false),
        allocData(0, 0, 0, CF, false, false),
        endData()
};

static struct Data notb[] = {
        allocData(0, 0, 0xFF, 0, false, false),
        allocData(0x0F, 0, 0xF0, 0, false, false),
        allocData(0xF0, 0, 0x0F, 0, false, false),
        endData()
};

static struct Data notw[] = {
        allocData(0, 0, 0xFFFF, 0, false, false),
        allocData(0xF0F, 0, 0xF0F0, 0, false, false),
        allocData(0xF0F0, 0, 0x0F0F, 0, false, false),
        endData()
};

static struct Data notd[] = {
        allocData(0, 0, 0xFFFFFFFF, 0, false, false),
        allocData(0x0F0F0F0F, 0, 0xF0F0F0F0, 0, false, false),
        allocData(0xF0F0F0F0, 0, 0x0F0F0F0F, 0, false, false),
        endData()
};

static struct Data negb[] = {
        allocData(0, 0, 0x0, 0, false, false),
        allocData(4, 0, ((S8)-4) & 0xFF, 0, true, false),
        endData()
};

static struct Data negw[] = {
        allocData(0, 0, 0x0, 0, false, false),
        allocData(2045, 0, ((S16)-2045) & 0xFFFF, 0, true, false),
        endData()
};

static struct Data negd[] = {
        allocData(0, 0, 0x0, 0, false, false),
        allocData(20458512, 0, (U32)(-20458512), 0, true, false),
        endData()
};

static struct Data mulAl[] = {
        allocData(2, 2, 4, 0, false, false),
        allocData(0, 0, 0, 0, false, false),
        allocData(0x20, 0x10, 0x200, 0, true, true),
        endData()
};

static struct Data mulAx[] = {
        allocDataConstvar2(0, 2, 4, 0, false, false, 2, 0),
        allocDataConstvar2(0, 0, 0, 0, false, false, 0, 0),
        allocDataConstvar2(0, 0x2001, 0x0010, 0, true, true, 0x10, 0x0002),
        allocDataConstvar2(0, 0x2001, 0x1000, 0, true, true, 0x1000, 0x0200),
        endData()
};

static struct Data mulEax[] = {
        allocDataConstvar2(0, 2, 4, 0, false, false, 2, 0),
        allocDataConstvar2(0, 0, 0, 0, false, false, 0, 0),
        allocDataConstvar2(0, 0x20000001, 0x00000010, 0, true, true, 0x10, 0x00000002),
        allocDataConstvar2(0, 0x20000001, 0x00010000, 0, true, true, 0x00010000, 0x00002000),
        endData()
};

static struct Data imulAl[] = {
        allocData(2, 2, 4, 0, false, false),
        allocData(0xFA, 2, 0xFFF4, 0, false, false), // -6 x 2 = -12
        allocData(0xFA, 0x9C, 600, 0, true, true), // -6 x -100 = 600
        allocData(0, 0xFF, 0, 0, false, false),
        endData()
};

static struct Data imulAx[] = {
        allocDataConstvar2(0, 2, 4, 0, false, false, 2, 0),
        allocDataConstvar2(0, 0xFFFA, 0xFFF4, 0, false, false, 2, 0xFFFF), // -6 x 2 = -12
        allocDataConstvar2(0, ((S16)-600) & 0xFFFF, 0x5780, 0, true, true, 30000, 0xFEED), // -600 x 30000 = -18000000
        allocDataConstvar2(0, 0xFFFA, 600, 0, false, false, 0xFF9C, 0), // -6 x -100 = 600
        endData()
};

static struct Data imulEax[] = {
        allocDataConstvar2(0, 2, 4, 0, false, false, 2, 0),
        allocDataConstvar2(0, 0xFFFFFFFA, 0xFFFFFFF4, 0, false, false, 2, 0xFFFFFFFF), // -6 x 2 = -12
        allocDataConstvar2(0, (U32)(-60000), 0x1729f800, 0, true, true, 3000000, 0xFFFFFFD6), // -60000 x 3000000 = -180000000000
        endData()
};

static struct Data divAl[] = {
        allocData(10, 3, 0x0103, 0, false, false),
        allocData(1003, 200, 0x0305, 0, false, false),
        endData()
};

static struct Data divAx[] = {
        allocDataConstvar2(0, 10, 0x0003, 0, false, false, 3, 0x0001),
        allocDataConstvar2(0xCB, 0x8512, 4445, 0, false, false, 3000, 2874), // 13337874 / 3000 = 4445 r 2874
        endData()
};

static struct Data divEax[] = {
        allocDataConstvar2(0, 10, 0x0003, 0, false, false, 3, 0x0001),
        allocDataConstvar2(0xCB, 0x85121234, 0xB2D, 0, false, false, 0x12345678, 0x1227B71C), // 874110915124 / 305419896 = 2861 r 304592668
        endData()
};

static struct Data idivAl[] = {
        allocData(10, 3, 0x0103, 0, false, false),
        allocData(10, ((S8)-3) & 0xFF, 0x01FD, 0, false, false),
        allocData(((S16)-1003) & 0xFFFF, ((S8)-100) & 0xFF, 0xFD0A, 0, false, false), // -3 rem, 10 quo
        endData()
};

static struct Data idivAx[] = {
        allocDataConstvar2(0, 10, 3, 0, false, false, 3, 1),
        allocDataConstvar2(0xCB, 0x8512, 4445, 0, false, false, 3000, 2874), // 13337874 / 3000 = 4445 r 2874
        allocDataConstvar2(0xFF34, 0x7AEE, ((S16)-4445) & 0xFFFF, 0, false, false, 3000, ((S16)-2874) & 0xFFFF), // -13337874 / 3000 = -4445 r -2874
        endData()
};

static struct Data idivEax[] = {
        allocDataConstvar2(0, 10, 3, 0, false, false, 3, 1),
        allocDataConstvar2(0xCB, 0x85121234, 0xB2D, 0, false, false, 0x12345678, 0x1227B71C), // 874110915124 / 305419896 = 2861 r 304592668
        allocDataConstvar2(0xFFFFFF34, 0x7AEDEDCC, 0xFFFFF4D3, 0, false, false, 0x12345678, 0xEDD848E4), // -874110915124 / 305419896 = -2861 r -304592668
        endData()
};

static struct Data clc[] = {
        allocData(0, 0, 0, 0, false, false),
        allocData(0, 0, 0, CF, false, false),
        endData()
};

static struct Data stc[] = {
        allocData(0, 0, 0, 0, true, false),
        allocData(0, 0, 0, CF, true, false),
        endData()
};

static struct Data btw[] = {
        allocData(0xFFFD, 1, 0xFFFD, CF, false, false),
        allocData(0x10, 4, 0x10, 0, true, false),
        allocData(0xFFFD, 33, 0xFFFD, CF, false, false),
        allocData(0x10, 36, 0x10, 0, true, false),
        endData()
};

static struct Data btd[] = {
        allocData(0xFFFDFFFF, 17, 0xFFFDFFFF, CF, false, false),
        allocData(0x100000, 20, 0x100000, 0, true, false),
        allocData(0xFFFDFFFF, 81, 0xFFFDFFFF, CF, false, false),
        allocData(0x100000, 84, 0x100000, 0, true, false),
        endData()
};

static struct Data btsw[] = {
        allocData(0xFFFD, 1, 0xFFFF, CF, false, false),
        allocData(0x10, 4, 0x10, 0, true, false),
        allocData(0xFFFD, 33, 0xFFFF, CF, false, false),
        allocData(0x10, 36, 0x10, 0, true, false),
        endData()
};

static struct Data btsd[] = {
        allocData(0xFFFDFFFF, 17, 0xFFFFFFFF, CF, false, false),
        allocData(0x100000, 20, 0x100000, 0, true, false),
        allocData(0xFFFDFFFF, 81, 0xFFFFFFFF, CF, false, false),
        allocData(0x100000, 84, 0x100000, 0, true, false),
        endData()
};

static struct Data shld16[] = {
        allocDataConstNoOF(0x1234, 0x5678, 0x2345, 4, 8, 0, true),
        allocDataConst(0x8080, 0x8000, 0x0101, 1, 8, 0, true, true),
        allocDataConst(0x4080, 0x8000, 0x8101, 1, 8, 0, false, true),
        allocDataConst(0x2080, 0x8000, 0x4101, 1, 8, 0, false, false),
        allocDataConstNoOF(0x4080, 0x8000, 0x0202, 2, 8, 0, true),
        //allocDataConstNoOF(0x1234, 0x5678, 0x6785, 20, 8, 0, true), // undefined
        //allocDataConst(0x8080, 0x8000, 0x0001, 17, 8, 0, true, true),
        //allocDataConst(0x4080, 0x4000, 0x8000, 17, 8, 0, false, true),
        //allocDataConst(0x2080, 0x2000, 0x4000, 17, 8, 0, false, false),
        endData()
};

static struct Data shld32[] = {
        allocDataConstNoOF(0x12345678, 0x90abcdef, 0x4567890a, 12, 8, 0, true),
        allocDataConst(0x80808080, 0x80000000, 0x01010101, 1, 8, 0, true, true),
        allocDataConst(0x40808080, 0x80000000, 0x81010101, 1, 8, 0, false, true),
        allocDataConst(0x20808080, 0x80000000, 0x41010101, 1, 8, 0, false, false),
        allocDataConstNoOF(0x40808080, 0x80000000, 0x02020202, 2, 8, 0, true),
        allocDataConst(0x12345678, 0x90abcdef, 0x34567890, 40, 8, 0, false, false),
        endData()
};

static struct Data shrd16[] = {
        allocDataConstNoOF(0x1234, 0x5678, 0x8123, 4, 8, 0, false),
        allocDataConst(0x0101, 0x0001, 0x8080, 1, 8, 0, true, true),
        allocDataConst(0x0102, 0x0001, 0x8081, 1, 8, 0, false, true),
        allocDataConst(0x0101, 0x0002, 0x0080, 1, 8, 0, true, false),
        allocDataConstNoOF(0x8080, 0x0001, 0x6020, 2, 8, 0, false),
        //allocDataConstNoOF(0x1234, 0x5678, 0x8567, 20, 8, 0, true, true), // undefined
        allocDataConstNoOF(0x0101, 0x0001, 0x8000, 17, 8, 0, true),
        allocDataConstNoOF(0x0102, 0x0002, 0x0001, 17, 8, 0, false),
        endData()
};

static struct Data shrd32[] = {
        allocDataConstNoOF(0x12345678, 0x90abcdef, 0xbcdef123, 20, 8, 0, false),
        allocDataConst(0x01010101, 0x00000001, 0x80808080, 1, 8, 0, true, true),
        allocDataConst(0x01010102, 0x00000001, 0x80808081, 1, 8, 0, false, true),
        allocDataConst(0x01010101, 0x00000002, 0x00808080, 1, 8, 0, true, false),
        allocDataConstNoOF(0x80808080, 0x00000001, 0x60202020, 2, 8, 0, false),
        endData()
};

static struct Data xadd[] = {
    {1, 100000, 200200, 300200, 100000, 0, 0, 0, 0, 0, 0, 0, 1, 0, true, 0, 0, 1, 0, 0},
    {1, 0xFFFFFFFF, 1, 0, 0xFFFFFFFF, 0, 0, 1, 0, 0, 0, 0, 1, 0, true, 0, 0, 1, 0, 0},
    endData()
};

#if defined (BOXEDWINE_MSVC) && !defined (BOXEDWINE_64)
#define X86_TEST(op, d, a, c)                  \
    {                                           \
        struct Data* data = d;                  \
        U32 flagMask = CF|OF|ZF|PF|SF|AF;       \
                                                \
        while (data->valid) {                   \
            U32 result;                         \
            U32 flags = data->flags;            \
            __asm {                             \
                __asm mov ebx, data                  \
                __asm mov eax, [ebx].var1            \
                __asm mov ecx, [ebx].var2            \
                __asm mov edx, flags                 \
                                                \
                __asm push edx                       \
                __asm popf                           \
                                                \
                __asm op a, c                        \
                __asm mov result, eax                \
                                                \
                __asm pushf                          \
                __asm pop edx                        \
                __asm mov flags, edx                 \
            }                                   \
            if (!data->dontUseResultAndCheckSFZF)   \
                assertTrue(result == data->result); \
            if (data->dontUseResultAndCheckSFZF || data->hasSF) \
                assertTrue((flags & SF)!=0 == data->fSF!=0);    \
            if (data->dontUseResultAndCheckSFZF || data->hasZF) \
                assertTrue((flags & ZF)!=0 == data->fZF!=0);    \
            if (data->hasCF)                                    \
                assertTrue((flags & CF)!=0 == data->fCF!=0);    \
            if (data->hasOF)                                    \
                assertTrue((flags & OF)!=0 == data->fOF!=0);    \
            if (data->hasAF)                                    \
                assertTrue((flags & AF)!=0 == data->fAF!=0);    \
            data++;                                             \
        }                                                       \
    }                                           

#define X86_TEST1(op, d, a)                  \
    {                                           \
        struct Data* data = d;                  \
        U32 flagMask = CF|OF|ZF|PF|SF|AF;       \
                                                \
        while (data->valid) {                   \
            U32 result;                         \
            U32 flags = data->flags;            \
            __asm {                             \
                __asm mov ebx, data                  \
                __asm mov eax, [ebx].var1            \
                __asm mov ecx, [ebx].var2            \
                __asm mov edx, flags                 \
                                                \
                __asm push edx                       \
                __asm popf                           \
                                                \
                __asm op a                        \
                __asm mov result, eax                \
                                                \
                __asm pushf                          \
                __asm pop edx                        \
                __asm mov flags, edx                 \
            }                                   \
            if (!data->dontUseResultAndCheckSFZF)   \
                assertTrue(result == data->result); \
            if (data->dontUseResultAndCheckSFZF || data->hasSF) \
                assertTrue((flags & SF)!=0 == data->fSF!=0);    \
            if (data->dontUseResultAndCheckSFZF || data->hasZF) \
                assertTrue((flags & ZF)!=0 == data->fZF!=0);    \
            if (data->hasCF)                                    \
                assertTrue((flags & CF)!=0 == data->fCF!=0);    \
            if (data->hasOF)                                    \
                assertTrue((flags & OF)!=0 == data->fOF!=0);    \
            if (data->hasAF)                                    \
                assertTrue((flags & AF)!=0 == data->fAF!=0);    \
            data++;                                             \
        }                                                       \
    } 

#define X86_TEST0(op, d)                  \
    {                                           \
        struct Data* data = d;                  \
        U32 flagMask = CF|OF|ZF|PF|SF|AF;       \
                                                \
        while (data->valid) {                   \
            U32 result;                         \
            U32 flags = data->flags;            \
            __asm {                             \
                __asm mov ebx, data                  \
                __asm mov eax, [ebx].var1            \
                __asm mov ecx, [ebx].var2            \
                __asm mov edx, flags                 \
                                                \
                __asm push edx                       \
                __asm popf                           \
                                                \
                __asm op                         \
                __asm mov result, eax                \
                                                \
                __asm pushf                          \
                __asm pop edx                        \
                __asm mov flags, edx                 \
            }                                   \
            if (!data->dontUseResultAndCheckSFZF)   \
                assertTrue(result == data->result); \
            if (data->dontUseResultAndCheckSFZF || data->hasSF) \
                assertTrue((flags & SF)!=0 == data->fSF!=0);    \
            if (data->dontUseResultAndCheckSFZF || data->hasZF) \
                assertTrue((flags & ZF)!=0 == data->fZF!=0);    \
            if (data->hasCF)                                    \
                assertTrue((flags & CF)!=0 == data->fCF!=0);    \
            if (data->hasOF)                                    \
                assertTrue((flags & OF)!=0 == data->fOF!=0);    \
            if (data->hasAF)                                    \
                assertTrue((flags & AF)!=0 == data->fAF!=0);    \
            data++;                                             \
        }                                                       \
    } 

#define X86_TEST2C(op, d, a, c, imm)                  \
    {                                           \
        struct Data* data = d;                  \
        U32 flagMask = CF|OF|ZF|PF|SF|AF;       \
                                                \
            U32 result;                         \
            U32 flags = data->flags;            \
            __asm {                             \
                __asm mov ebx, data                  \
                __asm mov eax, [ebx].var1            \
                __asm mov ecx, [ebx].var2            \
                __asm mov edx, flags                 \
                                                \
                __asm push edx                       \
                __asm popf                           \
                                                \
                __asm op a, c, imm                        \
                __asm mov result, eax                \
                                                \
                __asm pushf                          \
                __asm pop edx                        \
                __asm mov flags, edx                 \
            }                                   \
            if (!data->dontUseResultAndCheckSFZF)   \
                assertTrue(result == data->result); \
            if (data->dontUseResultAndCheckSFZF || data->hasSF) \
                assertTrue((flags & SF)!=0 == data->fSF!=0);    \
            if (data->dontUseResultAndCheckSFZF || data->hasZF) \
                assertTrue((flags & ZF)!=0 == data->fZF!=0);    \
            if (data->hasCF)                                    \
                assertTrue((flags & CF)!=0 == data->fCF!=0);    \
            if (data->hasOF)                                    \
                assertTrue((flags & OF)!=0 == data->fOF!=0);    \
            if (data->hasAF)                                    \
                assertTrue((flags & AF)!=0 == data->fAF!=0);    \
    } 

#define X86_TEST1C(op, d, a, imm)                  \
    {                                           \
        struct Data* data = d;                  \
        U32 flagMask = CF|OF|ZF|PF|SF|AF;       \
                                                \
            U32 result;                         \
            U32 flags = data->flags;            \
            __asm {                             \
                __asm mov ebx, data                  \
                __asm mov eax, [ebx].var1            \
                __asm mov ecx, [ebx].var2            \
                __asm mov edx, flags                 \
                                                \
                __asm push edx                       \
                __asm popf                           \
                                                \
                __asm op a, imm                        \
                __asm mov result, eax                \
                                                \
                __asm pushf                          \
                __asm pop edx                        \
                __asm mov flags, edx                 \
            }                                   \
            if (!data->dontUseResultAndCheckSFZF)   \
                assertTrue(result == data->result); \
            if (data->dontUseResultAndCheckSFZF || data->hasSF) \
                assertTrue((flags & SF)!=0 == data->fSF!=0);    \
            if (data->dontUseResultAndCheckSFZF || data->hasZF) \
                assertTrue((flags & ZF)!=0 == data->fZF!=0);    \
            if (data->hasCF)                                    \
                assertTrue((flags & CF)!=0 == data->fCF!=0);    \
            if (data->hasOF)                                    \
                assertTrue((flags & OF)!=0 == data->fOF!=0);    \
            if (data->hasAF)                                    \
                assertTrue((flags & AF)!=0 == data->fAF!=0);    \
    } 

#define X86_TEST0C(op, d, imm)                  \
    {                                           \
        struct Data* data = d;                  \
        U32 flagMask = CF|OF|ZF|PF|SF|AF;       \
                                                \
            U32 result;                         \
            U32 flags = data->flags;            \
            __asm {                             \
                __asm mov ebx, data                  \
                __asm mov eax, [ebx].var1            \
                __asm mov ecx, [ebx].var2            \
                __asm mov edx, flags                 \
                                                \
                __asm push edx                       \
                __asm popf                           \
                                                \
                __asm op imm                        \
                __asm mov result, eax                \
                                                \
                __asm pushf                          \
                __asm pop edx                        \
                __asm mov flags, edx                 \
            }                                   \
            if (!data->dontUseResultAndCheckSFZF)   \
                assertTrue(result == data->result); \
            if (data->dontUseResultAndCheckSFZF || data->hasSF) \
                assertTrue((flags & SF)!=0 == data->fSF!=0);    \
            if (data->dontUseResultAndCheckSFZF || data->hasZF) \
                assertTrue((flags & ZF)!=0 == data->fZF!=0);    \
            if (data->hasCF)                                    \
                assertTrue((flags & CF)!=0 == data->fCF!=0);    \
            if (data->hasOF)                                    \
                assertTrue((flags & OF)!=0 == data->fOF!=0);    \
            if (data->hasAF)                                    \
                assertTrue((flags & AF)!=0 == data->fAF!=0);    \
    }

#define STR_TEST(esiStr, editStr, startECX, OP, checkEndFlags, endCF, endZF, newECX) \
{                                                                                   \
    U32 cfValue;                                                                    \
    U32 zfValue;                                                                    \
    U32 esiValue = (U32)esiStr;                                                     \
    U32 ediValue = (U32)editStr;                                                    \
    U32 ecxValue;                                                                   \
    __asm {                                                                         \
        __asm mov esi, esiValue                                                    \
        __asm mov edi, ediValue                                                    \
        __asm mov ecx, startECX                                                    \
        __asm OP                                                                   \
        __asm mov eax, 0                                                           \
        __asm setb al                                                              \
        __asm mov cfValue, eax                                                     \
        __asm setz al                                                              \
        __asm mov zfValue, eax                                                     \
        __asm mov ecxValue, ecx                                                         \
    }                                                                               \
    if (checkEndFlags) {                                                            \
        bool hasCF=cfValue!=0;                                                      \
        assertTrue(endCF==hasCF);                                                   \
                                                                                    \
        bool hasZF=zfValue!=0;                                                      \
        assertTrue(endZF==hasZF);                                                   \
    }                                                                               \
    assertTrue(ecxValue==newECX);                                                   \
}
#else
#define X86_TEST0C(op, d, imm)
#define X86_TEST1C(op, d, a, imm)
#define X86_TEST2C(op, d, a, c, imm) 
#define X86_TEST0(op, d)
#define X86_TEST1(op, d, a)
#define X86_TEST(op, d, a, c)
#define STR_TEST(esiStr, editStr, startECX, OP, checkEndFlags, endCF, endZF, newECX)
#endif

void testAdd0x000() {cpu->big = false;EbGb(0x00, addb);}
void testAdd0x200() {cpu->big = true;EbGb(0x00, addb);X86_TEST(add, addb, al, cl)}
void testAdd0x001() {cpu->big = false;EwGw(0x01, addw);X86_TEST(add, addw, ax, cx)}
void testAdd0x201() {cpu->big = true;EdGd(0x01, addd);X86_TEST(add, addd, eax, ecx)}
void testAdd0x002() {cpu->big = false;GbEb(0x02, addb);}
void testAdd0x202() {cpu->big = true;GbEb(0x02, addb);}
void testAdd0x003() {cpu->big = false;GwEw(0x03, addw);}
void testAdd0x203() {cpu->big = true;GdEd(0x03, addd);}
void testAdd0x004() {cpu->big = false;AlIb(0x04, addb);}
void testAdd0x204() {cpu->big = true;AlIb(0x04, addb);}
void testAdd0x005() {cpu->big = false;AxIw(0x05, addw);}
void testAdd0x205() {cpu->big = true;EaxId(0x05, addd);}

void testOr0x008() {cpu->big = false;EbGb(0x08, orb);}
void testOr0x208() {cpu->big = true;EbGb(0x08, orb);X86_TEST(or, orb, al, cl)}
void testOr0x009() {cpu->big = false;EwGw(0x09, orw);X86_TEST(or, orw, ax, cx)}
void testOr0x209() {cpu->big = true;EdGd(0x09, ord);X86_TEST(or, ord, eax, ecx)}
void testOr0x00a() {cpu->big = false;GbEb(0x0a, orb);}
void testOr0x20a() {cpu->big = true;GbEb(0x0a, orb);}
void testOr0x00b() {cpu->big = false;GwEw(0x0b, orw);}
void testOr0x20b() {cpu->big = true;GdEd(0x0b, ord);}
void testOr0x00c() {cpu->big = false;AlIb(0x0c, orb);}
void testOr0x20c() {cpu->big = true;AlIb(0x0c, orb);}
void testOr0x00d() {cpu->big = false;AxIw(0x0d, orw);}
void testOr0x20d() {cpu->big = true;EaxId(0x0d, ord);}

void testAdc0x010() {cpu->big = false;EbGb(0x10, adcb);}
void testAdc0x210() {cpu->big = true;EbGb(0x10, addb);X86_TEST(adc, adcb, al, cl)}
void testAdc0x011() {cpu->big = false;EwGw(0x11, adcw);X86_TEST(adc, adcw, ax, cx)}
void testAdc0x211() {cpu->big = true;EdGd(0x11, adcd);X86_TEST(adc, adcd, eax, ecx)}
void testAdc0x012() {cpu->big = false;GbEb(0x12, adcb);}
void testAdc0x212() {cpu->big = true;GbEb(0x12, adcb);}
void testAdc0x013() {cpu->big = false;GwEw(0x13, adcw);}
void testAdc0x213() {cpu->big = true;GdEd(0x13, adcd);}
void testAdc0x014() {cpu->big = false;AlIb(0x14, adcb);}
void testAdc0x214() {cpu->big = true;AlIb(0x14, adcb);}
void testAdc0x015() {cpu->big = false;AxIw(0x15, adcw);}
void testAdc0x215() {cpu->big = true;EaxId(0x15, adcd);}

void testSbb0x018() {cpu->big = false;EbGb(0x18, sbbb);}
void testSbb0x218() {cpu->big = true;EbGb(0x18, sbbb);X86_TEST(sbb, sbbb, al, cl)}
void testSbb0x019() {cpu->big = false;EwGw(0x19, sbbw);X86_TEST(sbb, sbbw, ax, cx)}
void testSbb0x219() {cpu->big = true;EdGd(0x19, sbbd);X86_TEST(sbb, sbbd, eax, ecx)}
void testSbb0x01a() {cpu->big = false;GbEb(0x1a, sbbb);}
void testSbb0x21a() {cpu->big = true;GbEb(0x1a, sbbb);}
void testSbb0x01b() {cpu->big = false;GwEw(0x1b, sbbw);}
void testSbb0x21b() {cpu->big = true;GdEd(0x1b, sbbd);}
void testSbb0x01c() {cpu->big = false;AlIb(0x1c, sbbb);}
void testSbb0x21c() {cpu->big = true;AlIb(0x1c, sbbb);}
void testSbb0x01d() {cpu->big = false;AxIw(0x1d, sbbw);}
void testSbb0x21d() {cpu->big = true;EaxId(0x1d, sbbd);}

void testAnd0x020() {cpu->big = false;EbGb(0x20, andb);}
void testAnd0x220() {cpu->big = true;EbGb(0x20, andb);X86_TEST(and, andb, al, cl)}
void testAnd0x021() {cpu->big = false;EwGw(0x21, andw);X86_TEST(and, andw, ax, cx)}
void testAnd0x221() {cpu->big = true;EdGd(0x21, andd);X86_TEST(and, andd, eax, ecx)}
void testAnd0x022() {cpu->big = false;GbEb(0x22, andb);}
void testAnd0x222() {cpu->big = true;GbEb(0x22, andb);}
void testAnd0x023() {cpu->big = false;GwEw(0x23, andw);}
void testAnd0x223() {cpu->big = true;GdEd(0x23, andd);}
void testAnd0x024() {cpu->big = false;AlIb(0x24, andb);}
void testAnd0x224() {cpu->big = true;AlIb(0x24, andb);}
void testAnd0x025() {cpu->big = false;AxIw(0x25, andw);}
void testAnd0x225() {cpu->big = true;EaxId(0x25, andd);}

void testDaa0x027() {cpu->big = false;EbReg(0x27, 0, daa);}
void testDaa0x227() {cpu->big = true;EbReg(0x27, 0, daa);}

void testSub0x028() {cpu->big = false;EbGb(0x28, subb);}
void testSub0x228() {cpu->big = true;EbGb(0x28, subb);X86_TEST(sub, subb, al, cl)}
void testSub0x029() {cpu->big = false;EwGw(0x29, subw);X86_TEST(sub, subw, ax, cx)}
void testSub0x229() {cpu->big = true;EdGd(0x29, subd);X86_TEST(sub, subd, eax, ecx)}
void testSub0x02a() {cpu->big = false;GbEb(0x2a, subb);}
void testSub0x22a() {cpu->big = true;GbEb(0x2a, subb);}
void testSub0x02b() {cpu->big = false;GwEw(0x2b, subw);}
void testSub0x22b() {cpu->big = true;GdEd(0x2b, subd);}
void testSub0x02c() {cpu->big = false;AlIb(0x2c, subb);}
void testSub0x22c() {cpu->big = true;AlIb(0x2c, subb);}
void testSub0x02d() {cpu->big = false;AxIw(0x2d, subw);}
void testSub0x22d() {cpu->big = true;EaxId(0x2d, subd);}

void testDas0x02f() {cpu->big = false;EbReg(0x2f, 0, das);}
void testDas0x22f() {cpu->big = true;EbReg(0x2f, 0, das);}

void testXor0x030() {cpu->big = false;EbGb(0x30, xorb);}
void testXor0x230() {cpu->big = true;EbGb(0x30, xorb);X86_TEST(xor, xorb, al, cl)}
void testXor0x031() {cpu->big = false;EwGw(0x31, xorw);X86_TEST(xor, xorw, ax, cx)}
void testXor0x231() {cpu->big = true;EdGd(0x31, xord);X86_TEST(xor, xord, eax, ecx)}
void testXor0x032() {cpu->big = false;GbEb(0x32, xorb);}
void testXor0x232() {cpu->big = true;GbEb(0x32, xorb);}
void testXor0x033() {cpu->big = false;GwEw(0x33, xorw);}
void testXor0x233() {cpu->big = true;GdEd(0x33, xord);}
void testXor0x034() {cpu->big = false;AlIb(0x34, xorb);}
void testXor0x234() {cpu->big = true;AlIb(0x34, xorb);}
void testXor0x035() {cpu->big = false;AxIw(0x35, xorw);}
void testXor0x235() {cpu->big = true;EaxId(0x35, xord);}

void testAaa0x037() {cpu->big = false;EwReg(0x37, 0, aaa);}
void testAaa0x237() {cpu->big = true;EwReg(0x37, 0, aaa);}

void testCmp0x038() {cpu->big = false;EbGb(0x38, cmpb);}
void testCmp0x238() {cpu->big = true;EbGb(0x38, cmpb);X86_TEST(cmp, cmpb, al, cl)}
void testCmp0x039() {cpu->big = false;EwGw(0x39, cmpw);X86_TEST(cmp, cmpw, ax, cx)}
void testCmp0x239() {cpu->big = true;EdGd(0x39, cmpd);X86_TEST(cmp, cmpd, eax, ecx)}
void testCmp0x03a() {cpu->big = false;GbEb(0x3a, cmpb);}
void testCmp0x23a() {cpu->big = true;GbEb(0x3a, cmpb);}
void testCmp0x03b() {cpu->big = false;GwEw(0x3b, cmpw);}
void testCmp0x23b() {cpu->big = true;GdEd(0x3b, cmpd);}
void testCmp0x03c() {cpu->big = false;AlIb(0x3c, cmpb);}
void testCmp0x23c() {cpu->big = true;AlIb(0x3c, cmpb);}
void testCmp0x03d() {cpu->big = false;AxIw(0x3d, cmpw);}
void testCmp0x23d() {cpu->big = true;EaxId(0x3d, cmpd);}

void testAas0x03f() {cpu->big = false;EwReg(0x3f, 0, aas);}
void testAas0x23f() {cpu->big = true;EwReg(0x3f, 0, aas);}

void testIncAx0x040() {cpu->big = false;EwReg(0x40, 0, incw);X86_TEST1(inc, incw, ax)}
void testIncEax0x240() {cpu->big = true;EdReg(0x40, 0, incd);X86_TEST1(inc, incd, eax)}
void testIncCx0x041() {cpu->big = false;EwReg(0x41, 1, incw);}
void testIncEcx0x241() {cpu->big = true;EdReg(0x41, 1, incd);}
void testIncDx0x042() {cpu->big = false;EwReg(0x42, 2, incw);}
void testIncEdx0x242() {cpu->big = true;EdReg(0x42, 2, incd);}
void testIncBx0x043() {cpu->big = false;EwReg(0x43, 3, incw);}
void testIncEbx0x243() {cpu->big = true;EdReg(0x43, 3, incd);}
void testIncSp0x044() {cpu->big = false;EwReg(0x44, 4, incw);}
void testIncEsp0x244() {cpu->big = true;EdReg(0x44, 4, incd);}
void testIncBp0x045() {cpu->big = false;EwReg(0x45, 5, incw);}
void testIncEbp0x245() {cpu->big = true;EdReg(0x45, 5, incd);}
void testIncSi0x046() {cpu->big = false;EwReg(0x46, 6, incw);}
void testIncEsi0x246() {cpu->big = true;EdReg(0x46, 6, incd);}
void testIncDi0x047() {cpu->big = false;EwReg(0x47, 7, incw);}
void testIncEdi0x247() {cpu->big = true;EdReg(0x47, 7, incd);}

void testDecAx0x048() {cpu->big = false;EwReg(0x48, 0, decw);X86_TEST1(dec, decw, ax)}
void testDecEax0x248() {cpu->big = true;EdReg(0x48, 0, decd);X86_TEST1(dec, decd, eax)}
void testDecCx0x049() {cpu->big = false;EwReg(0x49, 1, decw);}
void testDecEcx0x249() {cpu->big = true;EdReg(0x49, 1, decd);}
void testDecDx0x04a() {cpu->big = false;EwReg(0x4a, 2, decw);}
void testDecEdx0x24a() {cpu->big = true;EdReg(0x4a, 2, decd);}
void testDecBx0x04b() {cpu->big = false;EwReg(0x4b, 3, decw);}
void testDecEbx0x24b() {cpu->big = true;EdReg(0x4b, 3, decd);}
void testDecSp0x04c() {cpu->big = false;EwReg(0x4c, 4, decw);}
void testDecEsp0x24c() {cpu->big = true;EdReg(0x4c, 4, decd);}
void testDecBp0x04d() {cpu->big = false;EwReg(0x4d, 5, decw);}
void testDecEbp0x24d() {cpu->big = true;EdReg(0x4d, 5, decd);}
void testDecSi0x04e() {cpu->big = false;EwReg(0x4e, 6, decw);}
void testDecEsi0x24e() {cpu->big = true;EdReg(0x4e, 6, decd);}
void testDecDi0x04f() {cpu->big = false;EwReg(0x4f, 7, decw);}
void testDecEdi0x24f() {cpu->big = true;EdReg(0x4f, 7, decd);}

void testPushAx0x050() {cpu->big = false;push16Reg(0x50, &cpu->reg[0]);}
void testPushEax0x250() {cpu->big = true;push32Reg(0x50, &cpu->reg[0]);}
void testPushCx0x051() {cpu->big = false;push16Reg(0x51, &cpu->reg[1]);}
void testPushEcx0x251() {cpu->big = true;push32Reg(0x51, &cpu->reg[1]);}
void testPushDx0x052() {cpu->big = false;push16Reg(0x52, &cpu->reg[2]);}
void testPushEdx0x252() {cpu->big = true;push32Reg(0x52, &cpu->reg[2]);}
void testPushBx0x053() {cpu->big = false;push16Reg(0x53, &cpu->reg[3]);}
void testPushEbx0x253() {cpu->big = true;push32Reg(0x53, &cpu->reg[3]);}
void testPushSp0x054() {cpu->big = false;push16Reg(0x54, &cpu->reg[4]);}
void testPushEsp0x254() {cpu->big = true;push32Reg(0x54, &cpu->reg[4]);}
void testPushBp0x055() {cpu->big = false;push16Reg(0x55, &cpu->reg[5]);}
void testPushEbp0x255() {cpu->big = true;push32Reg(0x55, &cpu->reg[5]);}
void testPushSi0x056() {cpu->big = false;push16Reg(0x56, &cpu->reg[6]);}
void testPushEsi0x256() {cpu->big = true;push32Reg(0x56, &cpu->reg[6]);}
void testPushDi0x057() {cpu->big = false;push16Reg(0x57, &cpu->reg[7]);}
void testPushEdi0x257() {cpu->big = true;push32Reg(0x57, &cpu->reg[7]);}

void testPopAx0x058() {cpu->big = false;Pop16(0x58, &cpu->reg[0]);}
void testPopEax0x258() {cpu->big = true;Pop32(0x58, &cpu->reg[0]);}
void testPopCx0x059() {cpu->big = false;Pop16(0x59, &cpu->reg[1]);}
void testPopEcx0x259() {cpu->big = true;Pop32(0x59, &cpu->reg[1]);}
void testPopDx0x05a() {cpu->big = false;Pop16(0x5a, &cpu->reg[2]);}
void testPopEdx0x25a() {cpu->big = true;Pop32(0x5a, &cpu->reg[2]);}
void testPopBx0x05b() {cpu->big = false;Pop16(0x5b, &cpu->reg[3]);}
void testPopEbx0x25b() {cpu->big = true;Pop32(0x5b, &cpu->reg[3]);}
void testPopSp0x05c() {cpu->big = false;Pop16_SP(0x5c, &cpu->reg[4]);}
void testPopEsp0x25c() {cpu->big = true;Pop32_SP(0x5c, &cpu->reg[4]);}
void testPopBp0x05d() {cpu->big = false;Pop16(0x5d, &cpu->reg[5]);}
void testPopEbp0x25d() {cpu->big = true;Pop32(0x5d, &cpu->reg[5]);}
void testPopSi0x05e() {cpu->big = false;Pop16(0x5e, &cpu->reg[6]);}
void testPopEsi0x25e() {cpu->big = true;Pop32(0x5e, &cpu->reg[6]);}
void testPopDi0x05f() {cpu->big = false;Pop16(0x5f, &cpu->reg[7]);}
void testPopEdi0x25f() {cpu->big = true;Pop32(0x5f, &cpu->reg[7]);}

void testPush0x068() {cpu->big = false;push16(0x68);}
void testPush0x268() {cpu->big = true;push32(0x68);}

void testIMul0x069() {cpu->big = false;GwEw(0x69, imulw);X86_TEST2C(imul, &imulw[0], ax, cx, 2) X86_TEST2C(imul, &imulw[1], ax, cx, 2) X86_TEST2C(imul, &imulw[2], ax, cx, 0xFFFE) X86_TEST2C(imul, &imulw[3], ax, cx, 300) X86_TEST2C(imul, &imulw[4], ax, cx, 300)}
void testIMul0x269() {cpu->big = true;GdEd(0x69, imuld);X86_TEST2C(imul, &imuld[0], eax, ecx, 2) X86_TEST2C(imul, &imuld[1], eax, ecx, 2) X86_TEST2C(imul, &imuld[2], eax, ecx, 0xFFFFFFFE) X86_TEST2C(imul, &imuld[3], eax, ecx, 400000) X86_TEST2C(imul, &imuld[4], eax, ecx, 400000)}

void testPush0x06a() {cpu->big = false;push16s8(0x6a);}
void testPush0x26a() {cpu->big = true;push32s8(0x6a);}

void testIMul0x06b() {cpu->big = false;GwEw(0x6b, imulw_s8);}
void testIMul0x26b() {cpu->big = true;GdEd(0x6b, imuld_s8);}

void testGrp10x080() {
    cpu->big = false;
    EbIb(0x80, 0, addb);
    EbIb(0x80, 1, orb);
    EbIb(0x80, 2, adcb);
    EbIb(0x80, 3, sbbb);
    EbIb(0x80, 4, andb);
    EbIb(0x80, 5, subb);
    EbIb(0x80, 6, xorb);
    EbIb(0x80, 7, cmpb);
}

void testGrp10x280() {
    cpu->big = true;
    EbIb(0x80, 0, addb);
    EbIb(0x80, 1, orb);
    EbIb(0x80, 2, adcb);
    EbIb(0x80, 3, sbbb);
    EbIb(0x80, 4, andb);
    EbIb(0x80, 5, subb);
    EbIb(0x80, 6, xorb);
    EbIb(0x80, 7, cmpb);
}

void testGrp10x081() {
    cpu->big = false;
    EwIw(0x81, 0, addw);
    EwIw(0x81, 1, orw);
    EwIw(0x81, 2, adcw);
    EwIw(0x81, 3, sbbw);
    EwIw(0x81, 4, andw);
    EwIw(0x81, 5, subw);
    EwIw(0x81, 6, xorw);
    EwIw(0x81, 7, cmpw);
}

void testGrp10x281() {
    cpu->big = true;
    EdId(0x81, 0, addd);
    EdId(0x81, 1, ord);
    EdId(0x81, 2, adcd);
    EdId(0x81, 3, sbbd);
    EdId(0x81, 4, andd);
    EdId(0x81, 5, subd);
    EdId(0x81, 6, xord);
    EdId(0x81, 7, cmpd);
}

void testGrp10x083() {
    cpu->big = false;
    EwIx(0x83, 0, addw);
    EwIx(0x83, 1, orw);
    EwIx(0x83, 2, adcw);
    EwIx(0x83, 3, sbbw);
    EwIx(0x83, 4, andw);
    EwIx(0x83, 5, subw);
    EwIx(0x83, 6, xorw);
    EwIx(0x83, 7, cmpw);
}

void testGrp10x283() {
    cpu->big = true;
    EdIx(0x83, 0, addd);
    EdIx(0x83, 1, ord);
    EdIx(0x83, 2, adcd);
    EdIx(0x83, 3, sbbd);
    EdIx(0x83, 4, andd);
    EdIx(0x83, 5, subd);
    EdIx(0x83, 6, xord);
    EdIx(0x83, 7, cmpd);
}

void testTest0x084() {cpu->big = false;EbGb(0x84, testb);}
void testTest0x284() {cpu->big = true;EbGb(0x84, testb); X86_TEST(test, testb, al, cl)}
void testTest0x085() {cpu->big = false;EwGw(0x85, testw); X86_TEST(test, testw, ax, cx)}
void testTest0x285() {cpu->big = true;EdGd(0x85, testd); X86_TEST(test, testd, eax, ecx)}

void testXchg0x086() {cpu->big = false;EbGb(0x86, xchgb);}
void testXchg0x286() {cpu->big = true;EbGb(0x86, xchgb);}
void testXchg0x087() {cpu->big = false;EwGw(0x87, xchgw);}
void testXchg0x287() {cpu->big = true;EdGd(0x87, xchgd);}

void testMovEbGb0x088() {cpu->big = false;EbGb(0x88, movb);}
void testMovEbGb0x288() {cpu->big = true;EbGb(0x88, movb);}
void testMovEwGw0x089() {cpu->big = false;EwGw(0x89, movw);}
void testMovEdGd0x289() {cpu->big = true;EdGd(0x89, movd);}

void testMovEbGb0x08a() {cpu->big = false;GbEb(0x8a, movb);}
void testMovEbGb0x28a() {cpu->big = true;GbEb(0x8a, movb);}
void testMovEwGw0x08b() {cpu->big = false;GwEw(0x8b, movw);}
void testMovEdGd0x28b() {cpu->big = true;GdEd(0x8b, movd);}

void testMovEwSw0x08c() {cpu->big = false;EwSw(0x8c, movw);}
void testMovEwSw0x28c() {cpu->big = true;EdSw(0x8c, movw);}

void testLeaGw0x08d() {cpu->big = false;LeaGw();}
void testLeaGd0x28d() {cpu->big = true;LeaGd();}

// :TODO: 0x08e
// :TODO: 0x28e

void testPopEw0x08f() {cpu->big = false;PopEw();}
void testPopEd0x28f() {cpu->big = true;PopEd();}

void testXchgCxAx0x091() {cpu->big = false;Reg16Reg16(0x91, xchgw, &cpu->reg[0], &cpu->reg[1]);}
void testXchgEcxEax0x291() {cpu->big = true;Reg32Reg32(0x91, xchgd, &cpu->reg[0], &cpu->reg[1]);}
void testXchgDxAx0x092() {cpu->big = false;Reg16Reg16(0x92, xchgw, &cpu->reg[0], &cpu->reg[2]);}
void testXchgEdxEax0x292() {cpu->big = true;Reg32Reg32(0x92, xchgd, &cpu->reg[0], &cpu->reg[2]);}
void testXchgBxAx0x093() {cpu->big = false;Reg16Reg16(0x93, xchgw, &cpu->reg[0], &cpu->reg[3]);}
void testXchgEbxEax0x293() {cpu->big = true;Reg32Reg32(0x93, xchgd, &cpu->reg[0], &cpu->reg[3]);}
void testXchgSpAx0x094() {cpu->big = false;Reg16Reg16(0x94, xchgw, &cpu->reg[0], &cpu->reg[4]);}
void testXchgEspEax0x294() {cpu->big = true;Reg32Reg32(0x94, xchgd, &cpu->reg[0], &cpu->reg[4]);}
void testXchgBpAx0x095() {cpu->big = false;Reg16Reg16(0x95, xchgw, &cpu->reg[0], &cpu->reg[5]);}
void testXchgEbpEax0x295() {cpu->big = true;Reg32Reg32(0x95, xchgd, &cpu->reg[0], &cpu->reg[5]);}
void testXchgSiAx0x096() {cpu->big = false;Reg16Reg16(0x96, xchgw, &cpu->reg[0], &cpu->reg[6]);}
void testXchgEsiEax0x296() {cpu->big = true;Reg32Reg32(0x96, xchgd, &cpu->reg[0], &cpu->reg[6]);}
void testXchgDiAx0x097() {cpu->big = false;Reg16Reg16(0x97, xchgw, &cpu->reg[0], &cpu->reg[7]);}
void testXchgEdiEax0x297() {cpu->big = true;Reg32Reg32(0x97, xchgd, &cpu->reg[0], &cpu->reg[7]);}

void testCbw0x098() {cpu->big = false;EwReg(0x98, 0, cbw); X86_TEST0(cbw, cbw)}
void testCwde0x298() {cpu->big = true;EdReg(0x98, 0, cwde); X86_TEST0(cwde, cwde)}
void testCwd0x099() {cpu->big = false;Reg16Reg16(0x99, cwd, &cpu->reg[0], &cpu->reg[2]);}
void testCdq0x299() {cpu->big = true;Reg32Reg32(0x99, cdq, &cpu->reg[0], &cpu->reg[2]);}

void testPushf0x09c() {cpu->big = false;Pushf(0x9c);}
void testPushf0x29c() {cpu->big = true;Pushfd(0x9c);}
void testPopf0x09d() {cpu->big = false;Popf(0x9d);}
void testPopf0x29d() {cpu->big = true;Popfd(0x9d);}

void testSahf0x09e() {cpu->big = false;flags(0x9e, sahf, &cpu->reg[0]);}
void testSahf0x29e() {cpu->big = true;flags(0x9e, sahf, &cpu->reg[0]);}
void testLahf0x09f() {cpu->big = false;flags(0x9f, lahf, &cpu->reg[0]);}
void testLahf0x29f() {cpu->big = true;flags(0x9f, lahf, &cpu->reg[0]);}

void strTest(U8 width, U8 prefix, U8 inst, U32 startFlags, const char* str1, U32 str1Len, const char* str2, U32 str2Len, U32 startESI,U32 startEDI, U32 startECX, U32 endESI, U32 endEDI, U32 endECX, bool checkEndFlags, bool endCF, bool endZF, U32 esAddress) {
    if (prefix) {
        newInstruction(prefix, startFlags);
        pushCode8(inst);
    } else {
        newInstruction(inst, startFlags);
    }
    cpu->seg[ES].address = esAddress;
    EDI = startEDI;
    ESI = startESI;
    ECX = startECX;
    
    if (startFlags & DF) {
        U32 offset = 0;
        if (width==2)
            offset = 1;
        else if (width==4)
            offset = 3;
        for (U32 i=0;i<str1Len;i++) {
            writeb(cpu->seg[DS].address+SI-i+offset, str1[i]);
        }
        for (U32 i=0;i<str2Len;i++) {
            writeb(cpu->seg[ES].address+DI-i+offset, str2[i]);
        }
    } else {
        if (str1)
            memcopyFromNative(cpu->seg[DS].address+SI, str1, str1Len);
        if (str2)
            memcopyFromNative(cpu->seg[ES].address+DI, str2, str2Len);
    }

    runTestCPU();
    assertTrue(EDI==endEDI);
    assertTrue(ESI==endESI);    
    assertTrue(ECX==endECX);    
    if (checkEndFlags) {
        bool hasCF=cpu->getCF()!=0;
        assertTrue(endCF==hasCF);

        bool hasZF=cpu->getZF()!=0;
        assertTrue(endZF==hasZF);
    }
    cpu->seg[ES].address = 0;
}

void testCmpsb0x0a6() {
    cpu->big = false;

    // SI > DI (DF)
    strTest(1, 0, 0xa6, DF, "1", 1, "0", 1, 0x12340010, 0x12340020, 0, 0x1234000F, 0x1234001F, 0, true, false, false, HEAP_ADDRESS+256);

    // SI > DI
    strTest(1, 0, 0xa6, 0, "1", 1, "0", 1, 0x12340010, 0x12340020, 0, 0x12340011, 0x12340021, 0, true, false, false, HEAP_ADDRESS+256);

    // SI < DI
    strTest(1, 0, 0xa6, 0, "0", 1, "1", 1, 0x12340010, 0x12340020, 0, 0x12340011, 0x12340021, 0, true, true, false, HEAP_ADDRESS+256);

    // SI == DI
    // this will test 16-bit wrapping
    strTest(1, 0, 0xa6, 0, "1", 1, "1", 1, 0x12340010, 0x1234FFFF, 0, 0x12340011, 0x12340000, 0, true, false, true, HEAP_ADDRESS-0x10000+200);

    // repz
    strTest(1, 0xf3, 0xa6, 0, "abcd", 4, "abce", 4, 0x12340000, 0x12340000, 0x12340010, 0x12340004, 0x12340004, 0x1234000C, true, true, false, HEAP_ADDRESS+256);

    // repnz
    strTest(1, 0xf2, 0xa6, 0, "abcd", 4, "123d", 4, 0x12340000, 0x12340000, 0x12340010, 0x12340004, 0x12340004, 0x1234000C, true, false, true, HEAP_ADDRESS+256);

    // repnz (DF)
    strTest(1, 0xf2, 0xa6, DF, "abcd", 4, "123d", 4, 0x12340020, 0x12340010, 0x12340010, 0x1234001C, 0x1234000C, 0x1234000C, true, false, true, HEAP_ADDRESS+256);    
}

void testCmpsb0x2a6() {
    cpu->big = true;
    
    // ESI > EDI
    STR_TEST("1", "0", 0, cmpsb, true, false, false, 0);
    strTest(1, 0, 0xa6, 0, "1", 1, "0", 1, 0, 1, 0, 1, 2, 0, true, false, false, HEAP_ADDRESS+256);


    // ESI < EDI
    STR_TEST("0", "1", 0, cmpsb, true, true, false, 0);
    strTest(1, 0, 0xa6, 0, "0", 1, "1", 1, 0, 1, 0, 1, 2, 0, true, true, false, HEAP_ADDRESS+256);

    // ESI == EDI
    STR_TEST("1", "1", 0, cmpsb, true, false, true, 0);
    strTest(1, 0, 0xa6, 0, "1", 1, "1", 1, 0, 1, 0, 1, 2, 0, true, false, true, HEAP_ADDRESS+256);

    // repz
    STR_TEST("abcd", "abce", 10, repz cmpsb, true, true, false, 6);
    strTest(1, 0xf3, 0xa6, 0, "abcd", 4, "abce", 4, 0, 256, 256, 4, 260, 252, true, true, false, HEAP_ADDRESS);
    
    // repnz
    STR_TEST("123d", "abcd", 10, repnz cmpsb, true, false, true, 6);
    strTest(1, 0xf2, 0xa6, 0, "123d", 4, "abcd", 4, 0, 256, 256, 4, 260, 252, true, false, true, HEAP_ADDRESS);
}

void testCmpsw0x0a7() {
    cpu->big = false;

    // SI > DI (DF)
    strTest(2, 0, 0xa7, DF, "21", 2, "11", 2, 0x12340010, 0x12340020, 0, 0x1234000E, 0x1234001E, 0, true, false, false, HEAP_ADDRESS+256);

    // SI > DI
    strTest(2, 0, 0xa7, 0, "12", 2, "11", 2, 0x12340010, 0x12340020, 0, 0x12340012, 0x12340022, 0, true, false, false, HEAP_ADDRESS+256);

    // SI < DI
    strTest(2, 0, 0xa7, 0, "11", 2, "12", 2, 0x12340010, 0x12340020, 0, 0x12340012, 0x12340022, 0, true, true, false, HEAP_ADDRESS+256);

    // SI == DI
    // this will test 16-bit wrapping
    strTest(2, 0, 0xa7, 0, "11", 2, "11", 2, 0x12340010, 0x1234FFFE, 0, 0x12340012, 0x12340000, 0, true, false, true, HEAP_ADDRESS-0x10000+200);

    // repz
    strTest(2, 0xf3, 0xa7, 0, "abcdefgh", 8, "abcdefgi", 8, 0x12340000, 0x12340000, 0x12340010, 0x12340008, 0x12340008, 0x1234000C, true, true, false, HEAP_ADDRESS+256);

    // repnz
    strTest(2, 0xf2, 0xa7, 0, "abcdefgh", 8, "123456gh", 8, 0x12340000, 0x12340000, 0x12340010, 0x12340008, 0x12340008, 0x1234000C, true, false, true, HEAP_ADDRESS+256);

    // repnz (DF)
    strTest(2, 0xf2, 0xa7, DF, "abcdefgh", 8, "123456gh", 8, 0x12340020, 0x12340010, 0x12340010, 0x12340018, 0x12340008, 0x1234000C, true, false, true, HEAP_ADDRESS+256);    
}

void testCmpsd0x2a7() {
    cpu->big = true;

    // reminder, little endian, so the biggest part of the 4 byte word is the last byte

    // ESI > EDI (DF)
    // since this is reversed, the biggest part of the number is the first byte
    strTest(4, 0, 0xa7, DF, "4321", 4, "1999", 4, 0x00000010, 0x00000020, 0, 0x0000000C, 0x0000001C, 0, true, false, false, HEAP_ADDRESS+256);

    // ESI > EDI
    strTest(4, 0, 0xa7, 0, "2222", 4, "4321", 4, 0x00000010, 0x00000020, 0, 0x00000014, 0x00000024, 0, true, false, false, HEAP_ADDRESS+256);

    // ESI < EDI
    strTest(4, 0, 0xa7, 0, "4321", 4, "2222", 4, 0x00000010, 0x00000020, 0, 0x00000014, 0x00000024, 0, true, true, false, HEAP_ADDRESS+256);

    // SI == DI
    strTest(4, 0, 0xa7, 0, "1234", 4, "1234", 4, 0x00000010, 0x00000020, 0, 0x00000014, 0x00000024, 0, true, false, true, HEAP_ADDRESS+256);

    // repz
    STR_TEST("abcdefghijklmnop", "abcdefghijklmnoq", 10, repz cmpsd, true, true, false, 6);
    strTest(4, 0xf3, 0xa7, 0, "abcdefghijklmnop", 16, "abcdefghijklmnoq", 16, 0x00000010, 0x00000020, 0x00000010, 0x00000020, 0x00000030, 0x0000000C, true, true, false, HEAP_ADDRESS+256);

    // repnz
    strTest(4, 0xf2, 0xa7, 0, "abcdefghijklmnop", 16, "123456781234mnop", 16, 0x00000010, 0x00000020, 0x00000010, 0x00000020, 0x00000030, 0x0000000C, true, false, true, HEAP_ADDRESS+256);

    // repz (DF)
    strTest(4, 0xf3, 0xa7, DF, "abcdefghijklmnop", 16, "abcdefghijklmnoq", 16, 0x00000010, 0x00000020, 0x00000010, 0x00000000, 0x00000010, 0x0000000C, true, true, false, HEAP_ADDRESS+256);    
}

// :TODO: 0xa0 - 0xaf

void testMovAlIb0x0b0() {cpu->big = false;EbRegIb(0xb0, cpu->reg8[0], 0, movb);}
void testMovAlIb0x2b0() {cpu->big = true;EbRegIb(0xb0, cpu->reg8[0], 0, movb);}
void testMovClIb0x0b1() {cpu->big = false;EbRegIb(0xb1, cpu->reg8[1], 1, movb);}
void testMovClIb0x2b1() {cpu->big = true;EbRegIb(0xb1, cpu->reg8[1], 1, movb);}
void testMovDlIb0x0b2() {cpu->big = false;EbRegIb(0xb2, cpu->reg8[2], 2, movb);}
void testMovDlIb0x2b2() {cpu->big = true;EbRegIb(0xb2, cpu->reg8[2], 2, movb);}
void testMovBlIb0x0b3() {cpu->big = false;EbRegIb(0xb3, cpu->reg8[3], 3, movb);}
void testMovBlIb0x2b3() {cpu->big = true;EbRegIb(0xb3, cpu->reg8[3], 3, movb);}
void testMovAhIb0x0b4() {cpu->big = false;EbRegIb(0xb4, cpu->reg8[4], 4, movb);}
void testMovAhIb0x2b4() {cpu->big = true;EbRegIb(0xb4, cpu->reg8[4], 4, movb);}
void testMovChIb0x0b5() {cpu->big = false;EbRegIb(0xb5, cpu->reg8[5], 5, movb);}
void testMovChIb0x2b5() {cpu->big = true;EbRegIb(0xb5, cpu->reg8[5], 5, movb);}
void testMovDhIb0x0b6() {cpu->big = false;EbRegIb(0xb6, cpu->reg8[6], 6, movb);}
void testMovDhIb0x2b6() {cpu->big = true;EbRegIb(0xb6, cpu->reg8[6], 6, movb);}
void testMovBhIb0x0b7() {cpu->big = false;EbRegIb(0xb7, cpu->reg8[7], 7, movb);}
void testMovBhIb0x2b7() {cpu->big = true;EbRegIb(0xb7, cpu->reg8[7], 7, movb);}
void testMovAxIw0x0b8() {cpu->big = false;EwRegIw(0xb8, &cpu->reg[0], 0, movw);}
void testMovEaxId0x2b8() {cpu->big = true;EdRegId(0xb8, &cpu->reg[0], 0, movd);}
void testMovCxIw0x0b9() {cpu->big = false;EwRegIw(0xb9, &cpu->reg[1], 1, movw);}
void testMovEcxId0x2b9() {cpu->big = true;EdRegId(0xb9, &cpu->reg[1], 1, movd);}
void testMovDxIw0x0ba() {cpu->big = false;EwRegIw(0xba, &cpu->reg[2], 2, movw);}
void testMovEdxId0x2ba() {cpu->big = true;EdRegId(0xba, &cpu->reg[2], 2, movd);}
void testMovBxIw0x0bb() {cpu->big = false;EwRegIw(0xbb, &cpu->reg[3], 3, movw);}
void testMovEbxId0x2bb() {cpu->big = true;EdRegId(0xbb, &cpu->reg[3], 3, movd);}
void testMovSpIw0x0bc() {cpu->big = false;EwRegIw(0xbc, &cpu->reg[4], 4, movw);}
void testMovEspId0x2bc() {cpu->big = true;EdRegId(0xbc, &cpu->reg[4], 4, movd);}
void testMovBpIw0x0bd() {cpu->big = false;EwRegIw(0xbd, &cpu->reg[5], 5, movw);}
void testMovEbpId0x2bd() {cpu->big = true;EdRegId(0xbd, &cpu->reg[5], 5, movd);}
void testMovSiIw0x0be() {cpu->big = false;EwRegIw(0xbe, &cpu->reg[6], 6, movw);}
void testMovEsiId0x2be() {cpu->big = true;EdRegId(0xbe, &cpu->reg[6], 6, movd);}
void testMovDiIw0x0bf() {cpu->big = false;EwRegIw(0xbf, &cpu->reg[7], 7, movw);}
void testMovEdiId0x2bf() {cpu->big = true;EdRegId(0xbf, &cpu->reg[7], 7, movd);}

void testGrp20x0c0() {
    cpu->big = false;
    EbIb(0xC0, 0, rolb);
    X86_TEST1C(rol, &rolb[0], al, 1) 
    X86_TEST1C(rol, &rolb[1], al, 1) 
    X86_TEST1C(rol, &rolb[2], al, 1) 
    X86_TEST1C(rol, &rolb[3], al, 4) 
    X86_TEST1C(rol, &rolb[4], al, 12) 
    X86_TEST1C(rol, &rolb[5], al, 0) 
    X86_TEST1C(rol, &rolb[6], al, 8)
    X86_TEST1C(rol, &rolb[7], al, 8)
    X86_TEST1C(rol, &rolb[8], al, 9)
    X86_TEST1C(rol, &rolb[9], al, 32)

    EbIb(0xC0, 1, rorb);
    X86_TEST1C(ror, &rorb[0], al, 1) 
    X86_TEST1C(ror, &rorb[1], al, 1) 
    X86_TEST1C(ror, &rorb[2], al, 1) 
    X86_TEST1C(ror, &rorb[3], al, 4) 
    X86_TEST1C(ror, &rorb[4], al, 12) 
    X86_TEST1C(ror, &rorb[5], al, 0) 
    X86_TEST1C(ror, &rorb[6], al, 8)
    X86_TEST1C(ror, &rorb[7], al, 8)
    X86_TEST1C(ror, &rorb[8], al, 9)
    X86_TEST1C(ror, &rorb[9], al, 32)

    EbIb(0xC0, 2, rclb);
    X86_TEST1C(rcl, &rclb[0], al, 1) 
    X86_TEST1C(rcl, &rclb[1], al, 1) 
    X86_TEST1C(rcl, &rclb[2], al, 1) 
    X86_TEST1C(rcl, &rclb[3], al, 5) 
    X86_TEST1C(rcl, &rclb[4], al, 14) 
    X86_TEST1C(rcl, &rclb[5], al, 0) 
    X86_TEST1C(rcl, &rclb[6], al, 9)
    X86_TEST1C(rcl, &rclb[7], al, 9)
    X86_TEST1C(rcl, &rclb[8], al, 10)
    X86_TEST1C(rcl, &rclb[9], al, 32)
    X86_TEST1C(rcl, &rclb[10], al, 1)
    X86_TEST1C(rcl, &rclb[11], al, 2)

    EbIb(0xC0, 3, rcrb);
    X86_TEST1C(rcr, &rcrb[0], al, 1) 
    X86_TEST1C(rcr, &rcrb[1], al, 1) 
    X86_TEST1C(rcr, &rcrb[2], al, 1) 
    X86_TEST1C(rcr, &rcrb[3], al, 5) 
    X86_TEST1C(rcr, &rcrb[4], al, 14) 
    X86_TEST1C(rcr, &rcrb[5], al, 0) 
    X86_TEST1C(rcr, &rcrb[6], al, 9)
    X86_TEST1C(rcr, &rcrb[7], al, 9)
    X86_TEST1C(rcr, &rcrb[8], al, 10)
    X86_TEST1C(rcr, &rcrb[9], al, 32)
    X86_TEST1C(rcr, &rcrb[10], al, 1)
    X86_TEST1C(rcr, &rcrb[11], al, 2)

    EbIb(0xC0, 4, shlb);
    X86_TEST1C(shl, &shlb[0], al, 1) 
    X86_TEST1C(shl, &shlb[1], al, 1) 
    X86_TEST1C(shl, &shlb[2], al, 1) 
    X86_TEST1C(shl, &shlb[3], al, 4) 
    X86_TEST1C(shl, &shlb[4], al, 12) 
    X86_TEST1C(shl, &shlb[5], al, 0) 
    X86_TEST1C(shl, &shlb[6], al, 32)

    EbIb(0xC0, 5, shrb);
    X86_TEST1C(shr, &shrb[0], al, 1) 
    X86_TEST1C(shr, &shrb[1], al, 1) 
    X86_TEST1C(shr, &shrb[2], al, 1) 
    X86_TEST1C(shr, &shrb[3], al, 1) 
    X86_TEST1C(shr, &shrb[4], al, 4) 
    X86_TEST1C(shr, &shrb[5], al, 12) 
    X86_TEST1C(shr, &shrb[6], al, 0)
    X86_TEST1C(shr, &shrb[7], al, 32)

    EbIb(0xC0, 6, shlb);

    EbIb(0xC0, 7, sarb);
    X86_TEST1C(sar, &sarb[0], al, 1) 
    X86_TEST1C(sar, &sarb[1], al, 1) 
    X86_TEST1C(sar, &sarb[2], al, 1) 
    X86_TEST1C(sar, &sarb[3], al, 7) 
    X86_TEST1C(sar, &sarb[4], al, 1) 
    X86_TEST1C(sar, &sarb[5], al, 4) 
    X86_TEST1C(sar, &sarb[6], al, 12)
    X86_TEST1C(sar, &sarb[7], al, 0)
    X86_TEST1C(sar, &sarb[8], al, 32)
}

void testGrp20x2c0() {
    cpu->big = true;
    EbIb(0xC0, 0, rolb);
    EbIb(0xC0, 1, rorb);
    EbIb(0xC0, 2, rclb);
    EbIb(0xC0, 3, rcrb);
    EbIb(0xC0, 4, shlb);
    EbIb(0xC0, 5, shrb);
    EbIb(0xC0, 6, shlb);
    EbIb(0xC0, 7, sarb);
}

void testGrp20x0c1() {
    cpu->big = false;
    EwIb(0xC1, 0, rolw);
    EwIb(0xC1, 1, rorw);
    EwIb(0xC1, 2, rclw);
    EwIb(0xC1, 3, rcrw);
    EwIb(0xC1, 4, shlw);
    EwIb(0xC1, 5, shrw);
    EwIb(0xC1, 6, shlw);
    EwIb(0xC1, 7, sarw);
}

void testGrp20x2c1() {
    cpu->big = true;
    EdIb(0xC1, 0, rold);
    EdIb(0xC1, 1, rord);
    EdIb(0xC1, 2, rcld);
    EdIb(0xC1, 3, rcrd);
    EdIb(0xC1, 4, shld);
    EdIb(0xC1, 5, shrd);
    EdIb(0xC1, 6, shld);
    EdIb(0xC1, 7, sard);
}

void testMovEbIb0x0c6() {cpu->big = false;EbIb(0xc6, 0, movb);}
void testMovEbIb0x2c6() {cpu->big = true;EbIb(0xc6, 0, movb);}
void testMovEwIw0x0c7() {cpu->big = false;EwIw(0xc7, 0, movw);}
void testMovEdId0x2c7() {cpu->big = true;EdId(0xc7, 0, movd);}

void testGrp20x0d0() {
    cpu->big = false;
    Eb(0xD0, 0, rolb_1);
    Eb(0xD0, 1, rorb_1);
    Eb(0xD0, 2, rclb_1);
    Eb(0xD0, 3, rcrb_1);
    Eb(0xD0, 4, shlb_1);
    Eb(0xD0, 5, shrb_1);
    Eb(0xD0, 6, shlb_1);
    Eb(0xD0, 7, sarb_1);
}

void testGrp20x2d0() {
    cpu->big = true;
    Eb(0xD0, 0, rolb_1);
    Eb(0xD0, 1, rorb_1);
    Eb(0xD0, 2, rclb_1);
    Eb(0xD0, 3, rcrb_1);
    Eb(0xD0, 4, shlb_1);
    Eb(0xD0, 5, shrb_1);
    Eb(0xD0, 6, shlb_1);
    Eb(0xD0, 7, sarb_1);
}

void testGrp20x0d1() {
    cpu->big = false;
    Ew(0xD1, 0, rolw_1);
    Ew(0xD1, 1, rorw_1);
    Ew(0xD1, 2, rclw_1);
    Ew(0xD1, 3, rcrw_1);
    Ew(0xD1, 4, shlw_1);
    Ew(0xD1, 5, shrw_1);
    Ew(0xD1, 6, shlw_1);
    Ew(0xD1, 7, sarw_1);
}

void testGrp20x2d1() {
    cpu->big = true;
    Ed(0xD1, 0, rold_1);
    Ed(0xD1, 1, rord_1);
    Ed(0xD1, 2, rcld_1);
    Ed(0xD1, 3, rcrd_1);
    Ed(0xD1, 4, shld_1);
    Ed(0xD1, 5, shrd_1);
    Ed(0xD1, 6, shld_1);
    Ed(0xD1, 7, sard_1);
}

void testGrp20x0d2() {
    cpu->big = false;
    EbCl(0xD2, 0, rolb);
    EbCl(0xD2, 1, rorb);
    EbCl(0xD2, 2, rclb);
    EbCl(0xD2, 3, rcrb);
    EbCl(0xD2, 4, shlb);
    EbCl(0xD2, 5, shrb);
    EbCl(0xD2, 6, shlb);
    EbCl(0xD2, 7, sarb);
}

void testGrp20x2d2() {
    cpu->big = true;
    EbCl(0xD2, 0, rolb);
    EbCl(0xD2, 1, rorb);
    EbCl(0xD2, 2, rclb);
    EbCl(0xD2, 3, rcrb);
    EbCl(0xD2, 4, shlb);
    EbCl(0xD2, 5, shrb);
    EbCl(0xD2, 6, shlb);
    EbCl(0xD2, 7, sarb);
}

void testGrp20x0d3() {
    cpu->big = false;
    EwCl(0xD3, 0, rolw);
    EwCl(0xD3, 1, rorw);
    EwCl(0xD3, 2, rclw);
    EwCl(0xD3, 3, rcrw);
    EwCl(0xD3, 4, shlw);
    EwCl(0xD3, 5, shrw);
    EwCl(0xD3, 6, shlw);
    EwCl(0xD3, 7, sarw);
}

void testGrp20x2d3() {
    cpu->big = true;
    EdCl(0xD3, 0, rold);
    EdCl(0xD3, 1, rord);
    EdCl(0xD3, 2, rcld);
    EdCl(0xD3, 3, rcrd);
    EdCl(0xD3, 4, shld);
    EdCl(0xD3, 5, shrd);
    EdCl(0xD3, 6, shld);
    EdCl(0xD3, 7, sard);
}

void testAam0x0d4() {cpu->big = false;EwRegIb(0xd4, 0, aam);X86_TEST0(aam, aam)}
void testAam0x2d4() {cpu->big = true;EwRegIb(0xd4, 0, aam);}

void testAad0x0d5() {cpu->big = false;EwRegIb(0xd5, 0, aad);X86_TEST0(aad, aad)}
void testAad0x2d5() {cpu->big = true;EwRegIb(0xd5, 0, aad);}

void testSalc0x0d6() {cpu->big=false; EbReg(0xd6, 0, salc);}
void testSalc0x2d6() {cpu->big=true; EbReg(0xd6, 0, salc);}

void testXlat0x0d7() {
    U32 result;

    cpu->big=false;
    newInstruction(0xd7, 0);
    EBX = DEFAULT;
    EAX = DEFAULT;
    writed(cpu->seg[DS].address + 2000, DEFAULT);
    writed(cpu->seg[DS].address + 2004, DEFAULT);
    writed(cpu->seg[DS].address + 2008, DEFAULT);
    BX = 2000;
    AL=4;
    writeb(cpu->seg[DS].address + 2004, 0x08);
    runTestCPU();
    result = DEFAULT;
    result&=~0xFF;
    result+=8;
    assertTrue(EAX == result);
}

void testXlat0x2d7() {
    U32 result;

    cpu->big=true;
    newInstruction(0xd7, 0);
    EAX = DEFAULT;
    writed(cpu->seg[DS].address + 2000, DEFAULT);
    writed(cpu->seg[DS].address + 2004, DEFAULT);
    writed(cpu->seg[DS].address + 2008, DEFAULT);
    EBX=2000;
    AL=4;
    writeb(cpu->seg[DS].address + 2004, 0x08);
    runTestCPU();
    result = DEFAULT;
    result&=~0xFF;
    result+=8;
    assertTrue(EAX == result);
}

#include <math.h>
const U32 FLOAT_POSITIVE_INFINITY_BITS = 0x7f800000;
const U32 FLOAT_NEGATIVE_INFINITY_BITS = 0xff800000;
const U32 FLOAT_NAN_BITS = 0x7fd00000;

#ifdef BOXEDWINE_MSVC
#include <float.h>
//#define isnan(x) _isnan(x)
//#define isinf(x) (!_finite(x))
#endif

 static U8 rm(int ea, int group, int sub) {
    int result = (group & 7) << 3 | (sub & 7);
    if (!ea)
        result |= 0xC0;
    return (U8)result;
}

int getStackPos() {
    newInstruction(0xdf, 0);
    pushCode8(rm(false, 4, 0));
    runTestCPU();
    return (AX & 0x3800) >> 11;

}

struct FPU_Float {
    union {
        float f;
        U32   i;
    };
};

float getTopFloat() {
    struct FPU_Float result;
    newInstruction(0xd9, 0);	
    pushCode8(rm(true, 2, cpu->big?5:6));
    if (cpu->big)
        pushCode32(4);
    else
        pushCode16(4);
    runTestCPU();
    result.i = readd(HEAP_ADDRESS+4);
    return result.f;
}

void writeF(float f) {
    struct FPU_Float value;
    value.f = f;    
    writed(HEAP_ADDRESS, 0xCDCDCDCD);
    writed(HEAP_ADDRESS+4, value.i);
    writed(HEAP_ADDRESS+8, 0xCDCDCDCD);
}

void fldf32(float f) {
    int rm = 0;    
    if (cpu->big)
        rm += 5;
    else
        rm += 6;
    newInstructionWithRM(0xd9, rm, 0);
    if (cpu->big)
        pushCode32(4);
    else
        pushCode16(4);
    writeF(f);
    runTestCPU();
}

void fpu_init() {
    newInstruction(0xdb, 0);
    pushCode8(rm(false, 4, 3));
    runTestCPU();
}

void doF32Instruction(int op1, int group1, int op2, int group2, float x, float y, float r) {
    float result; 

    fpu_init();

    fldf32(x);
    writeF(y);

    newInstruction(op1, 0);
    pushCode8(rm(true, group1, cpu->big?5:6));
    if (cpu->big)
        pushCode32(4);
    else
        pushCode16(4);
    runTestCPU();
    result = getTopFloat();
    assertTrue((isnan(result) && isnan(r)) || result == r);
    assertTrue(getStackPos()==7); // nothing was popped

    fpu_init();

    fldf32(y);
    fldf32(x);
    newInstruction(op2, 0);
    pushCode8(rm(false, group2, 1));
    runTestCPU();
    result = getTopFloat();
    assertTrue((isnan(result) && isnan(r)) || result == r);
    assertTrue(getStackPos()==6); // nothing was popped
}

void F32Add(float x, float y, float r) {
    doF32Instruction(0xd8, 0, 0xd8, 0, x, y, r);
}

void doF32Add() {
    F32Add(0.0f, 0.0f, 0.0f);
    F32Add(-0.0f, 0.0f, 0.0f);
    F32Add(0.0f, -0.0f, 0.0f);

    F32Add(0.0f, POSITIVE_INFINITY, POSITIVE_INFINITY);
    F32Add(POSITIVE_INFINITY, 0.0f, POSITIVE_INFINITY);
    F32Add(0.0f, NEGATIVE_INFINITY, NEGATIVE_INFINITY);
    F32Add(NEGATIVE_INFINITY, 0.0f, NEGATIVE_INFINITY);
    F32Add(NEGATIVE_INFINITY, POSITIVE_INFINITY, TEST_NAN);
    F32Add(POSITIVE_INFINITY, 1.0f, POSITIVE_INFINITY);
    F32Add(POSITIVE_INFINITY, 2.0f, POSITIVE_INFINITY);
    F32Add(POSITIVE_INFINITY, POSITIVE_INFINITY, POSITIVE_INFINITY);

    F32Add(TEST_NAN, 2.0f, TEST_NAN);
    F32Add(TEST_NAN, TEST_NAN, TEST_NAN);
    F32Add(-2.0f, TEST_NAN, TEST_NAN);

    F32Add(0.0f, 1.0f, 1.0f);
    F32Add(1.0f, 0.0f, 1.0f);
    F32Add(0.0f, -1.0f, -1.0f);
    F32Add(-1.0f, 0.0f, -1.0f);
    F32Add(-1.0f, 1.0f, 0.0f);
    F32Add(1.0f, -1.0f, 0.0f);
    F32Add(-1.0f, -1.0f, -2.0f);
    F32Add(1.0f, 1.0f, 2.0f);

    F32Add(100.01f, 0.001f, 100.011f);
}

void testF32Add() {
    doF32Add();
}

void F32Sub(float x, float y, float r) {
    doF32Instruction(0xd8, 4, 0xd8, 4, x, y, r);
}

void doF32Sub() {
    F32Sub(0.0f, 0.0f, 0.0f);
    F32Sub(-0.0f, 0.0f, 0.0f);
    F32Sub(0.0f, -0.0f, 0.0f);

    F32Sub(0.0f, POSITIVE_INFINITY, NEGATIVE_INFINITY);
    F32Sub(POSITIVE_INFINITY, 0.0f, POSITIVE_INFINITY);
    F32Sub(0.0f, NEGATIVE_INFINITY, POSITIVE_INFINITY);
    F32Sub(NEGATIVE_INFINITY, 0.0f, NEGATIVE_INFINITY);
    F32Sub(NEGATIVE_INFINITY, POSITIVE_INFINITY, NEGATIVE_INFINITY);
    F32Sub(POSITIVE_INFINITY, 1.0f, POSITIVE_INFINITY);
    F32Sub(POSITIVE_INFINITY, 2.0f, POSITIVE_INFINITY);
    F32Sub(POSITIVE_INFINITY, POSITIVE_INFINITY, TEST_NAN);

    F32Sub(TEST_NAN, 2.0f, TEST_NAN);
    F32Sub(TEST_NAN, TEST_NAN, TEST_NAN);
    F32Sub(-2.0f, TEST_NAN, TEST_NAN);

    F32Sub(0.0f, 1.0f, -1.0f);
    F32Sub(1.0f, 0.0f, 1.0f);
    F32Sub(0.0f, -1.0f, 1.0f);
    F32Sub(-1.0f, 0.0f, -1.0f);
    F32Sub(-1.0f, 1.0f, -2.0f);
    F32Sub(1.0f, -1.0f, 2.0f);
    F32Sub(-1.0f, -1.0f, 0.0f);
    F32Sub(1.0f, 1.0f, 0.0f);

    F32Sub(100.01f, 0.001f, 100.009f);
}

void testF32Sub() {
    doF32Sub();
}

void F32SubR(float x, float y, float r) {
    doF32Instruction(0xd8, 5, 0xd8, 5, x, y, r);
}

void doF32SubR() {
    F32SubR(0.0f, 0.0f, 0.0f);
    F32SubR(-0.0f, 0.0f, 0.0f);
    F32SubR(0.0f, -0.0f, 0.0f);

    F32SubR(0.0f, POSITIVE_INFINITY, POSITIVE_INFINITY);
    F32SubR(POSITIVE_INFINITY, 0.0f, NEGATIVE_INFINITY);
    F32SubR(0.0f, NEGATIVE_INFINITY, NEGATIVE_INFINITY);
    F32SubR(NEGATIVE_INFINITY, 0.0f, POSITIVE_INFINITY);
    F32SubR(NEGATIVE_INFINITY, POSITIVE_INFINITY, POSITIVE_INFINITY);
    F32SubR(POSITIVE_INFINITY, 1.0f, NEGATIVE_INFINITY);
    F32SubR(POSITIVE_INFINITY, 2.0f, NEGATIVE_INFINITY);
    F32SubR(POSITIVE_INFINITY, POSITIVE_INFINITY, TEST_NAN);

    F32SubR(TEST_NAN, 2.0f, TEST_NAN);
    F32SubR(TEST_NAN, TEST_NAN, TEST_NAN);
    F32SubR(-2.0f, TEST_NAN, TEST_NAN);

    F32SubR(0.0f, 1.0f, 1.0f);
    F32SubR(1.0f, 0.0f, -1.0f);
    F32SubR(0.0f, -1.0f, -1.0f);
    F32SubR(-1.0f, 0.0f, 1.0f);
    F32SubR(-1.0f, 1.0f, 2.0f);
    F32SubR(1.0f, -1.0f, -2.0f);
    F32SubR(-1.0f, -1.0f, 0.0f);
    F32SubR(1.0f, 1.0f, 0.0f);

    F32SubR(100.01f, 0.001f, -100.009f);
}

void testF32SubR() {
    doF32SubR();
}

void F32Mul(float x, float y, float r) {
    doF32Instruction(0xd8, 1, 0xd8, 1, x, y, r);
}

void doF32Mul() {
    F32Mul(0.0f, 0.0f, 0.0f);
    F32Mul(-0.0f, 0.0f, 0.0f);
    F32Mul(0.0f, -0.0f, 0.0f);

    F32Mul(0.0f, POSITIVE_INFINITY, TEST_NAN);
    F32Mul(POSITIVE_INFINITY, 0.0f, TEST_NAN);
    F32Mul(0.0f, NEGATIVE_INFINITY, TEST_NAN);
    F32Mul(NEGATIVE_INFINITY, 0.0f, TEST_NAN);
    F32Mul(NEGATIVE_INFINITY, POSITIVE_INFINITY, NEGATIVE_INFINITY);
    F32Mul(POSITIVE_INFINITY, 1.0f, POSITIVE_INFINITY);
    F32Mul(POSITIVE_INFINITY, 2.0f, POSITIVE_INFINITY);
    F32Mul(POSITIVE_INFINITY, POSITIVE_INFINITY, POSITIVE_INFINITY);

    F32Mul(TEST_NAN, 2.0f, TEST_NAN);
    F32Mul(TEST_NAN, TEST_NAN, TEST_NAN);
    F32Mul(-2.0f, TEST_NAN, TEST_NAN);

    F32Mul(0.0f, 1.0f, 0.0f);
    F32Mul(1.0f, 0.0f, 0.0f);
    F32Mul(0.0f, -1.0f, 0.0f);
    F32Mul(-1.0f, 0.0f, 0.0f);
    F32Mul(-1.0f, 1.0f, -1.0f);
    F32Mul(1.0f, -1.0f, -1.0f);
    F32Mul(-1.0f, -1.0f, 1.0f);
    F32Mul(1.0f, 1.0f, 1.0f);

    F32Mul(100.01f, 0.001f, .10001001f);
}

void testF32Mul() {
    doF32Mul();
}

void F32Div(float x, float y, float r) {
    doF32Instruction(0xd8, 6, 0xd8, 6, x, y, r);
}

void doF32Div() {
    F32Div(0.0f, 0.0f, TEST_NAN);
    F32Div(-0.0f, 0.0f, TEST_NAN);
    F32Div(0.0f, -0.0f, TEST_NAN);

    F32Div(0.0f, POSITIVE_INFINITY, 0.0f);
    F32Div(POSITIVE_INFINITY, 0.0f, POSITIVE_INFINITY);
    F32Div(0.0f, NEGATIVE_INFINITY, -0.0f);
    F32Div(NEGATIVE_INFINITY, 0.0f, NEGATIVE_INFINITY);
    F32Div(NEGATIVE_INFINITY, POSITIVE_INFINITY, TEST_NAN);
    F32Div(POSITIVE_INFINITY, 1.0f, POSITIVE_INFINITY);
    F32Div(POSITIVE_INFINITY, 2.0f, POSITIVE_INFINITY);
    F32Div(POSITIVE_INFINITY, POSITIVE_INFINITY, TEST_NAN);

    F32Div(TEST_NAN, 2.0f, TEST_NAN);
    F32Div(TEST_NAN, TEST_NAN, TEST_NAN);
    F32Div(-2.0f, TEST_NAN, TEST_NAN);

    F32Div(0.0f, 1.0f, 0.0f);
    F32Div(1.0f, 0.0f, POSITIVE_INFINITY);
    F32Div(0.0f, -1.0f, 0.0f);
    F32Div(-1.0f, 0.0f, NEGATIVE_INFINITY);
    F32Div(-1.0f, 1.0f, -1.0f);
    F32Div(1.0f, -1.0f, -1.0f);
    F32Div(-1.0f, -1.0f, 1.0f);
    F32Div(1.0f, 1.0f, 1.0f);

    F32Div(100.01f, 0.001f, 100010.0f);
}

void testF32Div() {
    doF32Div();
}

void F32DivR(float x, float y, float r) {
    doF32Instruction(0xd8, 7, 0xd8, 7, x, y, r);
}

void doF32DivR() {
    F32DivR(0.0f, 0.0f, TEST_NAN);
    F32DivR(-0.0f, 0.0f, TEST_NAN);
    F32DivR(0.0f, -0.0f, TEST_NAN);

    F32DivR(0.0f, POSITIVE_INFINITY, POSITIVE_INFINITY);
    F32DivR(POSITIVE_INFINITY, 0.0f, 0.0f);
    F32DivR(0.0f, NEGATIVE_INFINITY, NEGATIVE_INFINITY);
    F32DivR(NEGATIVE_INFINITY, 0.0f, -0.0f);
    F32DivR(NEGATIVE_INFINITY, POSITIVE_INFINITY, TEST_NAN);
    F32DivR(POSITIVE_INFINITY, 1.0f, 0.0f);
    F32DivR(POSITIVE_INFINITY, 2.0f, 0.0f);
    F32DivR(POSITIVE_INFINITY, POSITIVE_INFINITY, TEST_NAN);

    F32DivR(TEST_NAN, 2.0f, TEST_NAN);
    F32DivR(TEST_NAN, TEST_NAN, TEST_NAN);
    F32DivR(-2.0f, TEST_NAN, TEST_NAN);

    F32DivR(0.0f, 1.0f, POSITIVE_INFINITY);
    F32DivR(1.0f, 0.0f, 0.0f);
    F32DivR(0.0f, -1.0f, NEGATIVE_INFINITY);
    F32DivR(-1.0f, 0.0f, 0.0f);
    F32DivR(-1.0f, 1.0f, -1.0f);
    F32DivR(1.0f, -1.0f, -1.0f);
    F32DivR(-1.0f, -1.0f, 1.0f);
    F32DivR(1.0f, 1.0f, 1.0f);

    F32DivR(100.01f, 0.001f, .000009999f);
}

void testF32DivR() {
    doF32DivR();
}

int UNORDERED = 0x100 | 0x400 | 0x4000;
int LESS = 0x100;
int GREATER = 0x0;
int EQUAL = 0x4000;
int MASK = 0x100 | 0x200 | 0x400 | 0x4000;

void assertTest(int r) {
    newInstruction(0xdf, 0);
    pushCode8(rm(false, 4, 0));
    runTestCPU();
    assertTrue((AX & MASK) == r);
}

void F32ComBase(int op, int group, float x, float y, int r, int popCount) {
    fpu_init();

    fldf32(x);
    writeF(y);

    newInstruction(op, 0);
    pushCode8(rm(true, group, cpu->big?5:6));
    if (cpu->big)
        pushCode32(4);
    else
        pushCode16(4);
    runTestCPU();
    assertTest(r);
    assertTrue(getStackPos()==((7+popCount)&7));

    fpu_init();

    fldf32(y);
    fldf32(x);
    newInstruction(op, 0);
    pushCode8(rm(false, group, 1));
    runTestCPU();
    assertTest(r);
    assertTrue(getStackPos()==((6+popCount)&7));
}
void F32Com(float x, float y, int r) {
    F32ComBase(0xd8, 2, x, y, r, 0);
}

void doF32Com() {
    F32Com(0.0f, 0.0f, EQUAL);
    F32Com(-0.0f, 0.0f, EQUAL);
    F32Com(0.0f, -0.0f, EQUAL);

    F32Com(0.0f, POSITIVE_INFINITY, LESS);
    F32Com(POSITIVE_INFINITY, 0.0f, GREATER);
    F32Com(0.0f, NEGATIVE_INFINITY, GREATER);
    F32Com(NEGATIVE_INFINITY, 0.0f, LESS);
    F32Com(NEGATIVE_INFINITY, POSITIVE_INFINITY, LESS);
    F32Com(POSITIVE_INFINITY, 1.0f, GREATER);
    F32Com(POSITIVE_INFINITY, 2.0f, GREATER);
    F32Com(POSITIVE_INFINITY, POSITIVE_INFINITY, EQUAL);

    F32Com(TEST_NAN, 2.0f, UNORDERED);
    F32Com(TEST_NAN, TEST_NAN, UNORDERED);
    F32Com(-2.0f, TEST_NAN, UNORDERED);

    F32Com(0.0f, 1.0f, LESS);
    F32Com(1.0f, 0.0f, GREATER);
    F32Com(0.0f, -1.0f, GREATER);
    F32Com(-1.0f, 0.0f, LESS);
    F32Com(-1.0f, 1.0f, LESS);
    F32Com(1.0f, -1.0f, GREATER);
    F32Com(-1.0f, -1.0f, EQUAL);
    F32Com(1.0f, 1.0f, EQUAL);

    F32Com(100.01f, 0.001f, GREATER);
}

void testF32Com() {
    doF32Com();
}

void F32ComP(float x, float y, int r) {
    F32ComBase(0xd8, 3, x, y, r, 1);
}

void doF32ComP() {
    F32ComP(0.0f, 0.0f, EQUAL);
    F32ComP(-0.0f, 0.0f, EQUAL);
    F32ComP(0.0f, -0.0f, EQUAL);

    F32ComP(0.0f, POSITIVE_INFINITY, LESS);
    F32ComP(POSITIVE_INFINITY, 0.0f, GREATER);
    F32ComP(0.0f, NEGATIVE_INFINITY, GREATER);
    F32ComP(NEGATIVE_INFINITY, 0.0f, LESS);
    F32ComP(NEGATIVE_INFINITY, POSITIVE_INFINITY, LESS);
    F32ComP(POSITIVE_INFINITY, 1.0f, GREATER);
    F32ComP(POSITIVE_INFINITY, 2.0f, GREATER);
    F32ComP(POSITIVE_INFINITY, POSITIVE_INFINITY, EQUAL);

    F32ComP(TEST_NAN, 2.0f, UNORDERED);
    F32ComP(TEST_NAN, TEST_NAN, UNORDERED);
    F32ComP(-2.0f, TEST_NAN, UNORDERED);

    F32ComP(0.0f, 1.0f, LESS);
    F32ComP(1.0f, 0.0f, GREATER);
    F32ComP(0.0f, -1.0f, GREATER);
    F32ComP(-1.0f, 0.0f, LESS);
    F32ComP(-1.0f, 1.0f, LESS);
    F32ComP(1.0f, -1.0f, GREATER);
    F32ComP(-1.0f, -1.0f, EQUAL);
    F32ComP(1.0f, 1.0f, EQUAL);

    F32ComP(100.01f, 0.001f, GREATER);
}

void testF32ComP() {
    doF32ComP();
}

void testFPUD8() {
    testF32Add();
    testF32Sub();
    testF32SubR();
    testF32Mul();
    testF32Div();
    testF32DivR();
    testF32Com();
    testF32ComP();
}

void FSTFloat(int op, int group, float f, int pop) {
    struct FPU_Float result;
    fpu_init();

    fldf32(f);
    writed(HEAP_ADDRESS+4, 0xCDCDCDCD);

    newInstruction(op, 0);
    pushCode8(rm(true, group, cpu->big?5:6));
    if (cpu->big)
        pushCode32(4);
    else
        pushCode16(4);
    runTestCPU();
    result.i = readd(HEAP_ADDRESS+4);
    assertTrue(result.f==f || (isnan(result.f) && isnan(f)));
    assertTrue(getStackPos()==(pop?0:7));
}

void doFSTFloat(int op, int group, int pop) {
    FSTFloat(op, group, 0.0f, pop);
    FSTFloat(op, group, 1.0f, pop);
    FSTFloat(op, group, -1.0f, pop);
    FSTFloat(op, group, 0.00001f, pop);
    FSTFloat(op, group, -0.00001f, pop);
    FSTFloat(op, group, 1010.01f, pop);
    FSTFloat(op, group, -1010.01f, pop);
    FSTFloat(op, group, TEST_NAN, pop);
    FSTFloat(op, group, POSITIVE_INFINITY, pop);
    FSTFloat(op, group, NEGATIVE_INFINITY, pop);
}

void testFSTFloat() {
    doFSTFloat(0xd9, 2, false);
}

void testFSTPFloat() {
    doFSTFloat(0xd9, 3, true);
}

void doFLDSti() {
    fldf32(4.0f);
    fldf32(3.0f);
    fldf32(2.0f);
    fldf32(1.0f);

    newInstruction(0xd9, 0);
    pushCode8(rm(false, 0, 0));
    runTestCPU();
    assertTrue(getTopFloat()==1.0f);

    newInstruction(0xd9, 0);
    pushCode8(rm(false, 0, 2));
    runTestCPU();
    assertTrue(getTopFloat()==2.0f);

    newInstruction(0xd9, 0);
    pushCode8(rm(false, 0, 4));
    runTestCPU();
    assertTrue(getTopFloat()==3.0f);

    newInstruction(0xd9, 0);
    pushCode8(rm(false, 0, 6));
    runTestCPU();
    assertTrue(getTopFloat()==4.0f);
}

void testFLDSTi() {
    doFLDSti();
}

void doFXCHSTi() {
    fldf32(4.0f);
    fldf32(3.0f);
    fldf32(2.0f);
    fldf32(1.0f);

    newInstruction(0xd9, 0);
    pushCode8(rm(false, 1, 3));
    runTestCPU();
    assertTrue(getTopFloat()==4.0f);
}

void testFXCHSTi() {
    doFXCHSTi();
}

void doFSTPSTi() {
    fldf32(4.0f);
    fldf32(3.0f);
    fldf32(2.0f);
    fldf32(1.0f);

    newInstruction(0xd9, 0);
    pushCode8(rm(false, 3, 2));
    runTestCPU();
    assertTrue(getTopFloat()==2.0f);

    newInstruction(0xd9, 0);
    pushCode8(rm(false, 3, 2));
    runTestCPU();
    assertTrue(getTopFloat()==1.0f);
}

void testFSTPSTi() {
    doFSTPSTi();
}

void doFCHS() {
    fldf32(432.1f);
    newInstruction(0xd9, 0);
    pushCode8(rm(false, 4, 0));
    runTestCPU();
    assertTrue(getTopFloat()==-432.1f);

    fldf32(-0.001234f);
    newInstruction(0xd9, 0);
    pushCode8(rm(false, 4, 0));
    runTestCPU();
    assertTrue(getTopFloat()==0.001234f);

    fldf32(TEST_NAN);
    newInstruction(0xd9, 0);
    pushCode8(rm(false, 4, 0));
    runTestCPU();
    assertTrue(isnan(getTopFloat()));

    fldf32(POSITIVE_INFINITY);
    newInstruction(0xd9, 0);
    pushCode8(rm(false, 4, 0));
    runTestCPU();
    assertTrue(getTopFloat()==NEGATIVE_INFINITY);

    fldf32(NEGATIVE_INFINITY);
    newInstruction(0xd9, 0);
    pushCode8(rm(false, 4, 0));
    runTestCPU();
    assertTrue(getTopFloat()==POSITIVE_INFINITY);
}

void testFCHS() {
    doFCHS();
}

void doFABS() {
    fldf32(432.1f);
    newInstruction(0xd9, 0);
    pushCode8(rm(false, 4, 1));
    runTestCPU();
    assertTrue(getTopFloat()==432.1f);

    fldf32(-0.001234f);
    newInstruction(0xd9, 0);
    pushCode8(rm(false, 4, 1));
    runTestCPU();
    assertTrue(getTopFloat()==0.001234f);

    fldf32(TEST_NAN);
    newInstruction(0xd9, 0);
    pushCode8(rm(false, 4, 1));
    runTestCPU();
    assertTrue(isnan(getTopFloat()));

    fldf32(POSITIVE_INFINITY);
    newInstruction(0xd9, 0);
    pushCode8(rm(false, 4, 1));
    runTestCPU();
    assertTrue(getTopFloat()==POSITIVE_INFINITY);

    fldf32(NEGATIVE_INFINITY);
    newInstruction(0xd9, 0);
    pushCode8(rm(false, 4, 1));
    runTestCPU();
    assertTrue(getTopFloat()==POSITIVE_INFINITY);
}

void testFABS() {
    doFABS();
}

void doFTST() {
    fldf32(432.1f);
    newInstruction(0xd9, 0);
    pushCode8(rm(false, 4, 4));
    runTestCPU();
    assertTest(GREATER);

    fldf32(-0.00001f);
    newInstruction(0xd9, 0);
    pushCode8(rm(false, 4, 4));
    runTestCPU();
    assertTest(LESS);

    fldf32(0.0f);
    newInstruction(0xd9, 0);
    pushCode8(rm(false, 4, 4));
    runTestCPU();
    assertTest(EQUAL);

    fldf32(POSITIVE_INFINITY);
    newInstruction(0xd9, 0);
    pushCode8(rm(false, 4, 4));
    runTestCPU();
    assertTest(GREATER);

    fldf32(NEGATIVE_INFINITY);
    newInstruction(0xd9, 0);
    pushCode8(rm(false, 4, 4));
    runTestCPU();
    assertTest(LESS);

    fldf32(TEST_NAN);
    newInstruction(0xd9, 0);
    pushCode8(rm(false, 4, 4));
    runTestCPU();
    assertTest(UNORDERED);
}

void testFTST() {
    doFTST();
}

void doFXAM() {
    fldf32(0.0f);
    newInstruction(0xd9, 0);
    pushCode8(rm(false, 4, 5));
    runTestCPU();
    assertTest(0x4000);

    fldf32(TEST_NAN);
    newInstruction(0xd9, 0);
    pushCode8(rm(false, 4, 5));
    runTestCPU();
    assertTest(0x100);

    fldf32(POSITIVE_INFINITY);
    newInstruction(0xd9, 0);
    pushCode8(rm(false, 4, 5));
    runTestCPU();
    assertTest(0x100 | 0x400);

    fldf32(NEGATIVE_INFINITY);
    newInstruction(0xd9, 0);
    pushCode8(rm(false, 4, 5));
    runTestCPU();
    assertTest(0x100 | 0x200 | 0x400);

    fldf32(1.0f);
    newInstruction(0xd9, 0);
    pushCode8(rm(false, 4, 5));
    runTestCPU();
    assertTest(0x400);

    fldf32(-2.0f);
    newInstruction(0xd9, 0);
    pushCode8(rm(false, 4, 5));
    runTestCPU();
    assertTest(0x200 | 0x400);
}

void testFXAM() {
    doFXAM();
}

void doFLD1() {
    newInstruction(0xd9, 0);
    pushCode8(rm(false, 5, 0));
    runTestCPU();
    assertTrue(getTopFloat() == 1.0f);
}

void testFLD1() {
    doFLD1();
}

void doFLDL2T() {
    newInstruction(0xd9, 0);
    pushCode8(rm(false, 5, 1));
    runTestCPU();
    assertTrue(getTopFloat() == 3.321928f);
}

void testFLDL2T() {
    doFLDL2T();
}

void doFLDL2E() {
    newInstruction(0xd9, 0);
    pushCode8(rm(false, 5, 2));
    runTestCPU();
    assertTrue(getTopFloat() == 1.442695f);
}

void testFLDL2E() {
    doFLDL2E();
}

void doFLDPI() {
    newInstruction(0xd9, 0);
    pushCode8(rm(false, 5, 3));
    runTestCPU();
    assertTrue(getTopFloat() == 3.1415927f);
}

void testFLDPI() {
    doFLDPI();
}

void doFLDLG2() {
    newInstruction(0xd9, 0);
    pushCode8(rm(false, 5, 4));
    runTestCPU();
    assertTrue(getTopFloat() == .30103f);
}

void testFLDLG2() {
    doFLDLG2();
}

void doFLDLN2() {
    newInstruction(0xd9, 0);
    pushCode8(rm(false, 5, 5));
    runTestCPU();
    assertTrue(getTopFloat() == 0.6931472f);
}

void testFLDLN2() {
    doFLDLN2();
}

void doFLDZ() {
    newInstruction(0xd9, 0);
    pushCode8(rm(false, 5, 6));
    runTestCPU();
    assertTrue(getTopFloat() == 0.0f);
}

void testFLDZ() {
    doFLDZ();
}

void doF2XM1() {
    fldf32(0.0f);
    newInstruction(0xd9, 0);
    pushCode8(rm(false, 6, 0));
    assertTrue(getTopFloat() == 0.0f);

    fldf32(TEST_NAN);
    newInstruction(0xd9, 0);
    pushCode8(rm(false, 6, 0));
    runTestCPU();
    assertTrue(isnan(getTopFloat()));

    fldf32(POSITIVE_INFINITY);
    newInstruction(0xd9, 0);
    pushCode8(rm(false, 6, 0));
    runTestCPU();
    assertTrue(getTopFloat() == POSITIVE_INFINITY);

    fldf32(NEGATIVE_INFINITY);
    newInstruction(0xd9, 0);
    pushCode8(rm(false, 6, 0));
    runTestCPU();
    assertTrue(getTopFloat() == -1.0f);

    fldf32(-1.0f);
    newInstruction(0xd9, 0);
    pushCode8(rm(false, 6, 0));
    runTestCPU();
    assertTrue(getTopFloat() == -0.5f);

    fldf32(1.0f);
    newInstruction(0xd9, 0);
    pushCode8(rm(false, 6, 0));
    runTestCPU();
    assertTrue(getTopFloat() == 1.0f);

    fldf32(-0.5f);
    newInstruction(0xd9, 0);
    pushCode8(rm(false, 6, 0));
    runTestCPU();
    assertTrue(getTopFloat() == -0.29289323f);
}

void testF2XM1() {
    doF2XM1();
}

void doFYL2X() {
    fldf32(1.0f);
    fldf32(0.0f);
    newInstruction(0xd9, 0);
    pushCode8(rm(false, 6, 1));
    runTestCPU();
    assertTrue(getTopFloat() == NEGATIVE_INFINITY);

    fpu_init();
    fldf32(2.0f);
    fldf32(1.0f);
    newInstruction(0xd9, 0);
    pushCode8(rm(false, 6, 1));
    runTestCPU();
    assertTrue(getTopFloat() == 0.0f);

    fpu_init();
    fldf32(8.0f);
    fldf32(2.5f);
    newInstruction(0xd9, 0);
    pushCode8(rm(false, 6, 1));
    runTestCPU();
    assertTrue(getTopFloat() == 10.575425f);

    fpu_init();
    fldf32(8.0f);
    fldf32(2.0f);
    newInstruction(0xd9, 0);
    pushCode8(rm(false, 6, 1));
    runTestCPU();
    assertTrue(getTopFloat() == 8.0f);

    fpu_init();
    fldf32(8.0f);
    fldf32(-2.0f);
    newInstruction(0xd9, 0);
    pushCode8(rm(false, 6, 1));
    runTestCPU();
    assertTrue(isnan(getTopFloat()));

    fpu_init();
    fldf32(10.0f);
    fldf32(8.0f);
    newInstruction(0xd9, 0);
    pushCode8(rm(false, 6, 1));
    runTestCPU();
    assertTrue(getTopFloat() == 30.0f);

    fpu_init();
    fldf32(10.0f);
    fldf32(TEST_NAN);
    newInstruction(0xd9, 0);
    pushCode8(rm(false, 6, 1));
    runTestCPU();
    assertTrue(isnan(getTopFloat()));

    fpu_init();
    fldf32(10.0f);
    fldf32(POSITIVE_INFINITY);
    newInstruction(0xd9, 0);
    pushCode8(rm(false, 6, 1));
    runTestCPU();
    assertTrue(getTopFloat() == POSITIVE_INFINITY);

    fpu_init();
    fldf32(10.0f);
    fldf32(NEGATIVE_INFINITY);
    newInstruction(0xd9, 0);
    pushCode8(rm(false, 6, 1));
    runTestCPU();
    assertTrue(isnan(getTopFloat()));
}

void testFYL2X() {
    doFYL2X();
}

void doFSQRT() {
    fpu_init();
    fldf32(0.0f);
    newInstruction(0xd9, 0);
    pushCode8(rm(false, 7, 2));
    runTestCPU();
    assertTrue(getTopFloat() == 0.0f);

    fpu_init();
    fldf32(1.0f);
    newInstruction(0xd9, 0);
    pushCode8(rm(false, 7, 2));
    runTestCPU();
    assertTrue(getTopFloat() == 1.0f);

    fpu_init();
    fldf32(2.0f);
    newInstruction(0xd9, 0);
    pushCode8(rm(false, 7, 2));
    runTestCPU();
    assertTrue(getTopFloat() == 1.4142135f);

    fpu_init();
    fldf32(4.0f);
    newInstruction(0xd9, 0);
    pushCode8(rm(false, 7, 2));
    runTestCPU();
    assertTrue(getTopFloat() == 2.0f);
}

void testFSQRT() {
    doFSQRT();
}

void testFPUD9() {
    // 0
    testFLDSTi();
    // 1
    testFXCHSTi();
    // 2 NOP

    // 3
    testFSTPSTi();

    // 4
    testFCHS();
    testFABS();
    testFTST();
    testFXAM();

    // 5
    testFLD1();
    testFLDL2T();
    testFLDL2E();
    testFLDPI();
    testFLDLG2();
    testFLDLN2();
    testFLDZ();

    // 6
    testF2XM1();
    testFYL2X();

    // 7
    testFSQRT();

    // ea
    testFSTFloat();
    testFSTPFloat();
}

void testFPU0x0d8() {cpu->big=false;testFPUD8();}
void testFPU0x2d8() {cpu->big=true;testFPUD8();}

void testFPU0x0d9() {cpu->big=false;testFPUD9();}
void testFPU0x2d9() {cpu->big=true;testFPUD9();}

void testCmc0x0f5() {cpu->big=false;EbReg(0xf5, 0, cmc);}
void testCmc0x2f5() {cpu->big=true;EbReg(0xf5, 0, cmc);}

void testGrp30x0f6() {
    cpu->big = false;
    EbIb(0xf6, 0, testb);
    EbIb(0xf6, 1, testb);
    Eb(0xf6, 2, notb);
    Eb(0xf6, 3, negb);
    EbAlAx(0xf6, 4, mulAl, false);
    EbAlAx(0xf6, 5, imulAl, false);
    EbAlAx(0xf6, 6, divAl, true);
    EbAlAx(0xf6, 7, idivAl, true);
}

void testGrp30x2f6() {
    cpu->big = true;
    EbIb(0xf6, 0, testb);
    EbIb(0xf6, 1, testb);
    Eb(0xf6, 2, notb);
    Eb(0xf6, 3, negb);
    EbAlAx(0xf6, 4, mulAl, false);
    EbAlAx(0xf6, 5, imulAl, false);
    EbAlAx(0xf6, 6, divAl, true);
    EbAlAx(0xf6, 7, idivAl, true);
}

void testGrp30x0f7() {
    cpu->big = false;
    EwIw(0xf7, 0, testw);
    EwIw(0xf7, 1, testw);
    Ew(0xf7, 2, notw);
    Ew(0xf7, 3, negw);
    EwAxDx(0xf7, 4, mulAx, false);
    EwAxDx(0xf7, 5, imulAx, false);
    EwAxDx(0xf7, 6, divAx, true);
    EwAxDx(0xf7, 7, idivAx, true);
}

void testGrp30x2f7() {
    cpu->big = true;
    EdId(0xf7, 0, testd);
    EdId(0xf7, 1, testd);
    Ed(0xf7, 2, notd);
    Ed(0xf7, 3, negd);
    EdEaxEdx(0xf7, 4, mulEax, false);
    EdEaxEdx(0xf7, 5, imulEax, false);
    EdEaxEdx(0xf7, 6, divEax, true);
    EdEaxEdx(0xf7, 7, idivEax, true);
}

void testClc0x0f8() {cpu->big=false;EbReg(0xf8, 0, clc);}
void testClc0x2f8() {cpu->big=true;EbReg(0xf8, 0, clc);}
void testStc0x0f8() {cpu->big=false;EbReg(0xf9, 0, stc);}
void testStc0x2f8() {cpu->big=true;EbReg(0xf9, 0, stc);}

void testGrp40x0fe() {
    cpu->big=false;
    Eb(0xfe, 0, incb);
    Eb(0xfe, 1, decb);
}

void testGrp40x2fe() {
    cpu->big=true;
    Eb(0xfe, 0, incb);
    Eb(0xfe, 1, decb);
}

void testGrp50x0ff() {
    cpu->big=false;
    Ew(0xff, 0, incw);
    Ew(0xff, 1, decw);
}

void testGrp50x2ff() {
    cpu->big=true;
    Ed(0xff, 0, incd);
    Ed(0xff, 1, decd);
}

void testBt0x1a3() {
    cpu->big=false;
    EwGwEffective(0x1a3, btw);
    X86_TEST(bt, btw, ax, cx);
}

void testBt0x3a3() {
    cpu->big=true;
    EdGdEffective(0x1a3, btd);
    X86_TEST(bt, btd, eax, ecx);
}

void testShld0x1a4() {
    cpu->big=false;
    EwGw(0x1a4, shld16);
}

void testShld0x3a4() {
    cpu->big=true;
    EdGd(0x1a4, shld32);
    X86_TEST2C(shld, &shld32[0], eax, ecx, 12);
    X86_TEST2C(shld, &shld32[1], eax, ecx, 1);
    X86_TEST2C(shld, &shld32[2], eax, ecx, 1);
    X86_TEST2C(shld, &shld32[3], eax, ecx, 1);
    X86_TEST2C(shld, &shld32[4], eax, ecx, 2);
    X86_TEST2C(shld, &shld32[5], eax, ecx, 40);
}

void testShld0x1a5() {
    cpu->big=false;
    EwGwCl(0x1a5, shld16);
}

void testShld0x3a5() {
    cpu->big=true;
    EdGdCl(0x1a5, shld32);
}

void testBts0x1ab() {
    cpu->big=false;
    EwGwEffective(0x1ab, btsw);
}

void testBts0x3ab() {
    cpu->big=true;
    EdGdEffective(0x1ab, btsd);
}

void testShrd0x1ac() {
    cpu->big=false;
    EwGw(0x1ac, shrd16);

    X86_TEST2C(shrd, &shrd16[0], ax, cx, 4);
    X86_TEST2C(shrd, &shrd16[1], ax, cx, 1);
    X86_TEST2C(shrd, &shrd16[2], ax, cx, 1);
    X86_TEST2C(shrd, &shrd16[3], ax, cx, 1);
    X86_TEST2C(shrd, &shrd16[4], ax, cx, 2);
    X86_TEST2C(shrd, &shrd16[5], ax, cx, 17);
    X86_TEST2C(shrd, &shrd16[6], ax, cx, 17);
}

void testShrd0x3ac() {
    cpu->big=true;
    EdGd(0x1ac, shrd32);
}

void testShrd0x1ad() {
    cpu->big=false;
    EwGwCl(0x1ad, shrd16);
}

void testShrd0x3ad() {
    cpu->big=true;
    EdGdCl(0x1ad, shrd32);
}

/*
int valid;
    U32 var1;
    U32 var2;
    U32 result;
    U32 resultvar2;
    U32 flags;
    U32 constant;    
    int fCF;
    int fOF;
    int fZF;
    int fSF;
    int dontUseResultAndCheckSFZF;
    int useResultvar2;
    int constantWidth;
    bool hasOF;
    int fAF;
    U32 hasAF;
    U32 hasCF;
    U32 hasZF;
    U32 hasSF;
    */

static struct Data cmpxchgd[] = {
    // 1, D, S, D(result), EAX(result), flags, EAX, CF, OF, ZF, SF, 0, 1, 0, 1, 0, 0, 1, 1, 1
      {1, 1, 2, 2        , 1          , 0    , 1  ,  0,  0,  1,  0, 0, 1, 0, 1, 0, 0, 1, 1, 1},
      {1, 1, 2, 1        , 1          , 0    , 9  ,  0,  0,  0,  0, 0, 1, 0, 1, 0, 0, 1, 1, 1},
      {1, 9, 2, 9        , 9          , 0    , 1  ,  1,  0,  0,  1, 0, 1, 0, 1, 0, 0, 1, 1, 1},
    endData()
};

void testCmpXchg0x3b1() {
    cpu->big=true;
    EdGdEax(0x3b1, cmpxchgd);
#if defined (BOXEDWINE_MSVC) && !defined (BOXEDWINE_64)
    {  
        struct Data* data = cmpxchgd;
        U32 flagMask = CF|OF|ZF|PF|SF|AF;

        while (data->valid) {
            U32 result;
            U32 result2 = data->constant;
            U32 flags = data->flags;
            __asm {
                mov ebx, data;
                mov ecx, [ebx].var1;
                mov edx, [ebx].var2;
                mov eax, result2;
                mov ebx, flags;

                push ebx
                popf

                cmpxchg ecx, edx
                mov result, ecx
                mov result2, eax

                pushf
                pop ebx
                mov flags, ebx
            }
            assertTrue(result2 == data->resultvar2);
            if (!data->dontUseResultAndCheckSFZF)
                assertTrue(result == data->result);
            if (data->dontUseResultAndCheckSFZF || data->hasSF)
                assertTrue((flags & SF)!=0 == data->fSF!=0);
            if (data->dontUseResultAndCheckSFZF || data->hasZF)
                assertTrue((flags & ZF)!=0 == data->fZF!=0);
            if (data->hasCF)
                assertTrue((flags & CF)!=0 == data->fCF!=0);
            if (data->hasOF)
                assertTrue((flags & OF)!=0 == data->fOF!=0);
            if (data->hasAF)
                assertTrue((flags & AF)!=0 == data->fAF!=0);
            data++;
        }
    } 
#endif
}

void testXadd0x3c1() {
    cpu->big=true;
    EdGd(0x3c1, xadd);
    X86_TEST(xadd, xadd, eax, ecx)
}

void testPushSeg16(int inst, U8 seg) {
    U16 prevStack = cpu->reg[4].u16;
    writew(cpu->seg[SS].address+cpu->reg[4].u16-2, 0x2222);
    cpu->seg[seg].value = 0x107;
    cpu->big = 0;
    newInstruction(inst, 0);
    runTestCPU();
    if (cpu->reg[4].u16!=prevStack-2) {
        failed("stack wasn't decremented by 2");
    }
    if (readw(cpu->seg[SS].address+cpu->reg[4].u16)!=0x107) {
        failed("seg value was not found");
    }
}

void testPopSeg16(int inst, U8 seg) {
    struct user_desc* ldt = cpu->thread->process->getLDT(0x20);
    ldt->entry_number=0x20;
    ldt->base_addr = 0;
    ldt->seg_32bit = 1;
    ldt->seg_not_present = 0;

    newInstruction(inst, 0);
    cpu->reg[4].u32-=2;
    U32 prevStack = cpu->reg[4].u32;
    writed(cpu->seg[SS].address+cpu->reg[4].u16, 0x107);
    cpu->seg[seg].value = 0;
    cpu->big = 0;    
    runTestCPU();
    if (cpu->reg[4].u16!=prevStack+2) {
        failed("stack wasn't incremented by 2");
    }
    if (cpu->seg[seg].value!=0x107) {
        failed("seg value was not set");
    }
}

void testPushSeg32(int inst, U8 seg) {
    U32 prevStack = cpu->reg[4].u32;
    writed(cpu->seg[SS].address+cpu->reg[4].u32-4, 0x22222222);
    cpu->seg[seg].value = 0x107;
    cpu->big = 1;
    newInstruction(inst, 0);
    runTestCPU();
    if (cpu->reg[4].u32!=prevStack-4) {
        failed("stack wasn't decremented by 4");
    }
    if (readd(cpu->seg[SS].address+cpu->reg[4].u32)!=0x107) {
        failed("seg value was not found");
    }
}

void testPopSeg32(int inst, U8 seg) {
    struct user_desc* ldt = cpu->thread->process->getLDT(0x20);
    ldt->entry_number=0x20;
    ldt->base_addr = 0;
    ldt->seg_32bit = 1;
    ldt->seg_not_present = 0;

    newInstruction(inst, 0);
    cpu->reg[4].u32-=4;
    U32 prevStack = cpu->reg[4].u32;
    writed(cpu->seg[SS].address+cpu->reg[4].u32, 0x107);
    cpu->seg[seg].value = 0;
    cpu->big = 1;    
    runTestCPU();
    if (cpu->reg[4].u32!=prevStack+4) {
        failed("stack wasn't incremented by 4");
    }
    if (cpu->seg[seg].value!=0x107) {
        failed("seg value was not set");
    }
}

void testPushEs0x006() {
    testPushSeg16(0x06, ES);
}

void testPushEs0x206() {
    testPushSeg32(0x06, ES);
}

void testPopEs0x007() {
    testPopSeg16(0x07, ES);
}

void testPopEs0x207() {
    testPopSeg32(0x07, ES);
}

void testPushCs0x00e() {
    testPushSeg16(0x0e, CS);
}

void testPushCs0x20e() {
    testPushSeg32(0x0e, CS);
}

void testPushSs0x016() {
    testPushSeg16(0x16, SS);
}

void testPushSs0x216() {
    testPushSeg32(0x16, SS);
}

void testPopSs0x017() {
    testPopSeg16(0x17, SS);
}

void testPopSs0x217() {
    testPopSeg32(0x17, SS);
}

void testPushDs0x01e() {
    testPushSeg16(0x1e, DS);
}

void testPushDs0x21e() {
    testPushSeg32(0x1e, DS);
}

void testPopDs0x01f() {
    testPopSeg16(0x1f, DS);
}

void testPopDs0x21f() {
    testPopSeg32(0x1f, DS);
}

void testSeg(int inst, U8 seg) {
    U32 address = HEAP_ADDRESS;
    U32 offset = 3*1024;
    if (seg==CS) {
        address = CODE_ADDRESS;
    } else {
        cpu->seg[seg].address = HEAP_ADDRESS+512;
    }
    EAX = 0;
    writeb(cpu->seg[seg].address+offset, 0xbf);
    newInstruction(inst, 0);
    pushCode8(0xa1);
    if (cpu->big) {
        pushCode32(offset);
    } else {
        pushCode16(offset);
    }
    runTestCPU();
    if (EAX!=0xbf) {
        failed("seg prefix failed");
    }
}

void testSegEs0x026() {
    cpu->big = 0;
    testSeg(0x26, ES);
}

void testSegEs0x226() {
    cpu->big = 1;
    testSeg(0x26, ES);
}

void testSegCs0x02e() {
    cpu->big = 0;
    testSeg(0x2e, CS);
}

void testSegCs0x22e() {
    cpu->big = 1;
    testSeg(0x2e, CS);
}

void testSegSs0x036() {
    cpu->big = 0;
    testSeg(0x36, SS);
}

void testSegSs0x236() {
    cpu->big = 1;
    testSeg(0x36, SS);
}

void testSegDs0x03e() {
    cpu->big = 0;
    testSeg(0x3e, DS);
}

void testSegDs0x23e() {
    cpu->big = 1;
    testSeg(0x3e, DS);
}

void run(void (*functionPtr)(), const char* name) {
    didFail = 0;
    setup();
    functionPtr();
    printf("%s", name);
    printf(" ... ");
    if (didFail) {
        printf("FAILED\n");
    } else {
        printf("OK\n");
    }
}

// disp8 is signed, it doesn't matter if disp32 is signed or not because of 32-bit rollover
// Mod
// 00 	[DS:EAX]           [DS:ECX]           [SS:EDX]           [DS:EBX]           SIB0   [DS:disp32]         [DS:ESI]            [DS:EDI]
// 01 	[DS:EAX + disp8]   [DS:ECX + disp8]   [SS:EDX + disp8]   [DS:EBX + disp8]   SIB1   [SS:EBP + disp8]    [DS:ESI + disp8]    [DS:EDI + disp8]
// 10 	[DS:EAS + disp32]  [DS:ECX + disp32]  [SS:EDX + disp32]  [DS:EBX + disp32]  SIB1   [SS:EBP + disp32]   [DS:ESI + disp32]   [DS:EDI + disp32]

void runLeaGd(int rm, int hasSib, U32 sib, int hasDisp8, int hasDisp32, S32 disp, U32 result, U32 seg) {
    Reg* reg = &cpu->reg[G(rm)];    
    U32 originalValue = reg->u32;

    // lea Gw
    cseip=CODE_ADDRESS;
    cpu->eip.u32=0;   
    pushCode8(0x8d);

    pushCode8(rm);
    if (hasSib)
        pushCode8(sib);
    if (hasDisp8)
        pushCode8(disp);
    if (hasDisp32)
        pushCode32(disp);
    runTestCPU();
    assertTrue(reg->u32==result);

    reg->u32 = originalValue;
    // MOV Gd,Ed
    writed(cpu->seg[seg].address+result, 0x35792468);

    cseip=CODE_ADDRESS;
    cpu->eip.u32=0;   
    pushCode8(0x8b);

    pushCode8(rm);
    if (hasSib)
        pushCode8(sib);
    if (hasDisp8)
        pushCode8(disp);
    if (hasDisp32)
        pushCode32(disp);
    runTestCPU();
    assertTrue(reg->u32 == 0x35792468);
}

void initMem32() {
    zeroMemory(CODE_ADDRESS, K_PAGE_SIZE*17);
    zeroMemory(STACK_ADDRESS-K_PAGE_SIZE*17, K_PAGE_SIZE*17);
    zeroMemory(HEAP_ADDRESS, K_PAGE_SIZE*17);

    cpu->big = true;
    EAX = 0x12345678;
    EBX = 0x12345678;
    ECX = 0x12345678;
    EDX = 0x12345678;
    ESP = 0x12345678;
    EBP = 0x12345678;
    ESI = 0x12345678;
    EDI = 0x12345678;
}

void test32BitMemoryAccess() {
    U32 reg1, reg2;

    // Mod 00
        // [DS:EAX]
        initMem32(); EAX = 0;       runLeaGd(0, 0, 0, 0, 0, 0, 0, DS);
        initMem32(); EAX = 0x10000; runLeaGd(1<<3, 0, 0, 0, 0, 0, 0x10000, DS);

        // [DS:ECX]
        initMem32(); ECX = 0;       runLeaGd(0|1, 0, 0, 0, 0, 0, 0, DS);
        initMem32(); ECX = 0x10000; runLeaGd(1<<3|1, 0, 0, 0, 0, 0, 0x10000, DS);

        // [DS:EDX]
        initMem32(); EDX = 0;       runLeaGd(0|2, 0, 0, 0, 0, 0, 0, DS);
        initMem32(); EDX = 0x10000; runLeaGd(1<<3|2, 0, 0, 0, 0, 0, 0x10000, DS);

        // [DS:EBX]
        initMem32(); EBX = 0;       runLeaGd(0|3, 0, 0, 0, 0, 0, 0, DS);
        initMem32(); EBX = 0x10000; runLeaGd(1<<3|3, 0, 0, 0, 0, 0, 0x10000, DS);

        // SIB0
        // [DS:reg1+reg2<<0]
        for (reg1=0;reg1<8;reg1++) {
            for (reg2=0;reg2<8;reg2++) {
                U32 seg = DS;
                if (reg1 == 4) {
                    seg = SS;
                }
                if (reg1==5) { // reg1==5 means reg1 is replaced with disp32
                    if (reg2==4) { // reg2==4 means reg2 is not used
                        initMem32(); runLeaGd(0|4, 1, (reg2<<3)|reg1, 0, 1, 0, 0, seg);
                        initMem32(); runLeaGd(1<<3|4, 1, (reg2<<3)|reg1, 0, 1, 0x10000, 0x10000, seg);

                        // shift should have no effect
                        initMem32(); runLeaGd(1<<3|4, 1, (reg2<<3)|reg1|1<<6, 0, 1, 0x10000, 0x10000, seg);
                        initMem32(); runLeaGd(1<<3|4, 1, (reg2<<3)|reg1|2<<6, 0, 1, 0x10000, 0x10000, seg);
                        initMem32(); runLeaGd(1<<3|4, 1, (reg2<<3)|reg1|3<<6, 0, 1, 0x10000, 0x10000, seg);
                    } else {
                        initMem32(); cpu->reg[reg2].u32 = 0;          runLeaGd(0|4, 1, (reg2<<3)|reg1, 0, 1, 0, 0, seg);
                        initMem32(); cpu->reg[reg2].u32 = 0x10000;    runLeaGd(1<<3|4, 1, (reg2<<3)|reg1, 0, 1, 0, 0x10000, seg);
                        initMem32(); cpu->reg[reg2].u32 = 0;          runLeaGd(2<<3|4, 1, (reg2<<3)|reg1, 0, 1, 0x10000, 0x10000, seg);
                        initMem32(); cpu->reg[reg2].u32 = 0x1000;     runLeaGd(3<<3|4, 1, (reg2<<3)|reg1, 0, 1, 0x0F000, 0x10000, seg);
                        initMem32(); cpu->reg[reg2].u32 = -(0x2000);  runLeaGd(4<<3|4, 1, (reg2<<3)|reg1, 0, 1, 0x11000, 0x0F000, seg);
                        initMem32(); cpu->reg[reg2].u32 = 0x11000;  runLeaGd(4<<3|4, 1, (reg2<<3)|reg1, 0, 1, -(0x2000), 0x0F000, seg);

                        initMem32(); cpu->reg[reg2].u32 = 0;          runLeaGd(0|4, 1, (reg2<<3)|reg1|1<<6, 0, 1, 0, 0, seg);
                        initMem32(); cpu->reg[reg2].u32 = 0x08000;    runLeaGd(1<<3|4, 1, (reg2<<3)|reg1|1<<6, 0, 1, 0, 0x10000, seg);

                        initMem32(); cpu->reg[reg2].u32 = 0;          runLeaGd(0|4, 1, (reg2<<3)|reg1|2<<6, 0, 1, 0, 0, seg);
                        initMem32(); cpu->reg[reg2].u32 = 0x04000;    runLeaGd(1<<3|4, 1, (reg2<<3)|reg1|2<<6, 0, 1, 0, 0x10000, seg);

                        initMem32(); cpu->reg[reg2].u32 = 0;          runLeaGd(0|4, 1, (reg2<<3)|reg1|3<<6, 0, 1, 0, 0, seg);
                        initMem32(); cpu->reg[reg2].u32 = 0x02000;    runLeaGd(1<<3|4, 1, (reg2<<3)|reg1|3<<6, 0, 1, 0, 0x10000, seg);
                    }
                } else {
                    if (reg2==4) { // reg2==4 means reg2 is not used
                        initMem32(); cpu->reg[reg1].u32 = 0;       runLeaGd(0|4, 1, (reg2<<3)|reg1, 0, 0, 0, 0, seg);
                        initMem32(); cpu->reg[reg1].u32 = 0x10000; runLeaGd(1<<3|4, 1, (reg2<<3)|reg1, 0, 0, 0, 0x10000, seg);

                        // shifts should have no affect
                        initMem32(); cpu->reg[reg1].u32 = 0;       runLeaGd(0|4, 1, (reg2<<3)|reg1|1<<6, 0, 0, 0, 0, seg);
                        initMem32(); cpu->reg[reg1].u32 = 0x10000; runLeaGd(1<<3|4, 1, (reg2<<3)|reg1|1<<6, 0, 0, 0, 0x10000, seg);

                        initMem32(); cpu->reg[reg1].u32 = 0;       runLeaGd(0|4, 1, (reg2<<3)|reg1|2<<6, 0, 0, 0, 0, seg);
                        initMem32(); cpu->reg[reg1].u32 = 0x10000; runLeaGd(1<<3|4, 1, (reg2<<3)|reg1|2<<6, 0, 0, 0, 0x10000, seg);

                        initMem32(); cpu->reg[reg1].u32 = 0;       runLeaGd(0|4, 1, (reg2<<3)|reg1|3<<6, 0, 0, 0, 0, seg);
                        initMem32(); cpu->reg[reg1].u32 = 0x10000; runLeaGd(1<<3|4, 1, (reg2<<3)|reg1|33<<6, 0, 0, 0, 0x10000, seg);
                    } else {
                        if (reg1==reg2) {
                            initMem32(); cpu->reg[reg1].u32 = 0;      runLeaGd(0|4, 1, (reg2<<3)|reg1, 0, 0, 0, 0, seg);
                            initMem32(); cpu->reg[reg1].u32 = 0x8000; runLeaGd(0|4, 1, (reg2<<3)|reg1, 0, 0, 0, 0x10000, seg);
                            initMem32(); cpu->reg[reg1].u32 = 0x80008000; runLeaGd(0|4, 1, (reg2<<3)|reg1, 0, 0, 0, 0x10000, seg);

                            initMem32(); cpu->reg[reg1].u32 = 0;      runLeaGd(0|4, 1, (reg2<<3)|reg1|1<<6, 0, 0, 0, 0, seg);
                            initMem32(); cpu->reg[reg1].u32 = 0x4000; runLeaGd(0|4, 1, (reg2<<3)|reg1|1<<6, 0, 0, 0, 0x0C000, seg);

                            initMem32(); cpu->reg[reg1].u32 = 0;      runLeaGd(0|4, 1, (reg2<<3)|reg1|2<<6, 0, 0, 0, 0, seg);
                            initMem32(); cpu->reg[reg1].u32 = 0x2000; runLeaGd(0|4, 1, (reg2<<3)|reg1|2<<6, 0, 0, 0, 0x0A000, seg);

                            initMem32(); cpu->reg[reg1].u32 = 0;      runLeaGd(0|4, 1, (reg2<<3)|reg1|3<<6, 0, 0, 0, 0, seg);
                            initMem32(); cpu->reg[reg1].u32 = 0x1000; runLeaGd(0|4, 1, (reg2<<3)|reg1|3<<6, 0, 0, 0, 0x09000, seg);
                        } else {
                            initMem32(); cpu->reg[reg1].u32 = 0;         cpu->reg[reg2].u32 = 0;          runLeaGd(0|4, 1, (reg2<<3)|reg1, 0, 0, 0, 0, seg);
                            initMem32(); cpu->reg[reg1].u32 = 0;         cpu->reg[reg2].u32 = 0x10000;    runLeaGd(1<<3|4, 1, (reg2<<3)|reg1, 0, 0, 0, 0x10000, seg);
                            initMem32(); cpu->reg[reg1].u32 = 0x10000;   cpu->reg[reg2].u32 = 0;          runLeaGd(2<<3|4, 1, (reg2<<3)|reg1, 0, 0, 0, 0x10000, seg);
                            initMem32(); cpu->reg[reg1].u32 = 0x0F000;   cpu->reg[reg2].u32 = 0x1000;     runLeaGd(3<<3|4, 1, (reg2<<3)|reg1, 0, 0, 0, 0x10000, seg);
                            initMem32(); cpu->reg[reg1].u32 = 0x11000;   cpu->reg[reg2].u32 = -(0x2000);  runLeaGd(4<<3|4, 1, (reg2<<3)|reg1, 0, 0, 0, 0x0F000, seg);
                            initMem32(); cpu->reg[reg1].u32 = -(0x2000); cpu->reg[reg2].u32 = 0x11000;  runLeaGd(4<<3|4, 1, (reg2<<3)|reg1, 0, 0, 0, 0x0F000, seg);

                            initMem32(); cpu->reg[reg1].u32 = 0;         cpu->reg[reg2].u32 = 0;          runLeaGd(0|4, 1, (reg2<<3)|reg1|1<<6, 0, 0, 0, 0, seg);
                            initMem32(); cpu->reg[reg1].u32 = 0x10000;   cpu->reg[reg2].u32 = 0;          runLeaGd(2<<3|4, 1, (reg2<<3)|reg1|1<<6, 0, 0, 0, 0x10000, seg);
                            initMem32(); cpu->reg[reg1].u32 = 0;         cpu->reg[reg2].u32 = 0x08000;    runLeaGd(1<<3|4, 1, (reg2<<3)|reg1|1<<6, 0, 0, 0, 0x10000, seg);
                            initMem32(); cpu->reg[reg1].u32 = 0x10000;   cpu->reg[reg2].u32 = 0x80000000; runLeaGd(2<<3|4, 1, (reg2<<3)|reg1|1<<6, 0, 0, 0, 0x10000, seg);

                            initMem32(); cpu->reg[reg1].u32 = 0;         cpu->reg[reg2].u32 = 0;          runLeaGd(0|4, 1, (reg2<<3)|reg1|2<<6, 0, 0, 0, 0, seg);
                            initMem32(); cpu->reg[reg1].u32 = 0x10000;   cpu->reg[reg2].u32 = 0;          runLeaGd(2<<3|4, 1, (reg2<<3)|reg1|2<<6, 0, 0, 0, 0x10000, seg);
                            initMem32(); cpu->reg[reg1].u32 = 0;         cpu->reg[reg2].u32 = 0x04000;    runLeaGd(1<<3|4, 1, (reg2<<3)|reg1|2<<6, 0, 0, 0, 0x10000, seg);
                            initMem32(); cpu->reg[reg1].u32 = 0x10000;   cpu->reg[reg2].u32 = 0x40000000; runLeaGd(2<<3|4, 1, (reg2<<3)|reg1|2<<6, 0, 0, 0, 0x10000, seg);

                            initMem32(); cpu->reg[reg1].u32 = 0;         cpu->reg[reg2].u32 = 0;          runLeaGd(0|4, 1, (reg2<<3)|reg1|3<<6, 0, 0, 0, 0, seg);
                            initMem32(); cpu->reg[reg1].u32 = 0x10000;   cpu->reg[reg2].u32 = 0;          runLeaGd(2<<3|4, 1, (reg2<<3)|reg1|3<<6, 0, 0, 0, 0x10000, seg);
                            initMem32(); cpu->reg[reg1].u32 = 0;         cpu->reg[reg2].u32 = 0x02000;    runLeaGd(1<<3|4, 1, (reg2<<3)|reg1|3<<6, 0, 0, 0, 0x10000, seg);
                            initMem32(); cpu->reg[reg1].u32 = 0x10000;   cpu->reg[reg2].u32 = 0x20000000; runLeaGd(2<<3|4, 1, (reg2<<3)|reg1|3<<6, 0, 0, 0, 0x10000, seg);
                        }
                    }
                }
            }
        }

        // [DS:disp32]
        initMem32(); runLeaGd(0|5, 0, 0, 0, true, 0, 0, DS);
        initMem32(); runLeaGd(1<<3|5, 0, 0, 0, true, 0x10000, 0x10000, DS);

        // [DS:ESI]
        initMem32(); ESI = 0;       runLeaGd(0|6, 0, 0, 0, 0, 0, 0, DS);
        initMem32(); ESI = 0x10000; runLeaGd(1<<3|6, 0, 0, 0, 0, 0, 0x10000, DS);

        // [DS:EDI]
        initMem32(); EDI = 0;       runLeaGd(0|7, 0, 0, 0, 0, 0, 0, DS);
        initMem32(); EDI = 0x10000; runLeaGd(1<<3|7, 0, 0, 0, 0, 0, 0x10000, DS);

    // Mod 01
        // [DS:EAX+disp8]
        initMem32(); EAX = 0;       runLeaGd(0x40|0, 0, 0, 1, 0, 0, 0, DS);
        initMem32(); EAX = 0x10000; runLeaGd(0x40|1<<3, 0, 0, 1, 0, 0, 0x10000, DS);
        initMem32(); EAX = 0x0FFF0; runLeaGd(0x40|1<<3, 0, 0, 1, 0, 0x10, 0x10000, DS);
        initMem32(); EAX = 0x10010; runLeaGd(0x40|1<<3, 0, 0, 1, 0, -0x10, 0x10000, DS);

        // [DS:ECX+disp8]
        initMem32(); ECX = 0;       runLeaGd(0x40|0|1, 0, 0, 1, 0, 0, 0, DS);
        initMem32(); ECX = 0x10000; runLeaGd(0x40|1<<3|1, 0, 0, 1, 0, 0, 0x10000, DS);
        initMem32(); ECX = 0x0FFF0; runLeaGd(0x40|1<<3|1, 0, 0, 1, 0, 0x10, 0x10000, DS);
        initMem32(); ECX = 0x10010; runLeaGd(0x40|1<<3|1, 0, 0, 1, 0, -0x10, 0x10000, DS);

        // [DS:EDX+disp8]
        initMem32(); EDX = 0;       runLeaGd(0x40|0|2, 0, 0, 1, 0, 0, 0, DS);
        initMem32(); EDX = 0x10000; runLeaGd(0x40|1<<3|2, 0, 0, 1, 0, 0, 0x10000, DS);
        initMem32(); EDX = 0x0FFF0; runLeaGd(0x40|1<<3|2, 0, 0, 1, 0, 0x10, 0x10000, DS);
        initMem32(); EDX = 0x10010; runLeaGd(0x40|1<<3|2, 0, 0, 1, 0, -0x10, 0x10000, DS);

        // [DS:EBX+disp8]
        initMem32(); EBX = 0;       runLeaGd(0x40|0|3, 0, 0, 1, 0, 0, 0, DS);
        initMem32(); EBX = 0x10000; runLeaGd(0x40|1<<3|3, 0, 0, 1, 0, 0, 0x10000, DS);
        initMem32(); EBX = 0x0FFF0; runLeaGd(0x40|1<<3|3, 0, 0, 1, 0, 0x10, 0x10000, DS);
        initMem32(); EBX = 0x10010; runLeaGd(0x40|1<<3|3, 0, 0, 1, 0, -0x10, 0x10000, DS);

        // SIB1
        // [DS:reg1+reg2<<0+disp8]
        for (reg1=0;reg1<8;reg1++) {
            for (reg2=0;reg2<8;reg2++) {
                U32 seg = DS;
                if (reg1 == 4 || reg1==5) {
                    seg = SS;
                }
                if (reg2==4) { // reg2==4 means reg2 is not used
                    initMem32(); cpu->reg[reg1].u32 = 0;       runLeaGd(0x40|0|4, 1, (reg2<<3)|reg1, 1, 0, 0, 0, seg);
                    initMem32(); cpu->reg[reg1].u32 = 0x10000; runLeaGd(0x40|1<<3|4, 1, (reg2<<3)|reg1, 1, 0, 0, 0x10000, seg);
                    initMem32(); cpu->reg[reg1].u32 = 0x0FFF0; runLeaGd(0x40|2<<3|4, 1, (reg2<<3)|reg1, 1, 0, 0x10, 0x10000, seg);
                    initMem32(); cpu->reg[reg1].u32 = 0x10010; runLeaGd(0x40|3<<3|4, 1, (reg2<<3)|reg1, 1, 0, -0x10, 0x10000, seg);

                    // shift should have no effect
                    initMem32(); cpu->reg[reg1].u32 = 0x10010; runLeaGd(0x40|3<<3|4, 1, (reg2<<3)|reg1|1<<6, 1, 0, -0x10, 0x10000, seg);
                    initMem32(); cpu->reg[reg1].u32 = 0x10010; runLeaGd(0x40|3<<3|4, 1, (reg2<<3)|reg1|2<<6, 1, 0, -0x10, 0x10000, seg);
                    initMem32(); cpu->reg[reg1].u32 = 0x10010; runLeaGd(0x40|3<<3|4, 1, (reg2<<3)|reg1|3<<6, 1, 0, -0x10, 0x10000, seg);
                } else {
                    if (reg1==reg2) {
                        initMem32(); cpu->reg[reg1].u32 = 0;      runLeaGd(0x40|0|4, 1, (reg2<<3)|reg1, 1, 0, 0, 0, seg);
                        initMem32(); cpu->reg[reg1].u32 = 0x8000; runLeaGd(0x40|1<<3|4, 1, (reg2<<3)|reg1, 1, 0, 0, 0x10000, seg);
                        initMem32(); cpu->reg[reg1].u32 = 0x8000; runLeaGd(0x40|2<<3|4, 1, (reg2<<3)|reg1, 1, 0, -0x10, 0xFFF0, seg);
                        initMem32(); cpu->reg[reg1].u32 = 0x8000; runLeaGd(0x40|3<<3|4, 1, (reg2<<3)|reg1, 1, 0, 0x10, 0x10010, seg);
                        initMem32(); cpu->reg[reg1].u32 = 0x80008000; runLeaGd(0x40|4<<3|4, 1, (reg2<<3)|reg1, 1, 0, 0, 0x10000, seg);

                        initMem32(); cpu->reg[reg1].u32 = 0;      runLeaGd(0x40|0|4, 1, (reg2<<3)|reg1|1<<6, 1, 0, 0, 0, seg);
                        initMem32(); cpu->reg[reg1].u32 = 0x4000; runLeaGd(0x40|3<<3|4, 1, (reg2<<3)|reg1|1<<6, 1, 0, 0x10, 0x0C010, seg);

                        initMem32(); cpu->reg[reg1].u32 = 0;      runLeaGd(0x40|0|4, 1, (reg2<<3)|reg1|2<<6, 1, 0, 0, 0, seg);
                        initMem32(); cpu->reg[reg1].u32 = 0x2000; runLeaGd(0x40|3<<3|4, 1, (reg2<<3)|reg1|2<<6, 1, 0, 0x10, 0x0A010, seg);

                        initMem32(); cpu->reg[reg1].u32 = 0;      runLeaGd(0x40|0|4, 1, (reg2<<3)|reg1|3<<6, 1, 0, 0, 0, seg);
                        initMem32(); cpu->reg[reg1].u32 = 0x1000; runLeaGd(0x40|3<<3|4, 1, (reg2<<3)|reg1|3<<6, 1, 0, 0x10, 0x09010, seg);
                    } else {
                        initMem32(); cpu->reg[reg1].u32 = 0;         cpu->reg[reg2].u32 = 0;          runLeaGd(0x40|0|4, 1, (reg2<<3)|reg1, 1, 0, 0, 0, seg);
                        initMem32(); cpu->reg[reg1].u32 = 0;         cpu->reg[reg2].u32 = 0x10000;    runLeaGd(0x40|1<<3|4, 1, (reg2<<3)|reg1, 1, 0, 0, 0x10000, seg);
                        initMem32(); cpu->reg[reg1].u32 = 0;         cpu->reg[reg2].u32 = 0x10000;    runLeaGd(0x40|1<<3|4, 1, (reg2<<3)|reg1, 1, 0, -0x10, 0xFFF0, seg);
                        initMem32(); cpu->reg[reg1].u32 = 0;         cpu->reg[reg2].u32 = 0x10000;    runLeaGd(0x40|1<<3|4, 1, (reg2<<3)|reg1, 1, 0, 0x40, 0x10040, seg);
                        initMem32(); cpu->reg[reg1].u32 = 0x10000;   cpu->reg[reg2].u32 = 0;          runLeaGd(0x40|2<<3|4, 1, (reg2<<3)|reg1, 1, 0, 0, 0x10000, seg);
                        initMem32(); cpu->reg[reg1].u32 = 0x10000;   cpu->reg[reg2].u32 = 0;          runLeaGd(0x40|2<<3|4, 1, (reg2<<3)|reg1, 1, 0, -0x40, 0xFFC0, seg);
                        initMem32(); cpu->reg[reg1].u32 = 0x10000;   cpu->reg[reg2].u32 = 0;          runLeaGd(0x40|2<<3|4, 1, (reg2<<3)|reg1, 1, 0, 0x20, 0x10020, seg);
                        initMem32(); cpu->reg[reg1].u32 = 0x0F000;   cpu->reg[reg2].u32 = 0x1000;     runLeaGd(0x40|3<<3|4, 1, (reg2<<3)|reg1, 1, 0, 0, 0x10000, seg);
                        initMem32(); cpu->reg[reg1].u32 = 0x11000;   cpu->reg[reg2].u32 = -(0x2000);  runLeaGd(0x40|4<<3|4, 1, (reg2<<3)|reg1, 1, 0, 0, 0x0F000, seg);
                        initMem32(); cpu->reg[reg1].u32 = 0x11000;   cpu->reg[reg2].u32 = -(0x2000);  runLeaGd(0x40|4<<3|4, 1, (reg2<<3)|reg1, 1, 0, -0x30, 0x0EFD0, seg);
                        initMem32(); cpu->reg[reg1].u32 = 0x11000;   cpu->reg[reg2].u32 = -(0x2000);  runLeaGd(0x40|4<<3|4, 1, (reg2<<3)|reg1, 1, 0, 0x70, 0x0F070, seg);
                        initMem32(); cpu->reg[reg1].u32 = -(0x2000); cpu->reg[reg2].u32 = 0x11000;  runLeaGd(0x40|5<<3|4, 1, (reg2<<3)|reg1, 1, 0, 0, 0x0F000, seg);
                        initMem32(); cpu->reg[reg1].u32 = -(0x2000); cpu->reg[reg2].u32 = 0x11000;  runLeaGd(0x40|5<<3|4, 1, (reg2<<3)|reg1, 1, 0, -0x10, 0x0EFF0, seg);
                        initMem32(); cpu->reg[reg1].u32 = -(0x2000); cpu->reg[reg2].u32 = 0x11000;  runLeaGd(0x40|5<<3|4, 1, (reg2<<3)|reg1, 1, 0, 0x50, 0x0F050, seg);

                        initMem32(); cpu->reg[reg1].u32 = 0;         cpu->reg[reg2].u32 = 0;          runLeaGd(0x40|0|4, 1, (reg2<<3)|reg1|1<<6, 1, 0, 0, 0, seg);
                        initMem32(); cpu->reg[reg1].u32 = 0;         cpu->reg[reg2].u32 = 0x08000;    runLeaGd(0x40|1<<3|4, 1, (reg2<<3)|reg1|1<<6, 1, 0, 0, 0x10000, seg);
                        initMem32(); cpu->reg[reg1].u32 = 0x10000;   cpu->reg[reg2].u32 = 0;          runLeaGd(0x40|2<<3|4, 1, (reg2<<3)|reg1|1<<6, 1, 0, 0, 0x10000, seg);
                        initMem32(); cpu->reg[reg1].u32 = 0x10000;   cpu->reg[reg2].u32 = 0x80000000; runLeaGd(0x40|2<<3|4, 1, (reg2<<3)|reg1|1<<6, 1, 0, 0, 0x10000, seg);


                        initMem32(); cpu->reg[reg1].u32 = 0;         cpu->reg[reg2].u32 = 0;          runLeaGd(0x40|0|4, 1, (reg2<<3)|reg1|2<<6, 1, 0, 0, 0, seg);
                        initMem32(); cpu->reg[reg1].u32 = 0;         cpu->reg[reg2].u32 = 0x04000;    runLeaGd(0x40|1<<3|4, 1, (reg2<<3)|reg1|2<<6, 1, 0, 0, 0x10000, seg);
                        initMem32(); cpu->reg[reg1].u32 = 0x10000;   cpu->reg[reg2].u32 = 0;          runLeaGd(0x40|2<<3|4, 1, (reg2<<3)|reg1|2<<6, 1, 0, 0, 0x10000, seg);
                        initMem32(); cpu->reg[reg1].u32 = 0x10000;   cpu->reg[reg2].u32 = 0x40000000; runLeaGd(0x40|2<<3|4, 1, (reg2<<3)|reg1|2<<6, 1, 0, 0, 0x10000, seg);

                        initMem32(); cpu->reg[reg1].u32 = 0;         cpu->reg[reg2].u32 = 0;          runLeaGd(0x40|0|4, 1, (reg2<<3)|reg1|3<<6, 1, 0, 0, 0, seg);
                        initMem32(); cpu->reg[reg1].u32 = 0;         cpu->reg[reg2].u32 = 0x02000;    runLeaGd(0x40|1<<3|4, 1, (reg2<<3)|reg1|3<<6, 1, 0, 0, 0x10000, seg);
                        initMem32(); cpu->reg[reg1].u32 = 0x10000;   cpu->reg[reg2].u32 = 0;          runLeaGd(0x40|2<<3|4, 1, (reg2<<3)|reg1|3<<6, 1, 0, 0, 0x10000, seg);
                        initMem32(); cpu->reg[reg1].u32 = 0x10000;   cpu->reg[reg2].u32 = 0x20000000; runLeaGd(0x40|2<<3|4, 1, (reg2<<3)|reg1|3<<6, 1, 0, 0, 0x10000, seg);
                    }
                }
            }
        }

        // [SS:EBP+disp8]
        initMem32(); EBP = 0;       runLeaGd(0x40|0|5, 0, 0, 1, 0, 0, 0, SS);
        initMem32(); EBP = 0x10000; runLeaGd(0x40|1<<3|5, 0, 0, 1, 0, 0, 0x10000, SS);
        initMem32(); EBP = 0x0FFF0; runLeaGd(0x40|1<<3|5, 0, 0, 1, 0, 0x10, 0x10000, SS);
        initMem32(); EBP = 0x10010; runLeaGd(0x40|1<<3|5, 0, 0, 1, 0, -0x10, 0x10000, SS);

        // [DS:ESI+disp8]
        initMem32(); ESI = 0;       runLeaGd(0x40|0|6, 0, 0, 1, 0, 0, 0, DS);
        initMem32(); ESI = 0x10000; runLeaGd(0x40|1<<3|6, 0, 0, 1, 0, 0, 0x10000, DS);
        initMem32(); ESI = 0x0FFF0; runLeaGd(0x40|1<<3|6, 0, 0, 1, 0, 0x10, 0x10000, DS);
        initMem32(); ESI = 0x10010; runLeaGd(0x40|1<<3|6, 0, 0, 1, 0, -0x10, 0x10000, DS);

        // [DS:EDI+disp8]
        initMem32(); EDI = 0;       runLeaGd(0x40|0|7, 0, 0, 1, 0, 0, 0, DS);
        initMem32(); EDI = 0x10000; runLeaGd(0x40|1<<3|7, 0, 0, 1, 0, 0, 0x10000, DS);
        initMem32(); EDI = 0x0FFF0; runLeaGd(0x40|1<<3|7, 0, 0, 1, 0, 0x10, 0x10000, DS);
        initMem32(); EDI = 0x10010; runLeaGd(0x40|1<<3|7, 0, 0, 1, 0, -0x10, 0x10000, DS);

    // Mod 02
        // [DS:EAX+disp32]
        initMem32(); EAX = 0;       runLeaGd(0x80|0, 0, 0, 0, 1, 0, 0, DS);
        initMem32(); EAX = 0x10000; runLeaGd(0x80|1<<3, 0, 0, 0, 1, 0, 0x10000, DS);
        initMem32(); EAX = 0x0FFF0; runLeaGd(0x80|1<<3, 0, 0, 0, 1, 0x10, 0x10000, DS);
        initMem32(); EAX = 0x10010; runLeaGd(0x80|1<<3, 0, 0, 0, 1, -0x10, 0x10000, DS);
        initMem32(); EAX = -0x10; runLeaGd(0x80|1<<3, 0, 0, 0, 1, 0x10010, 0x10000, DS);

        // [DS:ECX+disp32]
        initMem32(); ECX = 0;       runLeaGd(0x80|0|1, 0, 0, 0, 1, 0, 0, DS);
        initMem32(); ECX = 0x10000; runLeaGd(0x80|1<<3|1, 0, 0, 0, 1, 0, 0x10000, DS);
        initMem32(); ECX = 0x0FFF0; runLeaGd(0x80|1<<3|1, 0, 0, 0, 1, 0x10, 0x10000, DS);
        initMem32(); ECX = 0x10010; runLeaGd(0x80|1<<3|1, 0, 0, 0, 1, -0x10, 0x10000, DS);
        initMem32(); ECX = -0x10; runLeaGd(0x80|1<<3|1, 0, 0, 0, 1, 0x10010, 0x10000, DS);

        // [DS:EDX+disp32]
        initMem32(); EDX = 0;       runLeaGd(0x80|0|2, 0, 0, 0, 1, 0, 0, DS);
        initMem32(); EDX = 0x10000; runLeaGd(0x80|1<<3|2, 0, 0, 0, 1, 0, 0x10000, DS);
        initMem32(); EDX = 0x0FFF0; runLeaGd(0x80|1<<3|2, 0, 0, 0, 1, 0x10, 0x10000, DS);
        initMem32(); EDX = 0x10010; runLeaGd(0x80|1<<3|2, 0, 0, 0, 1, -0x10, 0x10000, DS);
        initMem32(); EDX = -0x10; runLeaGd(0x80|1<<3|2, 0, 0, 0, 1, 0x10010, 0x10000, DS);

        // [DS:EBX+disp32]
        initMem32(); EBX = 0;       runLeaGd(0x80|0|3, 0, 0, 0, 1, 0, 0, DS);
        initMem32(); EBX = 0x10000; runLeaGd(0x80|1<<3|3, 0, 0, 0, 1, 0, 0x10000, DS);
        initMem32(); EBX = 0x0FFF0; runLeaGd(0x80|1<<3|3, 0, 0, 0, 1, 0x10, 0x10000, DS);
        initMem32(); EBX = 0x10010; runLeaGd(0x80|1<<3|3, 0, 0, 0, 1, -0x10, 0x10000, DS);
        initMem32(); EBX = -0x10; runLeaGd(0x80|1<<3|3, 0, 0, 0, 1, 0x10010, 0x10000, DS);

        // SIB1
        // [DS:reg1+reg2<<0+disp32]
        for (reg1=0;reg1<8;reg1++) {
            for (reg2=0;reg2<8;reg2++) {
                U32 seg = DS;
                if (reg1 == 4 || reg1==5) {
                    seg = SS;
                }
                if (reg2==4) { // reg2==4 means reg2 is not used
                    initMem32(); cpu->reg[reg1].u32 = 0;       runLeaGd(0x80|0|4, 1, (reg2<<3)|reg1, 0, 1, 0, 0, seg);
                    initMem32(); cpu->reg[reg1].u32 = 0x10000; runLeaGd(0x80|1<<3|4, 1, (reg2<<3)|reg1, 0, 1, 0, 0x10000, seg);
                    initMem32(); cpu->reg[reg1].u32 = 0x0FFF0; runLeaGd(0x80|2<<3|4, 1, (reg2<<3)|reg1, 0, 1, 0x10, 0x10000, seg);
                    initMem32(); cpu->reg[reg1].u32 = 0x10010; runLeaGd(0x80|3<<3|4, 1, (reg2<<3)|reg1, 0, 1, -0x10, 0x10000, seg);

                    // shift should have no effect
                    initMem32(); cpu->reg[reg1].u32 = 0x10010; runLeaGd(0x80|3<<3|4, 1, (reg2<<3)|reg1|1<<6, 0, 1, -0x10, 0x10000, seg);
                    initMem32(); cpu->reg[reg1].u32 = 0x10010; runLeaGd(0x80|3<<3|4, 1, (reg2<<3)|reg1|2<<6, 0, 1, -0x10, 0x10000, seg);
                    initMem32(); cpu->reg[reg1].u32 = 0x10010; runLeaGd(0x80|3<<3|4, 1, (reg2<<3)|reg1|3<<6, 0, 1, -0x10, 0x10000, seg);
                } else {
                    if (reg1==reg2) {
                        initMem32(); cpu->reg[reg1].u32 = 0;      runLeaGd(0x80|0|4, 1, (reg2<<3)|reg1, 0, 1, 0, 0, seg);
                        initMem32(); cpu->reg[reg1].u32 = 0x8000; runLeaGd(0x80|1<<3|4, 1, (reg2<<3)|reg1, 0, 1, 0, 0x10000, seg);
                        initMem32(); cpu->reg[reg1].u32 = 0x8000; runLeaGd(0x80|2<<3|4, 1, (reg2<<3)|reg1, 0, 1, -0x10, 0xFFF0, seg);
                        initMem32(); cpu->reg[reg1].u32 = 0x8000; runLeaGd(0x80|3<<3|4, 1, (reg2<<3)|reg1, 0, 1, 0x10, 0x10010, seg);
                        initMem32(); cpu->reg[reg1].u32 = 0x80008000; runLeaGd(0x80|4<<3|4, 1, (reg2<<3)|reg1, 0, 1, 0, 0x10000, seg);

                        initMem32(); cpu->reg[reg1].u32 = 0;      runLeaGd(0x80|0|4, 1, (reg2<<3)|reg1|1<<6, 0, 1, 0, 0, seg);
                        initMem32(); cpu->reg[reg1].u32 = 0x4000; runLeaGd(0x80|3<<3|4, 1, (reg2<<3)|reg1|1<<6, 0, 1, 0x10, 0x0C010, seg);

                        initMem32(); cpu->reg[reg1].u32 = 0;      runLeaGd(0x80|0|4, 1, (reg2<<3)|reg1|2<<6, 0, 1, 0, 0, seg);
                        initMem32(); cpu->reg[reg1].u32 = 0x2000; runLeaGd(0x80|3<<3|4, 1, (reg2<<3)|reg1|2<<6, 0, 1, 0x10, 0x0A010, seg);

                        initMem32(); cpu->reg[reg1].u32 = 0;      runLeaGd(0x80|0|4, 1, (reg2<<3)|reg1|3<<6, 0, 1, 0, 0, seg);
                        initMem32(); cpu->reg[reg1].u32 = 0x1000; runLeaGd(0x80|3<<3|4, 1, (reg2<<3)|reg1|3<<6, 0, 1, 0x10, 0x09010, seg);
                    } else {
                        initMem32(); cpu->reg[reg1].u32 = 0;         cpu->reg[reg2].u32 = 0;          runLeaGd(0x80|0|4, 1, (reg2<<3)|reg1, 0, 1, 0, 0, seg);
                        initMem32(); cpu->reg[reg1].u32 = 0;         cpu->reg[reg2].u32 = 0x10000;    runLeaGd(0x80|1<<3|4, 1, (reg2<<3)|reg1, 0, 1, 0, 0x10000, seg);
                        initMem32(); cpu->reg[reg1].u32 = 0;         cpu->reg[reg2].u32 = 0x10000;    runLeaGd(0x80|1<<3|4, 1, (reg2<<3)|reg1, 0, 1, -0x10, 0xFFF0, seg);
                        initMem32(); cpu->reg[reg1].u32 = 0;         cpu->reg[reg2].u32 = 0x10000;    runLeaGd(0x80|1<<3|4, 1, (reg2<<3)|reg1, 0, 1, 0x40, 0x10040, seg);
                        initMem32(); cpu->reg[reg1].u32 = 0x10000;   cpu->reg[reg2].u32 = 0;          runLeaGd(0x80|2<<3|4, 1, (reg2<<3)|reg1, 0, 1, 0, 0x10000, seg);
                        initMem32(); cpu->reg[reg1].u32 = 0x10000;   cpu->reg[reg2].u32 = 0;          runLeaGd(0x80|2<<3|4, 1, (reg2<<3)|reg1, 0, 1, -0x40, 0xFFC0, seg);
                        initMem32(); cpu->reg[reg1].u32 = 0x10000;   cpu->reg[reg2].u32 = 0;          runLeaGd(0x80|2<<3|4, 1, (reg2<<3)|reg1, 0, 1, 0x20, 0x10020, seg);
                        initMem32(); cpu->reg[reg1].u32 = 0x0F000;   cpu->reg[reg2].u32 = 0x1000;     runLeaGd(0x80|3<<3|4, 1, (reg2<<3)|reg1, 0, 1, 0, 0x10000, seg);
                        initMem32(); cpu->reg[reg1].u32 = 0x11000;   cpu->reg[reg2].u32 = -(0x2000);  runLeaGd(0x80|4<<3|4, 1, (reg2<<3)|reg1, 0, 1, 0, 0x0F000, seg);
                        initMem32(); cpu->reg[reg1].u32 = 0x11000;   cpu->reg[reg2].u32 = -(0x2000);  runLeaGd(0x80|4<<3|4, 1, (reg2<<3)|reg1, 0, 1, -0x30, 0x0EFD0, seg);
                        initMem32(); cpu->reg[reg1].u32 = 0x11000;   cpu->reg[reg2].u32 = -(0x2000);  runLeaGd(0x80|4<<3|4, 1, (reg2<<3)|reg1, 0, 1, 0x70, 0x0F070, seg);
                        initMem32(); cpu->reg[reg1].u32 = -(0x2000); cpu->reg[reg2].u32 = 0x11000;  runLeaGd(0x80|5<<3|4, 1, (reg2<<3)|reg1, 0, 1, 0, 0x0F000, seg);
                        initMem32(); cpu->reg[reg1].u32 = -(0x2000); cpu->reg[reg2].u32 = 0x11000;  runLeaGd(0x80|5<<3|4, 1, (reg2<<3)|reg1, 0, 1, -0x10, 0x0EFF0, seg);
                        initMem32(); cpu->reg[reg1].u32 = -(0x2000); cpu->reg[reg2].u32 = 0x11000;  runLeaGd(0x80|5<<3|4, 1, (reg2<<3)|reg1, 0, 1, 0x50, 0x0F050, seg);

                        initMem32(); cpu->reg[reg1].u32 = 0;         cpu->reg[reg2].u32 = 0;          runLeaGd(0x80|0|4, 1, (reg2<<3)|reg1|1<<6, 0, 1, 0, 0, seg);
                        initMem32(); cpu->reg[reg1].u32 = 0;         cpu->reg[reg2].u32 = 0x08000;    runLeaGd(0x80|1<<3|4, 1, (reg2<<3)|reg1|1<<6, 0, 1, 0, 0x10000, seg);
                        initMem32(); cpu->reg[reg1].u32 = 0x10000;   cpu->reg[reg2].u32 = 0;          runLeaGd(0x80|2<<3|4, 1, (reg2<<3)|reg1|1<<6, 0, 1, 0, 0x10000, seg);
                        initMem32(); cpu->reg[reg1].u32 = 0x10000;   cpu->reg[reg2].u32 = 0x80000000; runLeaGd(0x80|2<<3|4, 1, (reg2<<3)|reg1|1<<6, 0, 1, 0, 0x10000, seg);


                        initMem32(); cpu->reg[reg1].u32 = 0;         cpu->reg[reg2].u32 = 0;          runLeaGd(0x80|0|4, 1, (reg2<<3)|reg1|2<<6, 0, 1, 0, 0, seg);
                        initMem32(); cpu->reg[reg1].u32 = 0;         cpu->reg[reg2].u32 = 0x04000;    runLeaGd(0x80|1<<3|4, 1, (reg2<<3)|reg1|2<<6, 0, 1, 0, 0x10000, seg);
                        initMem32(); cpu->reg[reg1].u32 = 0x10000;   cpu->reg[reg2].u32 = 0;          runLeaGd(0x80|2<<3|4, 1, (reg2<<3)|reg1|2<<6, 0, 1, 0, 0x10000, seg);
                        initMem32(); cpu->reg[reg1].u32 = 0x10000;   cpu->reg[reg2].u32 = 0x40000000; runLeaGd(0x80|2<<3|4, 1, (reg2<<3)|reg1|2<<6, 0, 1, 0, 0x10000, seg);

                        initMem32(); cpu->reg[reg1].u32 = 0;         cpu->reg[reg2].u32 = 0;          runLeaGd(0x80|0|4, 1, (reg2<<3)|reg1|3<<6, 0, 1, 0, 0, seg);
                        initMem32(); cpu->reg[reg1].u32 = 0;         cpu->reg[reg2].u32 = 0x02000;    runLeaGd(0x80|1<<3|4, 1, (reg2<<3)|reg1|3<<6, 0, 1, 0, 0x10000, seg);
                        initMem32(); cpu->reg[reg1].u32 = 0x10000;   cpu->reg[reg2].u32 = 0;          runLeaGd(0x80|2<<3|4, 1, (reg2<<3)|reg1|3<<6, 0, 1, 0, 0x10000, seg);
                        initMem32(); cpu->reg[reg1].u32 = 0x10000;   cpu->reg[reg2].u32 = 0x20000000; runLeaGd(0x80|2<<3|4, 1, (reg2<<3)|reg1|3<<6, 0, 1, 0, 0x10000, seg);
                    }
                }
            }
        }

        // [SS:EBP+disp32]
        initMem32(); EBP = 0;       runLeaGd(0x80|0|5, 0, 0, 0, 1, 0, 0, SS);
        initMem32(); EBP = 0x10000; runLeaGd(0x80|1<<3|5, 0, 0, 0, 1, 0, 0x10000, SS);
        initMem32(); EBP = 0x0FFF0; runLeaGd(0x80|1<<3|5, 0, 0, 0, 1, 0x10, 0x10000, SS);
        initMem32(); EBP = 0x10010; runLeaGd(0x80|1<<3|5, 0, 0, 0, 1, -0x10, 0x10000, SS);
        initMem32(); EBP = -0x10; runLeaGd(0x80|1<<3|5, 0, 0, 0, 1, 0x10010, 0x10000, SS);

        // [DS:ESI+disp32]
        initMem32(); ESI = 0;       runLeaGd(0x80|0|6, 0, 0, 0, 1, 0, 0, DS);
        initMem32(); ESI = 0x10000; runLeaGd(0x80|1<<3|6, 0, 0, 0, 1, 0, 0x10000, DS);
        initMem32(); ESI = 0x0FFF0; runLeaGd(0x80|1<<3|6, 0, 0, 0, 1, 0x10, 0x10000, DS);
        initMem32(); ESI = 0x10010; runLeaGd(0x80|1<<3|6, 0, 0, 0, 1, -0x10, 0x10000, DS);
        initMem32(); ESI = -0x10; runLeaGd(0x80|1<<3|6, 0, 0, 0, 1, 0x10010, 0x10000, DS);

        // [DS:EDI+disp32]
        initMem32(); EDI = 0;       runLeaGd(0x80|0|7, 0, 0, 0, 1, 0, 0, DS);
        initMem32(); EDI = 0x10000; runLeaGd(0x80|1<<3|7, 0, 0, 0, 1, 0, 0x10000, DS);
        initMem32(); EDI = 0x0FFF0; runLeaGd(0x80|1<<3|7, 0, 0, 0, 1, 0x10, 0x10000, DS);
        initMem32(); EDI = 0x10010; runLeaGd(0x80|1<<3|7, 0, 0, 0, 1, -0x10, 0x10000, DS);
        initMem32(); EDI = -0x10; runLeaGd(0x80|1<<3|7, 0, 0, 0, 1, 0x10010, 0x10000, DS);
}

// disp8 is signed, it doesn't matter if disp16 is signed or not because of 16-bit rollover
// Mod
// 00 	[DS:BX + SI]           [DS:BX + DI]           [SS:BP + SI]           [SS:BP + DI]           [DS:SI]            [DS:DI]            [DS:disp16]        [DS:BX]
// 01 	[DS:BX + SI + disp8]   [DS:BX + DI + disp8]   [SS:BP + SI + disp8]   [SS:BP + DI + disp8]   [DS:SI + disp8]    [DS:DI + disp8]    [SS:BP + disp8]    [DS:BX + disp8]
// 10 	[DS:BX + SI + disp16]  [DS:BX + DI + disp16]  [SS:BP + SI + disp16]  [SS:BP + DI + disp16]  [DS:SI + disp16]   [DS:DI + disp16]   [SS:BP + disp16]   [DS:BX + disp16]

void runLeaGw(int rm, int hasDisp8, int hasDisp16, U32 d, U16 result, U32 seg) {
    Reg* reg = &cpu->reg[G(rm)];    
    U16 originalValue = reg->u16;
    S16 disp = (S16)d;

    // lea Gw
    cseip=CODE_ADDRESS;
    cpu->eip.u32=0;   
    pushCode8(0x8d);

    pushCode8(rm);
    if (hasDisp8)
        pushCode8(disp);
    if (hasDisp16)
        pushCode16(disp);
    runTestCPU();
    assertTrue(reg->u16==result);

    reg->u16 = originalValue;
    // MOV Gw,Ew
    writew(cpu->seg[seg].address+result, 0x3579);

    cseip=CODE_ADDRESS;
    cpu->eip.u32=0;   
    pushCode8(0x8b);

    pushCode8(rm);
    if (hasDisp8)
        pushCode8(disp);
    if (hasDisp16)
        pushCode16(disp);
    runTestCPU();
    assertTrue(reg->u16 == 0x3579);
}

void initMem16() {
    zeroMemory(CODE_ADDRESS, K_PAGE_SIZE*17);
    zeroMemory(STACK_ADDRESS-K_PAGE_SIZE*17, K_PAGE_SIZE*17);
    zeroMemory(HEAP_ADDRESS, K_PAGE_SIZE*17);

    cpu->big = false;
    EAX = 0x12345678;
    EBX = 0x12345678;
    ECX = 0x12345678;
    EDX = 0x12345678;
    ESP = 0x12345678;
    EBP = 0x12345678;
    ESI = 0x12345678;
    EDI = 0x12345678;
}

void test16BitMemoryAccess() {
    // Mod 00
        // [DS:BX + SI]
        initMem16(); BX = 0;      SI = 0;      runLeaGw(0, 0, 0, 0, 0, DS);
        initMem16(); BX = 1;      SI = 0;      runLeaGw(1<<3, 0, 0, 0, 1, DS);
        initMem16(); BX = 0;      SI = 1;      runLeaGw(2<<3, 0, 0, 0, 1, DS);
        initMem16(); BX = 0xFFFF; SI = 0;      runLeaGw(3<<3, 0, 0, 0, 0xFFFF, DS);
        initMem16(); BX = 0;      SI = 0xFFFF; runLeaGw(4<<3, 0, 0, 0, 0xFFFF, DS);
        initMem16(); BX = 0xFFFF; SI = 1;      runLeaGw(5<<3, 0, 0, 0, 0, DS);
        initMem16(); BX = 1;      SI = 0xFFFF; runLeaGw(6<<3, 0, 0, 0, 0, DS);
        initMem16(); BX = 0xFFFF; SI = 0xFFFF; runLeaGw(7<<3, 0, 0, 0, 0xFFFE, DS);

        // [DS:BX + DI]
        initMem16(); BX = 0;      DI = 0;      runLeaGw(0<<3|1, 0, 0, 0, 0, DS);
        initMem16(); BX = 1;      DI = 0;      runLeaGw(1<<3|1, 0, 0, 0, 1, DS);
        initMem16(); BX = 0;      DI = 1;      runLeaGw(2<<3|1, 0, 0, 0, 1, DS);
        initMem16(); BX = 0xFFFF; DI = 0;      runLeaGw(3<<3|1, 0, 0, 0, 0xFFFF, DS);
        initMem16(); BX = 0;      DI = 0xFFFF; runLeaGw(4<<3|1, 0, 0, 0, 0xFFFF, DS);
        initMem16(); BX = 0xFFFF; DI = 1;      runLeaGw(5<<3|1, 0, 0, 0, 0, DS);
        initMem16(); BX = 1;      DI = 0xFFFF; runLeaGw(6<<3|1, 0, 0, 0, 0, DS);
        initMem16(); BX = 0xFFFF; DI = 0xFFFF; runLeaGw(7<<3|1, 0, 0, 0, 0xFFFE, DS);

        // [SS:BP + SI]  
        initMem16(); BP = 0;      SI = 0;      runLeaGw(0<<3|2, 0, 0, 0, 0, SS);
        initMem16(); BP = 1;      SI = 0;      runLeaGw(1<<3|2, 0, 0, 0, 1, SS);
        initMem16(); BP = 0;      SI = 1;      runLeaGw(2<<3|2, 0, 0, 0, 1, SS);
        initMem16(); BP = 0xFFFF; SI = 0;      runLeaGw(3<<3|2, 0, 0, 0, 0xFFFF, SS);
        initMem16(); BP = 0;      SI = 0xFFFF; runLeaGw(4<<3|2, 0, 0, 0, 0xFFFF, SS);
        initMem16(); BP = 0xFFFF; SI = 1;      runLeaGw(5<<3|2, 0, 0, 0, 0, SS);
        initMem16(); BP = 1;      SI = 0xFFFF; runLeaGw(6<<3|2, 0, 0, 0, 0, SS);
        initMem16(); BP = 0xFFFF; SI = 0xFFFF; runLeaGw(7<<3|2, 0, 0, 0, 0xFFFE, SS);

        // [SS:BP + DI]
        initMem16(); BP = 0;      DI = 0;      runLeaGw(0<<3|3, 0, 0, 0, 0, SS);
        initMem16(); BP = 1;      DI = 0;      runLeaGw(1<<3|3, 0, 0, 0, 1, SS);
        initMem16(); BP = 0;      DI = 1;      runLeaGw(2<<3|3, 0, 0, 0, 1, SS);
        initMem16(); BP = 0xFFFF; DI = 0;      runLeaGw(3<<3|3, 0, 0, 0, 0xFFFF, SS);
        initMem16(); BP = 0;      DI = 0xFFFF; runLeaGw(4<<3|3, 0, 0, 0, 0xFFFF, SS);
        initMem16(); BP = 0xFFFF; DI = 1;      runLeaGw(5<<3|3, 0, 0, 0, 0, SS);
        initMem16(); BP = 1;      DI = 0xFFFF; runLeaGw(6<<3|3, 0, 0, 0, 0, SS);
        initMem16(); BP = 0xFFFF; DI = 0xFFFF; runLeaGw(7<<3|3, 0, 0, 0, 0xFFFE, SS);

        // [DS:SI]  
        initMem16(); SI = 0;      runLeaGw(0<<3|4, 0, 0, 0, 0, DS);
        initMem16(); SI = 1;      runLeaGw(1<<3|4, 0, 0, 0, 1, DS);
        initMem16(); SI = 0x2468; runLeaGw(2<<3|4, 0, 0, 0, 0x2468, DS);
        initMem16(); SI = 0xFFFF; runLeaGw(3<<3|4, 0, 0, 0, 0xFFFF, DS);

        // [DS:DI]  
        initMem16(); DI = 0;      runLeaGw(0<<3|5, 0, 0, 0, 0, DS);
        initMem16(); DI = 1;      runLeaGw(1<<3|5, 0, 0, 0, 1, DS);
        initMem16(); DI = 0x2468; runLeaGw(2<<3|5, 0, 0, 0, 0x2468, DS);
        initMem16(); DI = 0xFFFF; runLeaGw(3<<3|5, 0, 0, 0, 0xFFFF, DS);

        // [DS:disp16]
        initMem16(); runLeaGw(0<<3|6, 0, true, 0, 0, DS);
        initMem16(); runLeaGw(1<<3|6, 0, true, 1, 1, DS);
        initMem16(); runLeaGw(2<<3|6, 0, true, 0x2468, 0x2468, DS);
        initMem16(); runLeaGw(3<<3|6, 0, true, 0xFFFF, 0xFFFF, DS);

        // [DS:BX]
        initMem16(); BX = 0;      runLeaGw(0<<3|7, 0, 0, 0, 0, DS);
        initMem16(); BX = 1;      runLeaGw(1<<3|7, 0, 0, 0, 1, DS);
        initMem16(); BX = 0x2468; runLeaGw(2<<3|7, 0, 0, 0, 0x2468, DS);
        initMem16(); BX = 0xFFFF; runLeaGw(3<<3|7, 0, 0, 0, 0xFFFF, DS);

    // Mod 01
        // [DS:BX + SI + disp8]
        initMem16(); BX = 0;      SI = 0;      runLeaGw(0|0x40, true, 0, 0, 0, DS);
        initMem16(); BX = 1;      SI = 0;      runLeaGw(1<<3|0x40, true, 0, 1, 2, DS);
        initMem16(); BX = 1;      SI = 1;      runLeaGw(2<<3|0x40, true, 0, 1, 3, DS);
        initMem16(); BX = 0;      SI = 0;      runLeaGw(3<<3|0x40, true, 0, 0xFF, 0xFFFF, DS);
        initMem16(); BX = 0;      SI = 0xFFFF; runLeaGw(4<<3|0x40, true, 0, 0xFF, 0xFFFE, DS);
        initMem16(); BX = 0xFFFE; SI = 1;      runLeaGw(5<<3|0x40, true, 0, 1, 0, DS);
        initMem16(); BX = 1;      SI = 0xFFFF; runLeaGw(6<<3|0x40, true, 0, 1, 1, DS);
        initMem16(); BX = 0xFFFF; SI = 0xFFFF; runLeaGw(7<<3|0x40, true, 0, 0xFF, 0xFFFD, DS);

        // [DS:BX + DI + disp8]
        initMem16(); BX = 0;      DI = 0;      runLeaGw(0|0x41, true, 0, 0, 0, DS);
        initMem16(); BX = 1;      DI = 0;      runLeaGw(1<<3|0x41, true, 0, 1, 2, DS);
        initMem16(); BX = 1;      DI = 1;      runLeaGw(2<<3|0x41, true, 0, 1, 3, DS);
        initMem16(); BX = 0;      DI = 0;      runLeaGw(3<<3|0x41, true, 0, 0xFF, 0xFFFF, DS);
        initMem16(); BX = 0;      DI = 0xFFFF; runLeaGw(4<<3|0x41, true, 0, 0xFF, 0xFFFE, DS);
        initMem16(); BX = 0xFFFE; DI = 1;      runLeaGw(5<<3|0x41, true, 0, 1, 0, DS);
        initMem16(); BX = 1;      DI = 0xFFFF; runLeaGw(6<<3|0x41, true, 0, 1, 1, DS);
        initMem16(); BX = 0xFFFF; DI = 0xFFFF; runLeaGw(7<<3|0x41, true, 0, 0xFF, 0xFFFD, DS);

        // [SS:BP + SI + disp8]
        initMem16(); BP = 0;      SI = 0;      runLeaGw(0|0x42, true, 0, 0, 0, SS);
        initMem16(); BP = 1;      SI = 0;      runLeaGw(1<<3|0x42, true, 0, 1, 2, SS);
        initMem16(); BP = 1;      SI = 1;      runLeaGw(2<<3|0x42, true, 0, 1, 3, SS);
        initMem16(); BP = 0;      SI = 0;      runLeaGw(3<<3|0x42, true, 0, 0xFF, 0xFFFF, SS);
        initMem16(); BP = 0;      SI = 0xFFFF; runLeaGw(4<<3|0x42, true, 0, 0xFF, 0xFFFE, SS);
        initMem16(); BP = 0xFFFE; SI = 1;      runLeaGw(5<<3|0x42, true, 0, 1, 0, SS);
        initMem16(); BP = 1;      SI = 0xFFFF; runLeaGw(6<<3|0x42, true, 0, 1, 1, SS);
        initMem16(); BP = 0xFFFF; SI = 0xFFFF; runLeaGw(7<<3|0x42, true, 0, 0xFF, 0xFFFD, SS);

        // [SS:BP + DI + disp8]
        initMem16(); BP = 0;      DI = 0;      runLeaGw(0|0x43, true, 0, 0, 0, SS);
        initMem16(); BP = 1;      DI = 0;      runLeaGw(1<<3|0x43, true, 0, 1, 2, SS);
        initMem16(); BP = 1;      DI = 1;      runLeaGw(2<<3|0x43, true, 0, 1, 3, SS);
        initMem16(); BP = 0;      DI = 0;      runLeaGw(3<<3|0x43, true, 0, 0xFF, 0xFFFF, SS);
        initMem16(); BP = 0;      DI = 0xFFFF; runLeaGw(4<<3|0x43, true, 0, 0xFF, 0xFFFE, SS);
        initMem16(); BP = 0xFFFE; DI = 1;      runLeaGw(5<<3|0x43, true, 0, 1, 0, SS);
        initMem16(); BP = 1;      DI = 0xFFFF; runLeaGw(6<<3|0x43, true, 0, 1, 1, SS);
        initMem16(); BP = 0xFFFF; DI = 0xFFFF; runLeaGw(7<<3|0x43, true, 0, 0xFF, 0xFFFD, SS);

        // [DS:SI + disp8]
        initMem16(); SI = 0;      runLeaGw(0|0x44, true, 0, 0, 0, DS);
        initMem16(); SI = 0;      runLeaGw(1<<3|0x44, true, 0, 1, 1, DS);
        initMem16(); SI = 1;      runLeaGw(2<<3|0x44, true, 0, 0, 1, DS);
        initMem16(); SI = 0;      runLeaGw(3<<3|0x44, true, 0, 0xFF, 0xFFFF, DS);
        initMem16(); SI = 0xFFFF; runLeaGw(4<<3|0x44, true, 0, 0, 0xFFFF, DS);
        initMem16(); SI = 1;      runLeaGw(5<<3|0x44, true, 0, 0xFF, 0, DS);
        initMem16(); SI = 0xFFFF; runLeaGw(6<<3|0x44, true, 0, 1, 0, DS);
        initMem16(); SI = 0xFFFF; runLeaGw(7<<3|0x44, true, 0, 0xFF, 0xFFFE, DS);

        // [DS:DI + disp8]
        initMem16(); DI = 0;      runLeaGw(0|0x45, true, 0, 0, 0, DS);
        initMem16(); DI = 0;      runLeaGw(1<<3|0x45, true, 0, 1, 1, DS);
        initMem16(); DI = 1;      runLeaGw(2<<3|0x45, true, 0, 0, 1, DS);
        initMem16(); DI = 0;      runLeaGw(3<<3|0x45, true, 0, 0xFF, 0xFFFF, DS);
        initMem16(); DI = 0xFFFF; runLeaGw(4<<3|0x45, true, 0, 0, 0xFFFF, DS);
        initMem16(); DI = 1;      runLeaGw(5<<3|0x45, true, 0, 0xFF, 0, DS);
        initMem16(); DI = 0xFFFF; runLeaGw(6<<3|0x45, true, 0, 1, 0, DS);
        initMem16(); DI = 0xFFFF; runLeaGw(7<<3|0x45, true, 0, 0xFF, 0xFFFE, DS);

        // [SS:BP + disp8]
        initMem16(); BP = 0;      runLeaGw(0|0x46, true, 0, 0, 0, SS);
        initMem16(); BP = 0;      runLeaGw(1<<3|0x46, true, 0, 1, 1, SS);
        initMem16(); BP = 1;      runLeaGw(2<<3|0x46, true, 0, 0, 1, SS);
        initMem16(); BP = 0;      runLeaGw(3<<3|0x46, true, 0, 0xFF, 0xFFFF, SS);
        initMem16(); BP = 0xFFFF; runLeaGw(4<<3|0x46, true, 0, 0, 0xFFFF, SS);
        initMem16(); BP = 1;      runLeaGw(5<<3|0x46, true, 0, 0xFF, 0, SS);
        initMem16(); BP = 0xFFFF; runLeaGw(6<<3|0x46, true, 0, 1, 0, SS);
        initMem16(); BP = 0xFFFF; runLeaGw(7<<3|0x46, true, 0, 0xFF, 0xFFFE, SS);

        // [DS:BX + disp8]
        initMem16(); BX = 0;      runLeaGw(0|0x47, true, 0, 0, 0, DS);
        initMem16(); BX = 0;      runLeaGw(1<<3|0x47, true, 0, 1, 1, DS);
        initMem16(); BX = 1;      runLeaGw(2<<3|0x47, true, 0, 0, 1, DS);
        initMem16(); BX = 0;      runLeaGw(3<<3|0x47, true, 0, 0xFF, 0xFFFF, DS);
        initMem16(); BX = 0xFFFF; runLeaGw(4<<3|0x47, true, 0, 0, 0xFFFF, DS);
        initMem16(); BX = 1;      runLeaGw(5<<3|0x47, true, 0, 0xFF, 0, DS);
        initMem16(); BX = 0xFFFF; runLeaGw(6<<3|0x47, true, 0, 1, 0, DS);
        initMem16(); BX = 0xFFFF; runLeaGw(7<<3|0x47, true, 0, 0xFF, 0xFFFE, DS);

    // Mod 02
        // [DS:BX + SI + disp16]
        initMem16(); BX = 0;      SI = 0;      runLeaGw(0|0x80, 0, true, 0, 0, DS);
        initMem16(); BX = 1;      SI = 0;      runLeaGw(1<<3|0x80, 0, true, 1, 2, DS);
        initMem16(); BX = 1;      SI = 1;      runLeaGw(2<<3|0x80, 0, true, 1, 3, DS);
        initMem16(); BX = 0;      SI = 0;      runLeaGw(3<<3|0x80, 0, true, 0xFF, 0xFF, DS);
        initMem16(); BX = 0;      SI = 0xFFFF; runLeaGw(4<<3|0x80, 0, true, 0xFFFF, 0xFFFE, DS);
        initMem16(); BX = 0xFFFE; SI = 1;      runLeaGw(5<<3|0x80, 0, true, 1, 0, DS);
        initMem16(); BX = 1;      SI = 0xFFFF; runLeaGw(6<<3|0x80, 0, true, 1, 1, DS);
        initMem16(); BX = 0xFFFF; SI = 0xFFFF; runLeaGw(7<<3|0x80, 0, true, 0xFFFF, 0xFFFD, DS);

        // [DS:BX + DI + disp16]
        initMem16(); BX = 0;      DI = 0;      runLeaGw(0|0x81, 0, true, 0, 0, DS);
        initMem16(); BX = 1;      DI = 0;      runLeaGw(1<<3|0x81, 0, true, 1, 2, DS);
        initMem16(); BX = 1;      DI = 1;      runLeaGw(2<<3|0x81, 0, true, 1, 3, DS);
        initMem16(); BX = 0;      DI = 0;      runLeaGw(3<<3|0x81, 0, true, 0xFF, 0xFF, DS);
        initMem16(); BX = 0;      DI = 0xFFFF; runLeaGw(4<<3|0x81, 0, true, 0xFFFF, 0xFFFE, DS);
        initMem16(); BX = 0xFFFE; DI = 1;      runLeaGw(5<<3|0x81, 0, true, 1, 0, DS);
        initMem16(); BX = 1;      DI = 0xFFFF; runLeaGw(6<<3|0x81, 0, true, 1, 1, DS);
        initMem16(); BX = 0xFFFF; DI = 0xFFFF; runLeaGw(7<<3|0x81, 0, true, 0xFFFF, 0xFFFD, DS);

        // [SS:BP + SI + disp16]
        initMem16(); BP = 0;      SI = 0;      runLeaGw(0|0x82, 0, true, 0, 0, SS);
        initMem16(); BP = 1;      SI = 0;      runLeaGw(1<<3|0x82, 0, true, 1, 2, SS);
        initMem16(); BP = 1;      SI = 1;      runLeaGw(2<<3|0x82, 0, true, 1, 3, SS);
        initMem16(); BP = 0;      SI = 0;      runLeaGw(3<<3|0x82, 0, true, 0xFF, 0xFF, SS);
        initMem16(); BP = 0;      SI = 0xFFFF; runLeaGw(4<<3|0x82, 0, true, 0xFFFF, 0xFFFE, SS);
        initMem16(); BP = 0xFFFE; SI = 1;      runLeaGw(5<<3|0x82, 0, true, 1, 0, SS);
        initMem16(); BP = 1;      SI = 0xFFFF; runLeaGw(6<<3|0x82, 0, true, 1, 1, SS);
        initMem16(); BP = 0xFFFF; SI = 0xFFFF; runLeaGw(7<<3|0x82, 0, true, 0xFFFF, 0xFFFD, SS);

        // [SS:BP + DI + disp16]
        initMem16(); BP = 0;      DI = 0;      runLeaGw(0|0x83, 0, true, 0, 0, SS);
        initMem16(); BP = 1;      DI = 0;      runLeaGw(1<<3|0x83, 0, true, 1, 2, SS);
        initMem16(); BP = 1;      DI = 1;      runLeaGw(2<<3|0x83, 0, true, 1, 3, SS);
        initMem16(); BP = 0;      DI = 0;      runLeaGw(3<<3|0x83, 0, true, 0xFF, 0xFF, SS);
        initMem16(); BP = 0;      DI = 0xFFFF; runLeaGw(4<<3|0x83, 0, true, 0xFFFF, 0xFFFE, SS);
        initMem16(); BP = 0xFFFE; DI = 1;      runLeaGw(5<<3|0x83, 0, true, 1, 0, SS);
        initMem16(); BP = 1;      DI = 0xFFFF; runLeaGw(6<<3|0x83, 0, true, 1, 1, SS);
        initMem16(); BP = 0xFFFF; DI = 0xFFFF; runLeaGw(7<<3|0x83, 0, true, 0xFFFF, 0xFFFD, SS);

        // [DS:SI + disp16]
        initMem16(); SI = 0;      runLeaGw(0|0x84, 0, true, 0, 0, DS);
        initMem16(); SI = 0;      runLeaGw(1<<3|0x84, 0, true, 1, 1, DS);
        initMem16(); SI = 1;      runLeaGw(2<<3|0x84, 0, true, 0, 1, DS);
        initMem16(); SI = 0;      runLeaGw(3<<3|0x84, 0, true, 0xFF, 0xFF, DS);
        initMem16(); SI = 0xFFFF; runLeaGw(4<<3|0x84, 0, true, 0, 0xFFFF, DS);
        initMem16(); SI = 1;      runLeaGw(5<<3|0x84, 0, true, 0xFFFF, 0, DS);
        initMem16(); SI = 0xFFFF; runLeaGw(6<<3|0x84, 0, true, 1, 0, DS);
        initMem16(); SI = 0xFFFF; runLeaGw(7<<3|0x84, 0, true, 0xFFFF, 0xFFFE, DS);

        // [DS:DI + disp16]
        initMem16(); DI = 0;      runLeaGw(0|0x85, 0, true, 0, 0, DS);
        initMem16(); DI = 0;      runLeaGw(1<<3|0x85, 0, true, 1, 1, DS);
        initMem16(); DI = 1;      runLeaGw(2<<3|0x85, 0, true, 0, 1, DS);
        initMem16(); DI = 0;      runLeaGw(3<<3|0x85, 0, true, 0xFF, 0xFF, DS);
        initMem16(); DI = 0xFFFF; runLeaGw(4<<3|0x85, 0, true, 0, 0xFFFF, DS);
        initMem16(); DI = 1;      runLeaGw(5<<3|0x85, 0, true, 0xFFFF, 0, DS);
        initMem16(); DI = 0xFFFF; runLeaGw(6<<3|0x85, 0, true, 1, 0, DS);
        initMem16(); DI = 0xFFFF; runLeaGw(7<<3|0x85, 0, true, 0xFFFF, 0xFFFE, DS);

        // [SS:BP + disp16]
        initMem16(); BP = 0;      runLeaGw(0|0x86, 0, true, 0, 0, SS);
        initMem16(); BP = 0;      runLeaGw(1<<3|0x86, 0, true, 1, 1, SS);
        initMem16(); BP = 1;      runLeaGw(2<<3|0x86, 0, true, 0, 1, SS);
        initMem16(); BP = 0;      runLeaGw(3<<3|0x86, 0, true, 0xFF, 0xFF, SS);
        initMem16(); BP = 0xFFFF; runLeaGw(4<<3|0x86, 0, true, 0, 0xFFFF, SS);
        initMem16(); BP = 1;      runLeaGw(5<<3|0x86, 0, true, 0xFFFF, 0, SS);
        initMem16(); BP = 0xFFFF; runLeaGw(6<<3|0x86, 0, true, 1, 0, SS);
        initMem16(); BP = 0xFFFF; runLeaGw(7<<3|0x86, 0, true, 0xFFFF, 0xFFFE, SS);

        // [DS:BX + disp16]
        initMem16(); BX = 0;      runLeaGw(0|0x87, 0, true, 0, 0, DS);
        initMem16(); BX = 0;      runLeaGw(1<<3|0x87, 0, true, 1, 1, DS);
        initMem16(); BX = 1;      runLeaGw(2<<3|0x87, 0, true, 0, 1, DS);
        initMem16(); BX = 0;      runLeaGw(3<<3|0x87, 0, true, 0xFF, 0xFF, DS);
        initMem16(); BX = 0xFFFF; runLeaGw(4<<3|0x87, 0, true, 0, 0xFFFF, DS);
        initMem16(); BX = 1;      runLeaGw(5<<3|0x87, 0, true, 0xFFFF, 0, DS);
        initMem16(); BX = 0xFFFF; runLeaGw(6<<3|0x87, 0, true, 1, 0, DS);
        initMem16(); BX = 0xFFFF; runLeaGw(7<<3|0x87, 0, true, 0xFFFF, 0xFFFE, DS);
}


int main(int argc, char **argv) {	
    printf("Please wait, these first 2 tests can take a while\n");
    run(test32BitMemoryAccess, "32-bit Memory Access");
    run(test16BitMemoryAccess, "16-bit Memory Access");

    run(testAdd0x000, "Add 000");
    run(testAdd0x200, "Add 200");
    run(testAdd0x001, "Add 001");
    run(testAdd0x201, "Add 201");
    run(testAdd0x002, "Add 002");
    run(testAdd0x202, "Add 202");
    run(testAdd0x003, "Add 003");
    run(testAdd0x203, "Add 203");
    run(testAdd0x004, "Add 004");
    run(testAdd0x204, "Add 204");
    run(testAdd0x005, "Add 005");
    run(testAdd0x205, "Add 205");
    run(testPushEs0x006, "Push ES 006");
    run(testPushEs0x206, "Push ES 206");
    run(testPopEs0x007, "Pop ES 007");
    run(testPopEs0x207, "Pop ES 207");
    run(testOr0x008, "Or  008");
    run(testOr0x208, "Or  208");
    run(testOr0x009, "Or  009");
    run(testOr0x209, "Or  209");
    run(testOr0x00a, "Or  00a");
    run(testOr0x20a, "Or  20a");
    run(testOr0x00b, "Or  00b");
    run(testOr0x20b, "Or  20b");
    run(testOr0x00c, "Or  00c");
    run(testOr0x20c, "Or  20c");
    run(testOr0x00d, "Or  00d");
    run(testOr0x20d, "Or  20d");
    run(testPushCs0x00e, "Push CS 00e");
    run(testPushCs0x20e, "Push CS 20e");
    // 0x0f is a prefix byte
    run(testAdc0x010, "Adc 010");
    run(testAdc0x210, "Adc 210");
    run(testAdc0x011, "Adc 011");
    run(testAdc0x211, "Adc 211");
    run(testAdc0x012, "Adc 012");
    run(testAdc0x212, "Adc 212");
    run(testAdc0x013, "Adc 013");
    run(testAdc0x213, "Adc 213");
    run(testAdc0x014, "Adc 014");
    run(testAdc0x214, "Adc 214");
    run(testAdc0x015, "Adc 015");
    run(testAdc0x215, "Adc 215");
    run(testPushSs0x016, "Push SS 016");
    run(testPushSs0x216, "Push SS 216");
    run(testPopSs0x017, "Pop SS 017");
    run(testPopSs0x217, "Pop SS 217");
    run(testSbb0x018, "Sbb 018");
    run(testSbb0x218, "Sbb 218");
    run(testSbb0x019, "Sbb 019");
    run(testSbb0x219, "Sbb 219");
    run(testSbb0x01a, "Sbb 01a");
    run(testSbb0x21a, "Sbb 21a");
    run(testSbb0x01b, "Sbb 01b");
    run(testSbb0x21b, "Sbb 21b");
    run(testSbb0x01c, "Sbb 01c");
    run(testSbb0x21c, "Sbb 21c");
    run(testSbb0x01d, "Sbb 01d");
    run(testSbb0x21d, "Sbb 21d");
    run(testPushDs0x01e, "Push DS 01e");
    run(testPushDs0x21e, "Push DS 21e");
    run(testPopDs0x01f, "Pop DS 01f");
    run(testPopDs0x21f, "Pop DS 21f");
    run(testAnd0x020, "And 020");
    run(testAnd0x220, "And 220");
    run(testAnd0x021, "And 021");
    run(testAnd0x221, "And 221");
    run(testAnd0x022, "And 022");
    run(testAnd0x222, "And 222");
    run(testAnd0x023, "And 023");
    run(testAnd0x223, "And 223");
    run(testAnd0x024, "And 024");
    run(testAnd0x224, "And 224");
    run(testAnd0x025, "And 025");
    run(testAnd0x225, "And 225");
    run(testSegEs0x026, "Seg ES 026");
    run(testSegEs0x226, "Seg ES 226");
    run(testDaa0x027, "DAA 027");
    run(testDaa0x227, "DAA 227");
    run(testSub0x028, "Sub 028");
    run(testSub0x228, "Sub 228");
    run(testSub0x029, "Sub 029");
    run(testSub0x229, "Sub 229");
    run(testSub0x02a, "Sub 02a");
    run(testSub0x22a, "Sub 22a");
    run(testSub0x02b, "Sub 02b");
    run(testSub0x22b, "Sub 22b");
    run(testSub0x02c, "Sub 02c");
    run(testSub0x22c, "Sub 22c");
    run(testSub0x02d, "Sub 02d");
    run(testSub0x22d, "Sub 22d");
    run(testSegCs0x02e, "Seg CS 02e");
    run(testSegCs0x22e, "Seg CS 22e");
    run(testDas0x02f, "DAS 02f");
    run(testDas0x22f, "DAS 22f");
    run(testXor0x030, "Xor 030");
    run(testXor0x230, "Xor 230");
    run(testXor0x031, "Xor 031");
    run(testXor0x231, "Xor 231");
    run(testXor0x032, "Xor 032");
    run(testXor0x232, "Xor 232");
    run(testXor0x033, "Xor 033");
    run(testXor0x233, "Xor 233");
    run(testXor0x034, "Xor 034");
    run(testXor0x234, "Xor 234");
    run(testXor0x035, "Xor 035");
    run(testXor0x235, "Xor 235");
    run(testSegSs0x036, "Seg SS 036");
    run(testSegSs0x236, "Seg SS 236");
    run(testAaa0x037, "AAA 037");
    run(testAaa0x237, "AAA 237");
    run(testCmp0x038, "Cmp 038");
    run(testCmp0x238, "Cmp 238");
    run(testCmp0x039, "Cmp 039");
    run(testCmp0x239, "Cmp 239");
    run(testCmp0x03a, "Cmp 03a");
    run(testCmp0x23a, "Cmp 23a");
    run(testCmp0x03b, "Cmp 03b");
    run(testCmp0x23b, "Cmp 23b");
    run(testCmp0x03c, "Cmp 03c");
    run(testCmp0x23c, "Cmp 23c");
    run(testCmp0x03d, "Cmp 03d");
    run(testCmp0x23d, "Cmp 23d");
    run(testSegDs0x03e, "Seg DS 03e");
    run(testSegDs0x23e, "Seg DS 23e");
    run(testAas0x03f, "AAS 03f");
    run(testAas0x23f, "AAS 23f");
    run(testIncAx0x040,  "Inc AX  040");
    run(testIncEax0x240, "Inc EAX 240");
    run(testIncCx0x041,  "Inc CX  041");
    run(testIncEcx0x241, "Inc ECX 241");
    run(testIncDx0x042,  "Inc DX  042");
    run(testIncEdx0x242, "Inc EDX 242");
    run(testIncBx0x043,  "Inc BX  043");
    run(testIncEbx0x243, "Inc EBX 243");
    run(testIncSp0x044,  "Inc SP  044");
    run(testIncEsp0x244, "Inc ESP 244");
    run(testIncBp0x045,  "Inc BP  045");
    run(testIncEbp0x245, "Inc EBP 245");
    run(testIncSi0x046,  "Inc SI  046");
    run(testIncEsi0x246, "Inc ESI 246");
    run(testIncDi0x047,  "Inc DI  047");
    run(testIncEdi0x247, "Inc EDI 247");
    run(testDecAx0x048,  "Dec AX  048");
    run(testDecEax0x248, "Dec EAX 248");
    run(testDecCx0x049,  "Dec CX  049");
    run(testDecEcx0x249, "Dec ECX 249");
    run(testDecDx0x04a,  "Dec DX  04a");
    run(testDecEdx0x24a, "Dec EDX 24a");
    run(testDecBx0x04b,  "Dec BX  04b");
    run(testDecEbx0x24b, "Dec EBX 24b");
    run(testDecSp0x04c,  "Dec SP  04c");
    run(testDecEsp0x24c, "Dec ESP 24c");
    run(testDecBp0x04d,  "Dec BP  04d");
    run(testDecEbp0x24d, "Dec EBP 24d");
    run(testDecSi0x04e,  "Dec SI  04e");
    run(testDecEsi0x24e, "Dec ESI 24e");
    run(testDecDi0x04f,  "Dec DI  04f");
    run(testDecEdi0x24f, "Dec EDI 24f");
    run(testPushAx0x050,  "Push Ax  050");
    run(testPushEax0x250, "Push Eax 250");
    run(testPushCx0x051,  "Push Cx  051");
    run(testPushEcx0x251, "Push Ecx 251");
    run(testPushDx0x052,  "Push Dx  052");
    run(testPushEdx0x252, "Push Edx 252");
    run(testPushBx0x053,  "Push Bx  053");
    run(testPushEbx0x253, "Push Ebx 253");
    run(testPushSp0x054,  "Push Sp  054");
    run(testPushEsp0x254, "Push Esp 254");
    run(testPushBp0x055,  "Push Bp  055");
    run(testPushEbp0x255, "Push Ebp 255");
    run(testPushSi0x056,  "Push Si  056");
    run(testPushEsi0x256, "Push Esi 256");
    run(testPushDi0x057,  "Push Di  057");
    run(testPushEdi0x257, "Push Edi 257");
    run(testPopAx0x058,  "Pop Ax  058");
    run(testPopEax0x258, "Pop Eax 258");
    run(testPopCx0x059,  "Pop Cx  059");
    run(testPopEcx0x259, "Pop Ecx 259");
    run(testPopDx0x05a,  "Pop Dx  05a");
    run(testPopEdx0x25a, "Pop Edx 25a");
    run(testPopBx0x05b,  "Pop Bx  05b");
    run(testPopEbx0x25b, "Pop Ebx 25b");
    run(testPopSp0x05c,  "Pop Sp  05c");
    run(testPopEsp0x25c, "Pop Esp 25c");
    run(testPopBp0x05d,  "Pop Bp  05d");
    run(testPopEbp0x25d, "Pop Ebp 25d");
    run(testPopSi0x05e,  "Pop Si  05e");
    run(testPopEsi0x25e, "Pop Esi 25e");
    run(testPopDi0x05f,  "Pop Di  05f");
    run(testPopEdi0x25f, "Pop Edi 25f");

    run(testPush0x068, "Push 068");
    run(testPush0x268, "Push 268");

    run(testIMul0x069, "IMul 069");
    run(testIMul0x269, "IMul 269");

    run(testPush0x06a, "Push 06a");
    run(testPush0x26a, "Push 26a");

    run(testIMul0x06b, "IMul 06b");
    run(testIMul0x26b, "IMul 26b");

    run(testGrp10x080, "Grp1 080");
    run(testGrp10x280, "Grp1 280");
    run(testGrp10x081, "Grp1 081");
    run(testGrp10x281, "Grp1 281");
    run(testGrp10x083, "Grp1 083");
    run(testGrp10x283, "Grp1 283");

    run(testTest0x084, "Test 084");
    run(testTest0x284, "Test 284");
    run(testTest0x085, "Test 085");
    run(testTest0x285, "Test 285");
    
    run(testXchg0x086, "Xchg 086");
    run(testXchg0x286, "Xchg 286");
    run(testXchg0x087, "Xchg 087");
    run(testXchg0x287, "Xchg 287");

    run(testMovEbGb0x088, "Mov 088");
    run(testMovEbGb0x288, "Mov 288");
    run(testMovEwGw0x089, "Mov 089");
    run(testMovEdGd0x289, "Mov 289");

    run(testMovEbGb0x08a, "Mov 08a");
    run(testMovEbGb0x28a, "Mov 28a");
    run(testMovEwGw0x08b, "Mov 08b");
    run(testMovEdGd0x28b, "Mov 28b");

    run(testMovEwSw0x08c, "Mov 08c");
    run(testMovEwSw0x28c, "Mov 28c");

    run(testLeaGw0x08d, "Lea 08d");
    run(testLeaGd0x28d, "Lea 28d");

    // :TODO: 0x08e
    // :TODO: 0x28e

    run(testPopEw0x08f, "Pop 08f");
    run(testPopEd0x28f, "Pop 28f");

    // Pause 290 (SSE2)

    run(testXchgCxAx0x091,   "Xchg 091");
    run(testXchgEcxEax0x291, "Xchg 291");
    run(testXchgDxAx0x092,   "Xchg 092");
    run(testXchgEdxEax0x292, "Xchg 292");
    run(testXchgBxAx0x093,   "Xchg 093");
    run(testXchgEbxEax0x293, "Xchg 293");
    run(testXchgSpAx0x094,   "Xchg 094");
    run(testXchgEspEax0x294, "Xchg 294");
    run(testXchgBpAx0x095,   "Xchg 095");
    run(testXchgEbpEax0x295, "Xchg 295");
    run(testXchgSiAx0x096,   "Xchg 096");
    run(testXchgEsiEax0x296, "Xchg 296");
    run(testXchgDiAx0x097,   "Xchg 097");
    run(testXchgEdiEax0x297, "Xchg 297");
    
    run(testCbw0x098, "Cbw  098");
    run(testCwde0x298, "Cwde 298");
    run(testCwd0x099, "Cwd  099");
    run(testCdq0x299, "Cdq  299");
    
    run(testPushf0x09c, "Pushf 09c");
    run(testPushf0x29c, "Pushf 29c");
    run(testPopf0x09d,  "Popf  09d");
    run(testPopf0x29d,  "Popf  29d");

    run(testSahf0x09e, "Sahf 09e");
    run(testSahf0x29e, "Sahf 29e");
    run(testLahf0x09f, "Lahf 09f");
    run(testLahf0x29f, "Lahf 29f");

    // :TODO: 0xa0 - 0xaf
    run(testCmpsb0x0a6, "Cmpsb 0a6");
    run(testCmpsb0x2a6, "Cmpsb 2a6");

    run(testCmpsw0x0a7, "Cmpsw 0a7");
    run(testCmpsd0x2a7, "Cmpsd 2a7");
    
    run(testMovAlIb0x0b0, "Mov 0b0");
    run(testMovAlIb0x2b0, "Mov 2b0");
    run(testMovClIb0x0b1, "Mov 0b1");
    run(testMovClIb0x2b1, "Mov 2b1");
    run(testMovDlIb0x0b2, "Mov 0b2");
    run(testMovDlIb0x2b2, "Mov 2b2");
    run(testMovBlIb0x0b3, "Mov 0b3");
    run(testMovBlIb0x2b3, "Mov 2b3");
    run(testMovAhIb0x0b4, "Mov 0b4");
    run(testMovAhIb0x2b4, "Mov 2b4");
    run(testMovChIb0x0b5, "Mov 0b5");
    run(testMovChIb0x2b5, "Mov 2b5");
    run(testMovDhIb0x0b6, "Mov 0b6");
    run(testMovDhIb0x2b6, "Mov 2b6");
    run(testMovBhIb0x0b7, "Mov 0b7");
    run(testMovBhIb0x2b7, "Mov 2b7");
    run(testMovAxIw0x0b8, "Mov 0b8");
    run(testMovEaxId0x2b8, "Mov 2b8");
    run(testMovCxIw0x0b9, "Mov 0b9");
    run(testMovEcxId0x2b9, "Mov 2b9");
    run(testMovDxIw0x0ba, "Mov 0ba");
    run(testMovEdxId0x2ba, "Mov 2ba");
    run(testMovBxIw0x0bb, "Mov 0bb");
    run(testMovEbxId0x2bb, "Mov 2bb");
    run(testMovSpIw0x0bc, "Mov 0bc");
    run(testMovEspId0x2bc, "Mov 2bc");
    run(testMovBpIw0x0bd, "Mov 0bd");
    run(testMovEbpId0x2bd, "Mov 2bd");
    run(testMovSiIw0x0be, "Mov 0be");
    run(testMovEsiId0x2be, "Mov 2be");
    run(testMovDiIw0x0bf, "Mov 0bf");
    run(testMovEdiId0x2bf, "Mov 2bf");

    run(testGrp20x0c0, "Grp2 0c0");
    run(testGrp20x2c0, "Grp2 2c0");
    run(testGrp20x0c1, "Grp2 0c1");
    run(testGrp20x2c1, "Grp2 2c1");
    
    run(testMovEbIb0x0c6, "Mov 0c6");
    run(testMovEbIb0x2c6, "Mov 2c6");
    run(testMovEwIw0x0c7, "Mov 0c7");
    run(testMovEdId0x2c7, "Mov 2c7");
    
    run(testGrp20x0d0, "Grp2 0d0");
    run(testGrp20x2d0, "Grp2 2d0");
    run(testGrp20x0d1, "Grp2 0d1");
    run(testGrp20x2d1, "Grp2 2d1");
    run(testGrp20x0d2, "Grp2 0d2");
    run(testGrp20x2d2, "Grp2 2d2");
    run(testGrp20x0d3, "Grp2 0d3");
    run(testGrp20x2d3, "Grp2 2d3");
    run(testAam0x0d4, "AAM 0d4");
    run(testAam0x2d4, "AAM 2d4");
    run(testAad0x0d5, "AAD 0d5");
    run(testAad0x2d5, "AAD 2d5");
    run(testSalc0x0d6, "Salc 0d6");
    run(testSalc0x2d6, "Salc 2d6");

    run(testXlat0x0d7, "Xlat 0d7");
    run(testXlat0x2d7, "Xlat 2d7");

    run(testFPU0x0d8, "FPU 0d8");
    run(testFPU0x2d8, "FPU 2d8");

#ifndef BOXEDWINE_X64
    run(testFPU0x0d9, "FPU 0d9");
    run(testFPU0x2d9, "FPU 2d9");    
#endif
    run(testCmc0x0f5, "Cmc 0f5");
    run(testCmc0x2f5, "Cmc 2f5");
    
    run(testGrp30x0f6, "Grp3 0f6");
    run(testGrp30x2f6, "Grp3 2f6");
    run(testGrp30x0f7, "Grp3 0f7");
    run(testGrp30x2f7, "Grp3 2f7");

    run(testClc0x0f8, "Clc 0f8");
    run(testClc0x2f8, "Clc 2f8");
    run(testStc0x0f8, "Stc 0f9");
    run(testStc0x2f8, "Stc 2f9");

    run(testGrp40x0fe, "Grp4 0fe");
    run(testGrp40x2fe, "Grp4 2fe");
    run(testGrp50x0ff, "Grp5 0ff");
    run(testGrp50x2ff, "Grp5 2ff");
    
    run(testSse2MovUps110, "MOVUPD 110 (sse2)");
    run(testSseMovUps310, "MOVUPS 310 (sse1)");
    run(testSse2MovSd310, "MOVSD F2 310 (sse2)");
    run(testSseMovSs310, "MOVSS F3 310 (sse1)");
    run(testSse2MovPd111, "MOVUPD 111 (sse2)");
    run(testSseMovUps311, "MOVUPS 311 (sse1)");
    run(testSse2MovSd311, "MOVSD F2 311 (sse2)");
    run(testSseMovSs311, "MOVSS F3 311 (sse1)");
    run(testSse2MovLpd112, "MOVLPD 112 (sse2)");
    run(testSseMovHlps312, "MOVHLPS 312 (sse1)");
    run(testSseMovLps312, "MOVLPS 312 (sse1)");
    run(testSse2MovLpd113, "MOVLPD 113 (sse2)");
    run(testSseMovLps313, "MOVLPS 313 (sse1)");
    run(testSse2Unpcklpd114, "UNPCKLPD 114 (sse2)");
    run(testSseUnpcklps314, "UNPCKLPS 314 (sse1)");
    run(testSse2Unpckhpd115, "UNPCKHPD 115 (sse2)");
    run(testSseUnpckhps315, "UNPCKHPS 315 (sse1)");
    run(testSse2Movhpd116, "MOVHPD 116 (sse2)");
    run(testSseMovlhps316, "MOVLHPS 316 (sse1)");
    run(testSseMovhps316, "MOVHPS 316 (sse1)");
    run(testSse2Movhpd117, "MOVHPD 117 (sse2)");
    run(testSseMovhps317, "MOVHPS 317 (sse1)");
    // PREFETCHNTA 318/0 (sse1)
    // PREFETCHT0 318/1 (sse1)
    // PREFETCHT1 318/2 (sse1)
    // PREFETCHT2 318/3 (sse1)
    // HINT_NOP 318/4 (sse1)
    // HINT_NOP 318/5 (sse1)
    // HINT_NOP 318/6 (sse1)
    // HINT_NOP 318/7 (sse1)

    run(testSse2Movapd128, "MOVAPD 128 (sse2)");
    run(testSseMovaps328, "MOVAPS 328 (sse1)");
    run(testSse2Movapd129, "MOVAPD 129 (sse2)");
    run(testSseMovaps329, "MOVAPS 329 (sse1)");
    run(testSse2Cvtpi2pd12a, "CVTPI2PD 12A (sse2)");
    run(testSseCvtpi2ps32a, "CVTPI2PS 32A (sse1)");
    run(testSse2Cvtsi2sd32a, "CVTSI2SD F2 32A (sse2)");
    run(testSseCvtsi2ss32a, "CVTSI2SS F3 32A (sse1)");
    run(testSse2Movntpd12b, "MOVNTPD 12B (sse2)");
    run(testSseMovntps32b, "MOVNTPS 32B (sse1)");
    run(testSseCvttpd2pi12c, "CVTTPD2PI 12C (sse2)");
    run(testSseCvttps2pi32c, "CVTTPS2PI 32C (sse1)");
    run(testSse2Cvttsd2si32c, "CVTTSD2SI F2 32C (sse2)");
    run(testSseCvttss2si32c, "CVTTSS2SI F3 32C (sse1)");
    run(testSse2Cvtpd2pi12d, "CVTPD2PI 12D (sse2)");
    run(testSseCvtps2pi32d, "CVTPS2PI 32D (sse1)");
    run(testSse2Cvtsd2si32d, "CVTSD2SI F2 32D (sse2)");
    run(testSseCvtss2si32d, "CVTSS2SI F3 32D (sse1)");
    run(testSse2Ucomisd12e, "UCOMISD 12E (sse2)");
    run(testSseUcomiss32e, "UCOMISS 32E (sse1)");
    run(testSse2Comisd12f, "COMISD 12F (sse2)");
    run(testSseComiss32f, "COMISS 32F (sse1)");

    run(testSse2Movmskpd150, "MOVMSKPD 150 (sse2)");
    run(testSseMovmskps350, "MOVMSKPS 350 (sse1)");
    run(testSse2Sqrtpd151, "SQRTPD 151 (sse2)");
    run(testSseSqrtps351, "SQRTPS 351 (sse1)");
    run(testSse2Sqrtsd351, "SQRTSD F2 351 (sse2)");
    run(testSseSqrtss351, "SQRTSS F3 351 (sse1)");
    run(testSseRsqrtps352, "RSQRTPS 352 (sse1)");
    run(testSseRsqrtss352, "RSQRTSS F3 352 (sse1)");
    run(testSseRcpps353, "RCPPS 353 (sse1)");
    run(testSseRcpss353, "RCPSS F3 353 (sse1)");
    run(testSse2Andpd154, "ANDPD 154 (sse2)");
    run(testSseAndps354, "ANDPS 354 (sse1)");
    run(testSse2Andnpd155, "ANDNPD 155 (sse2)");
    run(testSseAndnps355, "ANDNPS 355 (sse1)");
    run(testSse2Orpd156, "ORPD 156 (sse2)");
    run(testSseOrps356, "ORPS 356 (sse1)");
    run(testSse2Xorpd157, "XORPD 157 (sse2)");
    run(testSseXorps357, "XORPS 357 (sse1)");
    run(testSse2Addpd158, "ADDPD 158 (sse2)");
    run(testSseAddps358, "ADDPS 358 (sse1)");
    run(testSse2Addsd358, "ADDSD F2 358 (sse2)");
    run(testSseAddss358, "ADDSS F3 358 (sse1)");
    run(testSse2Mulpd159, "MULPD 159 (sse2)");
    run(testSseMulps359, "MULPS 359 (sse1)");
    run(testSse2Mulsd359, "MULSD F2 359 (sse2)");
    run(testSseMulss359, "MULSS F3 359 (sse1)");
    run(testSse2Cvtpd2ps15a, "CVTPD2PS 15A (sse2)");
    run(testSse2Cvtps2pd35a, "CVTPS2PD 35A (sse2)");
    run(testSse2Cvtsd2ss35a, "CVTSD2SS F2 35A (sse2)");
    run(testSse2Cvtss2sd35a, "CVTSS2SD F3 35A (sse2)");
    run(testSse2Cvtps2dq15b, "CVTPS2DQ 15B (sse2)");
    run(testSse2Cvtdq2ps35b, "CVTDQ2PS 35B (sse2)");
    run(testSse2Cvttps2dq35b, "CVTTPS2DQ F3 35B (sse2)");
    run(testSse2Subpd15c, "SUBPD 15C (sse2)");
    run(testSseSubps35c, "SUBPS 35C (sse1)");
    run(testSse2Subsd35c, "SUBSD F2 35C (sse2)");
    run(testSseSubss35c, "SUBSS F3 35C (sse1)");
    run(testSse2Minpd15d, "MINPD 15D (sse2)");
    run(testSseMinps35d, "MINPS 35D (sse1)");
    run(testSse2Minsd35d, "MINSD F2 35D (sse2)");
    run(testSseMinss35d, "MINSS F3 35D (sse1)");
    run(testSse2Divpd15e, "DIVPD 15E (sse2)");
    run(testSseDivps35e, "DIVPS 35E (sse1)");
    run(testSse2Divsd35e, "DIVSD F2 35E (sse2)");
    run(testSseDivss35e, "DIVSS F3 35E (sse1)");
    run(testSse2Maxpd15f, "MAXPD 15F (sse2)");
    run(testSseMaxps35f, "MAXPS 35F (sse1)");
    run(testSse2Maxsd35f, "MAXSD F2 35F (sse2)");
    run(testSseMaxss35f, "MAXSS F3 35F (sse1)");

    run(testSse2Punpcklbw160, "PUNPCKLBW 160 (sse2)");
    run(testMmxPunpcklbw, "PUNPCKLBW 360 (mmx)");
    run(testSse2xPunpcklwd161, "PUNPCKLWD 161 (sse2)");
    run(testMmxPunpcklwd, "PUNPCKLWD 361 (mmx)");
    run(testSse2Punpckldq162, "PUNPCKLDQ 162 (sse2)");
    run(testMmxPunpckldq, "PUNPCKLDQ 362 (mmx)");
    run(testSse2Packsswb163, "PACKSSWB 163 (sse2)");
    run(testMmxPacksswb, "PACKSSWB 363 (mmx)");
    run(testSse2Pcmpgtb164, "PCMPGTB 164 (sse2)");
    run(testMmxPcmpgtb, "PCMPGTB 364 (mmx)");
    run(testSse2Pcmpgtw165, "PCMPGTW 165 (sse2)");
    run(testMmxPcmpgtw, "PCMPGTW 365 (mmx)");
    run(testSse2Pcmpgtd166, "PCMPGTD 166 (sse2)");
    run(testMmxPcmpgtd, "PCMPGTD 366 (mmx)");
    run(testSse2Packuswb167, "PACKUSWB 167 (sse2)");
    run(testMmxPackuswb, "PACKUSWB 367 (mmx)");
    run(testSse2Punpckhbw168, "PUNPCKHBW 168 (sse2)");
    run(testMmxPunpckhbw, "PUNPCKHBW 368 (mmx)");
    run(testSse2Punpckhwd169, "PUNPCKHWD 169 (sse2)");
    run(testMmxPunpckhwd, "PUNPCKHWD 369 (mmx)");
    run(testSse2Punpckhdq16a, "PUNPCKHDQ 16A (sse2)");
    run(testMmxPunpckhdq, "PUNPCKHDQ 36a (mmx)");
    run(testSse2Packssdw16b, "PACKSSDW 16B (sse2)");
    run(testMmxPackssdw, "PACKSSDW 36b (mmx)");            
    run(testSse2Punpcklqdq16c, "PUNPCKLQDQ 16C (sse2)");
    run(testMmxMovdToMmx, "MOVD 36e (mmx)");
    run(testSse2Punpckhqdq16d, "PUNPCKHQDQ 16D (sse2)");
    run(testSse2Movd16e, "MOVD 16E (sse2)");
    run(testSse2Movdqa16f, "MOVDQA 16F (sse2)");
    run(testMmxMovqToMmx, "MOVQ 36f (mmx)");
    run(testSse2Movdqu36f, "MOVDQU F3 36F (sse2)");

    run(testSse2Pshufd170, "PSHUFD 170 (sse2)");
    run(testSsePshufw370, "PSHUFW 370 (sse1)");
    run(testSse2Pshuflw370, "PSHUFLW F2 370 (sse2)");
    run(testSse2Pshufhw370, "PSHUFHW F3 370 (sse2)");
    run(testSse2Psrlw171, "PSRLW 171/2 (sse2)");
    run(testMmxPsrlwImm8, "PSRLW 371/2 (mmx)");
    run(testSse2Psraw171, "PSRAW 171/4 (sse2)");
    run(testMmxPsrawImm8, "PSRAW 371/4 (mmx)");
    run(testSse2Psllw171, "PSLLW 171/6 (sse2)");
    run(testMmxPsllwImm8, "PSLLW 371/6 (mmx)");
    run(testSse2Psrld172, "PSRLD 172/2 (sse2)");
    run(testMmxPsrldImm8, "PSRLD 372/2 (mmx)");
    run(testSse2Psrad172, "PSRAD 172/4 (sse2)");
    run(testMmxPsradImm8, "PSRAD 372/4 (mmx)");
    run(testSse2Pslld172, "PSLLD 172/6 (sse2)");
    run(testMmxPslldImm8, "PSLLD 372/6 (mmx)");
    run(testSse2Psrlq173, "PSRLQ 173/2 (sse2)");
    run(testMmxPsrlqImm8, "PSRLQ 373/2 (mmx)");
    run(testSse2Psrldq173, "PSRLDQ 173/3 (sse2)");
    run(testSse2Psllq173, "PSLLQ 173/6 (sse2)");
    run(testMmxPsllqImm8, "PSLLQ 373/6 (mmx)");    
    run(testSse2Pslldq173, "PSLLDQ 173/7 (sse2)");
    run(testSse2Pcmpeqb174, "PCMPEQB 174 (sse2)");
    run(testMmxPcmpeqb, "PCMPEQB 374 (mmx)");
    run(testSse2Pcmpeqw175, "PCMPEQW 175 (sse2)");
    run(testMmxPcmpeqw, "PCMPEQW 375 (mmx)");
    run(testSse2Pcmpeqd176, "PCMPEQD 176 (sse2)");
    run(testMmxPcmpeqd, "PCMPEQD 376 (mmx)");
    // :TODO: EMMS 377 (mmx)
    run(testSse2Movd17e, "MOVD 17E (sse2)");
    run(testMmxMovdToE, "MOVD 37e (mmx)");
    run(testSse2Movq37e, "MOVQ F3 37E (sse2)");
    run(testSse2Movdqa17f, "MOVDQA 17F (sse2)");
    run(testMmxMovqToE, "MOVQ 37f (mmx)");
    run(testSse2Movdqu37f, "MOVDQU F3 37F (sse2)");

    run(testBt0x1a3, "BT 1a3");
    run(testBt0x3a3, "BT 3a3");
    run(testShld0x1a4, "SHLD 1a4");
    run(testShld0x3a4, "SHLD 3a4");
    run(testShld0x1a5, "SHLD 1a5");
    run(testShld0x3a5, "SHLD 3a5");
    run(testBts0x1ab, "BTS 1ab");
    run(testBts0x3ab, "BTS 3ab");
    run(testShrd0x1ac, "SHRD 1ac");
    run(testShrd0x3ac, "SHRD 3ac");
    run(testShrd0x1ad, "SHRD 1ad");
    run(testShrd0x3ad, "SHRD 3ad");
    // LDMXCSR 3AE/2 (sse1)
    // STMXCSR 3AE/3 (sse1)
    // LFENCE 3AE/5 (sse2)
    // MFENCE 3AE/6 (sse2)
    // SFENCE 3AE/7 (sse1)
    // CLFLUSH 3AE/7 (sse2)

    run(testCmpXchg0x3b1, "CMPXCHG 3b1");

    run(testXadd0x3c1, "XADD 3c1");    
    run(testSse2Cmppd1c2, "CMPPD 1C2 (sse2)");
    run(testCmpps0x3c2, "CMPPS 3C2 (sse1)");
    run(testSse2Cmpsd3c2, "CMPSD F2 3C2 (sse2)");
    run(testCmpss0x3c2, "CMPSS F3 3C2 (sse1)");
    run(testSse2Movnti3c3, "MOVNTI 3C3 (sse2)");
    run(testPinsrw3c4, "PINSRW 3C4 (sse1)");
    run(testPextrw3c5, "PEXTRW 3C5 (sse1)");
    run(testSse2Shufpd1c6, "SHUFPD 1C6 (sse2)");
    run(testShufps3c6, "SHUFPS 3C6 (sse1)");

    run(testSse2Psrlw1d1, "PSRLW 1D1 (sse2)");
    run(testMmxPsrlw, "PSRLW 3D1 (mmx)");    
    run(testSse2Psrld1d2, "PSRLD 1D2 (sse2)");
    run(testMmxPsrld, "PSRLD 3D2 (mmx)");    
    run(testSse2Psrlq1d3, "PSRLQ 1D3 (sse2)");
    run(testMmxPsrlq, "PSRLQ 3D3 (mmx)");
    run(testSse2Paddq1d4, "PADDQ 1D4 (sse2)");
    run(testMmxPaddq3d4, "PADDQ 3D4 (mmx)");
    run(testSse2Pmullw1d5, "PMULLW 1D5 (sse2)");
    run(testMmxPmullw, "PMULLW 3d5 (mmx)");
    run(testSse2Movq1d6, "MOVQ 1D6 (sse2)");
    run(testSse2Movdq2q3d6, "MOVDQ2Q F2 3D6 (sse2)");
    run(testSse2Movq2dq3d6, "MOVQ2DQ F3 3D6 (sse2)");
    run(testSse2Pmovmskb1d7, "PMOVMSKB 1D7 (sse2)");
    run(testPmovmskb3d7, "PMOVMSKB 3D7 (sse1)");
    run(testSse2Psubusb1d8, "PSUBUSB 1D8 (sse2)");
    run(testMmxPsubusb, "PSUBUSB 3d8 (mmx)");
    run(testSse2Psubusw1d9, "PSUBUSW 1D9 (sse2)");
    run(testMmxPsubusw, "PSUBUSB 3d9 (mmx)");
    run(testSse2Pminub1da, "PMINUB 1DA (sse2)");
    run(testPminub3da, "PMINUB 3DA (sse1)");
    run(testSse2Pand1db, "PAND 1DB (sse2)");
    run(testMmxPand, "PAND 3db (mmx)");
    run(testSse2Paddusb1dc, "PADDUSB 1DC (sse2)");
    run(testMmxPaddusb, "PADDUSB 3dc (mmx)");
    run(testSse2Paddusw1dd, "PADDUSW 1DD (sse2)");
    run(testMmxPaddusw, "PADDUSB 3dd (mmx)");
    run(testSse2Pmaxub1de, "PMAXUB 1DE (sse2)");
    run(testPmaxub3de, "PMAXUB 3DE (sse1)");
    run(testSse2Pandn1df, "PANDN 1DF (sse2)");
    run(testMmxPandn, "PANDN 3df (mmx)");

    run(testSse2Pavgb1e0, "PAVGB 1E0 (sse2)");
    run(testPavgb3e0, "PAVGB 3E0 (sse1)");
    run(testSse2Psraw1e1, "PSRAW 1E1 (sse2)");
    run(testMmxPsraw, "PSRAW 3E1 (mmx)");    
    run(testSse2Psrad1e2, "PSRAD 1E2 (sse2)");
    run(testMmxPsrad, "PSRAD 3E2 (mmx)");
    run(testSse2Pavgw1e3, "PAVGW 1E3 (sse2)");
    run(testPavgw3e3, "PAVGW 3E3 (sse1)");
    run(testSse2Pmulhuw1e4, "PMULHUW 1E4 (sse2)");
    run(testPmulhuw3e4, "PMULHUW 3E4 (sse1)");
    run(testSse2Pmulhw1e5, "PMULHW 1E5 (sse2)");
    run(testMmxPmulhw, "PMULHW 3e5 (mmx)");
    run(testSse2Cvttpd2dq1e6, "CVTTPD2DQ 1E6 (sse2)");
    run(testSse2Cvtpd2dq3e6, "CVTPD2DQ F2 3E6 (sse2)");
    run(testSse2Cvtdq2pd3e6, "CVTDQ2PD F3 3E6 (sse2)");
    run(testSse2Movntdq1e7, "MOVNTDQ 1E7 (sse2)");
    run(testMovntq3e7, "MOVNTQ 3E7 (sse1)");
    run(testSse2Psubsb1e8, "PSUBSB 1E8 (sse2)");
    run(testMmxPsubsb, "PSUBSB 3e8 (mmx)");
    run(testSse2Psubsw1e9, "PSUBSW 1E9 (sse2)");
    run(testMmxPsubsw, "PSUBSW 3e9 (mmx)"); 
    run(testSse2Pminsw1ea, "PMINSW 1EA (sse2)");
    run(testPminsw3ea, "PMINSW 3EA (sse1)");
    run(testSse2Por1eb, "POR 1EB (sse2)");
    run(testMmxPor, "POR 3eb (mmx)");
    run(testSse2Paddsb1ec, "PADDSB 1EC (sse2)");
    run(testMmxPaddsb, "PADDSB 3ec (mmx)");
    run(testSse2Paddsw1ed, "PADDSW 1ED (sse2)");
    run(testMmxPaddsw, "PADDSW 3ed (mmx)");
    run(testSse2Pmaxsw1ee, "PMAXSW 1EE (sse2)");
    run(testPmaxsw3ee, "PMAXSW 3EE (sse1)");
    run(testSse2Pxor1ef, "PXOR 1EF (sse2)");
    run(testMmxPxor, "PXOR 3ef (mmx)");

    run(testSse2Psllw1f1, "PSLLW 1F1 (sse2)");
    run(testMmxPsllw, "PSLLW 3f1 (mmx)");
    run(testSse2Pslld1f2, "PSLLD 1F2 (sse2)");
    run(testMmxPslld, "PSLLD 3f2 (mmx)");    
    run(testSse2Psllq1f3, "PSLLQ 1F3 (sse2)");
    run(testMmxPsllq, "PSLLQ 3f3 (mmx)");
    run(testSse2Pmuludq1f4, "PMULUDQ 1F4 (sse2)");
    run(testSse2Pmuludq3f4, "PMULUDQ 3F4 (sse2)");
    run(testSse2Pmaddwd1f5, "PMADDWD 1F5 (sse2)");
    run(testMmxPmaddwd, "PMULLW 3f5 (mmx)");
    run(testSse2Psadbw1f6, "PSADBW 1F6 (sse2)");
    run(testPsadbw3f6, "PSADBW 3F6 (sse1)");
    run(testSse2Maskmovdqu1f7, "MASKMOVDQU 1F7 (sse2)");
    run(testMaskmovq3f7, "MASKMOVQ 3F7 (sse1)");
    run(testSse2Psubb1f8, "PSUBB 1F8 (sse2)");
    run(testMmxPsubb, "PSUBB 3f8 (mmx)");
    run(testSse2Psubw1f9, "PSUBW 1F9 (sse2)");
    run(testMmxPsubw, "PSUBW 3f9 (mmx)");
    run(testSse2Psubd1fa, "PSUBD 1FA (sse2)");
    run(testMmxPsubd, "PSUBD 3fa (mmx)");                
    run(testSse2Psubq1fb, "PSUBQ 1FB (sse2)");
    run(testSse2Psubq3fb, "PSUBQ 3FB (sse2)");
    run(testSse2Paddb1fc, "PADDB 1FC (sse2)");
    run(testMmxPaddb, "PADDB 3fc (mmx)");
    run(testSse2Paddw1fd, "PADDW 1FD (sse2)");
    run(testMmxPaddw, "PADDW 3fd (mmx)");
    run(testSse2Paddd1fe, "PADDD 1FE (sse2)");
    run(testMmxPaddd, "PADDD 3fe (mmx)");                                  
            

    printf("%d tests FAILED\n", totalFails);
    SDL_Delay(5000);
    if (totalFails)
        return 1;
    return 0;
}

#endif
