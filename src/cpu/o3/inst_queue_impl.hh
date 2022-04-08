/*
 * Copyright (c) 2011-2014, 2017-2020 ARM Limited
 * Copyright (c) 2013 Advanced Micro Devices, Inc.
 * All rights reserved.
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Copyright (c) 2004-2006 The Regents of The University of Michigan
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __CPU_O3_INST_QUEUE_IMPL_HH__
#define __CPU_O3_INST_QUEUE_IMPL_HH__

#include <limits>
#include <vector>

#include "base/logging.hh"
#include "cpu/o3/fu_pool.hh"
#include "cpu/o3/inst_queue.hh"
#include "debug/DOM.hh"
#include "debug/DebugDOM.hh"
#include "debug/IQ.hh"
#include "debug/TaintTrackerDebug.hh"
#include "enums/OpClass.hh"
#include "params/DerivO3CPU.hh"
#include "sim/core.hh"

// clang complains about std::set being overloaded with Packet::set if
// we open up the entire namespace std
using std::list;

template <class Impl>
InstructionQueue<Impl>::FUCompletion::FUCompletion(const DynInstPtr &_inst,
    int fu_idx, InstructionQueue<Impl> *iq_ptr)
    : Event(Stat_Event_Pri, AutoDelete),
      inst(_inst), fuIdx(fu_idx), iqPtr(iq_ptr), freeFU(false)
{
}

template <class Impl>
void
InstructionQueue<Impl>::FUCompletion::process()
{
    iqPtr->processFUCompletion(inst, freeFU ? fuIdx : -1);
    inst = NULL;
}


template <class Impl>
const char *
InstructionQueue<Impl>::FUCompletion::description() const
{
    return "Functional unit completion";
}

template <class Impl>
InstructionQueue<Impl>::InstructionQueue(O3CPU *cpu_ptr, IEW *iew_ptr,
                                         const DerivO3CPUParams &params)
    : cpu(cpu_ptr),
      iewStage(iew_ptr),
      fuPool(params.fuPool),
      iqPolicy(params.smtIQPolicy),
      numThreads(params.numThreads),
      numEntries(params.numIQEntries),
      totalWidth(params.issueWidth),
      commitToIEWDelay(params.commitToIEWDelay),
      iqStats(cpu, totalWidth),
      iqIOStats(cpu)
{
    assert(fuPool);

    // Set the number of total physical registers
    // As the vector registers have two addressing modes, they are added twice
    numPhysRegs = params.numPhysIntRegs + params.numPhysFloatRegs +
                    params.numPhysVecRegs +
                    params.numPhysVecRegs * TheISA::NumVecElemPerVecReg +
                    params.numPhysVecPredRegs +
                    params.numPhysCCRegs;

    //Create an entry for each physical register within the
    //dependency graph.
    dependGraph.resize(numPhysRegs);

    // Resize the register scoreboard.
    regScoreboard.resize(numPhysRegs);

    //Initialize Mem Dependence Units
    for (ThreadID tid = 0; tid < Impl::MaxThreads; tid++) {
        memDepUnit[tid].init(params, tid, cpu_ptr);
        memDepUnit[tid].setIQ(this);
    }

    resetState();

    //Figure out resource sharing policy
    if (iqPolicy == SMTQueuePolicy::Dynamic) {
        //Set Max Entries to Total ROB Capacity
        for (ThreadID tid = 0; tid < numThreads; tid++) {
            maxEntries[tid] = numEntries;
        }

    } else if (iqPolicy == SMTQueuePolicy::Partitioned) {
        //@todo:make work if part_amt doesnt divide evenly.
        int part_amt = numEntries / numThreads;

        //Divide ROB up evenly
        for (ThreadID tid = 0; tid < numThreads; tid++) {
            maxEntries[tid] = part_amt;
        }

        DPRINTF(IQ, "IQ sharing policy set to Partitioned:"
                "%i entries per thread.\n",part_amt);
    } else if (iqPolicy == SMTQueuePolicy::Threshold) {
        double threshold =  (double)params.smtIQThreshold / 100;

        int thresholdIQ = (int)((double)threshold * numEntries);

        //Divide up by threshold amount
        for (ThreadID tid = 0; tid < numThreads; tid++) {
            maxEntries[tid] = thresholdIQ;
        }

        DPRINTF(IQ, "IQ sharing policy set to Threshold:"
                "%i entries per thread.\n",thresholdIQ);
   }
    for (ThreadID tid = numThreads; tid < Impl::MaxThreads; tid++) {
        maxEntries[tid] = 0;
    }
}

template <class Impl>
InstructionQueue<Impl>::~InstructionQueue()
{
    dependGraph.reset();
#ifdef DEBUG
    cprintf("Nodes traversed: %i, removed: %i\n",
            dependGraph.nodesTraversed, dependGraph.nodesRemoved);
#endif
}

template <class Impl>
std::string
InstructionQueue<Impl>::name() const
{
    return cpu->name() + ".iq";
}

template <class Impl>
InstructionQueue<Impl>::
IQStats::IQStats(O3CPU *cpu, const unsigned &total_width)
    : Stats::Group(cpu),
    ADD_STAT(instsAdded, UNIT_COUNT,
             "Number of instructions added to the IQ (excludes non-spec)"),
    ADD_STAT(nonSpecInstsAdded, UNIT_COUNT,
             "Number of non-speculative instructions added to the IQ"),
    ADD_STAT(instsIssued, UNIT_COUNT, "Number of instructions issued"),
    ADD_STAT(intInstsIssued, UNIT_COUNT,
             "Number of integer instructions issued"),
    ADD_STAT(floatInstsIssued, UNIT_COUNT,
             "Number of float instructions issued"),
    ADD_STAT(branchInstsIssued, UNIT_COUNT,
             "Number of branch instructions issued"),
    ADD_STAT(memInstsIssued, UNIT_COUNT,
             "Number of memory instructions issued"),
    ADD_STAT(miscInstsIssued, UNIT_COUNT,
             "Number of miscellaneous instructions issued"),
    ADD_STAT(squashedInstsIssued, UNIT_COUNT,
             "Number of squashed instructions issued"),
    ADD_STAT(squashedInstsExamined, UNIT_COUNT,
             "Number of squashed instructions iterated over during squash; "
             "mainly for profiling"),
    ADD_STAT(squashedOperandsExamined, UNIT_COUNT,
             "Number of squashed operands that are examined and possibly "
             "removed from graph"),
    ADD_STAT(squashedNonSpecRemoved, UNIT_COUNT,
             "Number of squashed non-spec instructions that were removed"),
    ADD_STAT(numIssuedDist, UNIT_COUNT, "Number of insts issued each cycle"),
    ADD_STAT(statFuBusy, UNIT_COUNT,
             "attempts to use FU when none available"),
    ADD_STAT(statIssuedInstType, UNIT_COUNT,
             "Number of instructions issued per FU type, per thread"),
    ADD_STAT(issueRate, UNIT_RATE(Stats::Units::Count, Stats::Units::Cycle),
             "Inst issue rate", instsIssued / cpu->baseStats.numCycles),
    ADD_STAT(fuBusy, UNIT_COUNT, "FU busy when requested"),
    ADD_STAT(fuBusyRate, UNIT_RATE(Stats::Units::Count, Stats::Units::Count),
             "FU busy rate (busy events/executed inst)"),
    ADD_STAT(delayedLoads, UNIT_COUNT, "Number of loads delayed"),
    ADD_STAT(squashedDelayedLoads, UNIT_COUNT,
             "Number of delayed loads that were squashed"),
    ADD_STAT(reissuedDelayedLoads, UNIT_COUNT,
             "Number of delayed loads that were reissued"),
    ADD_STAT(faultLoads, UNIT_COUNT,
             "Number of fault loads squashed in delayed queue")
{
    instsAdded
        .prereq(instsAdded);

    nonSpecInstsAdded
        .prereq(nonSpecInstsAdded);

    instsIssued
        .prereq(instsIssued);

    intInstsIssued
        .prereq(intInstsIssued);

    floatInstsIssued
        .prereq(floatInstsIssued);

    branchInstsIssued
        .prereq(branchInstsIssued);

    memInstsIssued
        .prereq(memInstsIssued);

    miscInstsIssued
        .prereq(miscInstsIssued);

    squashedInstsIssued
        .prereq(squashedInstsIssued);

    squashedInstsExamined
        .prereq(squashedInstsExamined);

    squashedOperandsExamined
        .prereq(squashedOperandsExamined);

    squashedNonSpecRemoved
        .prereq(squashedNonSpecRemoved);
/*
    queueResDist
        .init(Num_OpClasses, 0, 99, 2)
        .name(name() + ".IQ:residence:")
        .desc("cycles from dispatch to issue")
        .flags(total | pdf | cdf )
        ;
    for (int i = 0; i < Num_OpClasses; ++i) {
        queueResDist.subname(i, opClassStrings[i]);
    }
*/
    numIssuedDist
        .init(0,total_width,1)
        .flags(Stats::pdf)
        ;
/*
    dist_unissued
        .init(Num_OpClasses+2)
        .name(name() + ".unissued_cause")
        .desc("Reason ready instruction not issued")
        .flags(pdf | dist)
        ;
    for (int i=0; i < (Num_OpClasses + 2); ++i) {
        dist_unissued.subname(i, unissued_names[i]);
    }
*/
    statIssuedInstType
        .init(cpu->numThreads,Enums::Num_OpClass)
        .flags(Stats::total | Stats::pdf | Stats::dist)
        ;
    statIssuedInstType.ysubnames(Enums::OpClassStrings);

    //
    //  How long did instructions for a particular FU type wait prior to issue
    //
/*
    issueDelayDist
        .init(Num_OpClasses,0,99,2)
        .name(name() + ".")
        .desc("cycles from operands ready to issue")
        .flags(pdf | cdf)
        ;
    for (int i=0; i<Num_OpClasses; ++i) {
        std::stringstream subname;
        subname << opClassStrings[i] << "_delay";
        issueDelayDist.subname(i, subname.str());
    }
*/
    issueRate
        .flags(Stats::total)
        ;

    statFuBusy
        .init(Num_OpClasses)
        .flags(Stats::pdf | Stats::dist)
        ;
    for (int i=0; i < Num_OpClasses; ++i) {
        statFuBusy.subname(i, Enums::OpClassStrings[i]);
    }

    fuBusy
        .init(cpu->numThreads)
        .flags(Stats::total)
        ;

    fuBusyRate
        .flags(Stats::total)
        ;
    fuBusyRate = fuBusy / instsIssued;
}

template <class Impl>
InstructionQueue<Impl>::
IQIOStats::IQIOStats(Stats::Group *parent)
    : Stats::Group(parent),
    ADD_STAT(intInstQueueReads, UNIT_COUNT,
             "Number of integer instruction queue reads"),
    ADD_STAT(intInstQueueWrites, UNIT_COUNT,
             "Number of integer instruction queue writes"),
    ADD_STAT(intInstQueueWakeupAccesses, UNIT_COUNT,
             "Number of integer instruction queue wakeup accesses"),
    ADD_STAT(fpInstQueueReads, UNIT_COUNT,
             "Number of floating instruction queue reads"),
    ADD_STAT(fpInstQueueWrites, UNIT_COUNT,
             "Number of floating instruction queue writes"),
    ADD_STAT(fpInstQueueWakeupAccesses, UNIT_COUNT,
             "Number of floating instruction queue wakeup accesses"),
    ADD_STAT(vecInstQueueReads, UNIT_COUNT,
             "Number of vector instruction queue reads"),
    ADD_STAT(vecInstQueueWrites, UNIT_COUNT,
             "Number of vector instruction queue writes"),
    ADD_STAT(vecInstQueueWakeupAccesses, UNIT_COUNT,
             "Number of vector instruction queue wakeup accesses"),
    ADD_STAT(intAluAccesses, UNIT_COUNT, "Number of integer alu accesses"),
    ADD_STAT(fpAluAccesses, UNIT_COUNT,
             "Number of floating point alu accesses"),
    ADD_STAT(vecAluAccesses, UNIT_COUNT, "Number of vector alu accesses"),
    ADD_STAT(taintedBranchesInserted, UNIT_COUNT,
             "Number of branches delayed due to implicit taint protection"),
    ADD_STAT(taintedBranchesFreed, UNIT_COUNT,
             "Number of tainted Branches freed to resolve"),
    ADD_STAT(taintedBranchesSquashed, UNIT_COUNT,
             "Number of tainted Branches squashed before being freed")
{
    using namespace Stats;
    intInstQueueReads
        .flags(total);

    intInstQueueWrites
        .flags(total);

    intInstQueueWakeupAccesses
        .flags(total);

    fpInstQueueReads
        .flags(total);

    fpInstQueueWrites
        .flags(total);

    fpInstQueueWakeupAccesses
        .flags(total);

    vecInstQueueReads
        .flags(total);

    vecInstQueueWrites
        .flags(total);

    vecInstQueueWakeupAccesses
        .flags(total);

    intAluAccesses
        .flags(total);

    fpAluAccesses
        .flags(total);

    vecAluAccesses
        .flags(total);
}

template <class Impl>
void
InstructionQueue<Impl>::resetState()
{
    //Initialize thread IQ counts
    for (ThreadID tid = 0; tid < Impl::MaxThreads; tid++) {
        count[tid] = 0;
        instList[tid].clear();
    }

    // Initialize the number of free IQ entries.
    freeEntries = numEntries;

    // Note that in actuality, the registers corresponding to the logical
    // registers start off as ready.  However this doesn't matter for the
    // IQ as the instruction should have been correctly told if those
    // registers are ready in rename.  Thus it can all be initialized as
    // unready.
    for (int i = 0; i < numPhysRegs; ++i) {
        regScoreboard[i] = false;
    }

    for (ThreadID tid = 0; tid < Impl::MaxThreads; ++tid) {
        squashedSeqNum[tid] = 0;
    }

    for (int i = 0; i < Num_OpClasses; ++i) {
        while (!readyInsts[i].empty())
            readyInsts[i].pop();
        queueOnList[i] = false;
        readyIt[i] = listOrder.end();
    }
    nonSpecInsts.clear();
    listOrder.clear();
    delayedMemInsts.clear();
    deferredMemInsts.clear();
    blockedMemInsts.clear();
    retryMemInsts.clear();
    wbOutstanding = 0;
}

template <class Impl>
void
InstructionQueue<Impl>::setActiveThreads(list<ThreadID> *at_ptr)
{
    activeThreads = at_ptr;
}

template <class Impl>
void
InstructionQueue<Impl>::setIssueToExecuteQueue(TimeBuffer<IssueStruct> *i2e_ptr)
{
      issueToExecuteQueue = i2e_ptr;
}

template <class Impl>
void
InstructionQueue<Impl>::setTimeBuffer(TimeBuffer<TimeStruct> *tb_ptr)
{
    timeBuffer = tb_ptr;

    fromCommit = timeBuffer->getWire(-commitToIEWDelay);
}

template <class Impl>
bool
InstructionQueue<Impl>::isDrained() const
{
    bool drained = dependGraph.empty() &&
                   instsToExecute.empty() &&
                   wbOutstanding == 0;
    for (ThreadID tid = 0; tid < numThreads; ++tid)
        drained = drained && memDepUnit[tid].isDrained();

    return drained;
}

template <class Impl>
void
InstructionQueue<Impl>::drainSanityCheck() const
{
    assert(dependGraph.empty());
    assert(instsToExecute.empty());
    for (ThreadID tid = 0; tid < numThreads; ++tid)
        memDepUnit[tid].drainSanityCheck();
}

template <class Impl>
void
InstructionQueue<Impl>::takeOverFrom()
{
    resetState();
}

template <class Impl>
int
InstructionQueue<Impl>::entryAmount(ThreadID num_threads)
{
    if (iqPolicy == SMTQueuePolicy::Partitioned) {
        return numEntries / num_threads;
    } else {
        return 0;
    }
}


template <class Impl>
void
InstructionQueue<Impl>::resetEntries()
{
    if (iqPolicy != SMTQueuePolicy::Dynamic || numThreads > 1) {
        int active_threads = activeThreads->size();

        list<ThreadID>::iterator threads = activeThreads->begin();
        list<ThreadID>::iterator end = activeThreads->end();

        while (threads != end) {
            ThreadID tid = *threads++;

            if (iqPolicy == SMTQueuePolicy::Partitioned) {
                maxEntries[tid] = numEntries / active_threads;
            } else if (iqPolicy == SMTQueuePolicy::Threshold &&
                       active_threads == 1) {
                maxEntries[tid] = numEntries;
            }
        }
    }
}

template <class Impl>
unsigned
InstructionQueue<Impl>::numFreeEntries()
{
    return freeEntries;
}

template <class Impl>
unsigned
InstructionQueue<Impl>::numFreeEntries(ThreadID tid)
{
    return maxEntries[tid] - count[tid];
}

// Might want to do something more complex if it knows how many instructions
// will be issued this cycle.
template <class Impl>
bool
InstructionQueue<Impl>::isFull()
{
    if (freeEntries == 0) {
        return(true);
    } else {
        return(false);
    }
}

template <class Impl>
bool
InstructionQueue<Impl>::isFull(ThreadID tid)
{
    if (numFreeEntries(tid) == 0) {
        return(true);
    } else {
        return(false);
    }
}

template <class Impl>
bool
InstructionQueue<Impl>::hasReadyInsts()
{
    if (!listOrder.empty()) {
        return true;
    }

    for (int i = 0; i < Num_OpClasses; ++i) {
        if (!readyInsts[i].empty()) {
            return true;
        }
    }

    return false;
}

template <class Impl>
void
InstructionQueue<Impl>::insert(const DynInstPtr &new_inst)
{
    if (new_inst->isFloating()) {
        iqIOStats.fpInstQueueWrites++;
    } else if (new_inst->isVector()) {
        iqIOStats.vecInstQueueWrites++;
    } else {
        iqIOStats.intInstQueueWrites++;
    }
    // Make sure the instruction is valid
    assert(new_inst);

    DPRINTF(IQ, "Adding instruction [sn:%llu] PC %s to the IQ.\n",
            new_inst->seqNum, new_inst->pcState());

    assert(freeEntries != 0);

    instList[new_inst->threadNumber].push_back(new_inst);

    --freeEntries;

    new_inst->setInIQ();

    // Look through its source registers (physical regs), and mark any
    // dependencies.
    addToDependents(new_inst);

    // Have this instruction set itself as the producer of its destination
    // register(s).
    addToProducers(new_inst);

    assert(!(new_inst->isMemRef() && new_inst->isControl()));

    if (new_inst->isMemRef()) {
        memDepUnit[new_inst->threadNumber].insert(new_inst);
    } else {
        addIfReady(new_inst);
    }

    ++iqStats.instsAdded;

    count[new_inst->threadNumber]++;

    assert(freeEntries == (numEntries - countInsts()));

}

template <class Impl>
void
InstructionQueue<Impl>::insertNonSpec(const DynInstPtr &new_inst)
{
    // @todo: Clean up this code; can do it by setting inst as unable
    // to issue, then calling normal insert on the inst.
    if (new_inst->isFloating()) {
        iqIOStats.fpInstQueueWrites++;
    } else if (new_inst->isVector()) {
        iqIOStats.vecInstQueueWrites++;
    } else {
        iqIOStats.intInstQueueWrites++;
    }

    assert(new_inst);

    nonSpecInsts[new_inst->seqNum] = new_inst;

    DPRINTF(IQ, "Adding non-speculative instruction [sn:%llu] PC %s "
            "to the IQ.\n",
            new_inst->seqNum, new_inst->pcState());

    assert(freeEntries != 0);

    instList[new_inst->threadNumber].push_back(new_inst);

    --freeEntries;

    new_inst->setInIQ();

    // Have this instruction set itself as the producer of its destination
    // register(s).
    addToProducers(new_inst);

    // If it's a memory instruction, add it to the memory dependency
    // unit.
    if (new_inst->isMemRef()) {
        memDepUnit[new_inst->threadNumber].insertNonSpec(new_inst);
    }

    ++iqStats.nonSpecInstsAdded;

    count[new_inst->threadNumber]++;

    assert(freeEntries == (numEntries - countInsts()));
}

template <class Impl>
void
InstructionQueue<Impl>::insertBarrier(const DynInstPtr &barr_inst)
{
    memDepUnit[barr_inst->threadNumber].insertBarrier(barr_inst);

    insertNonSpec(barr_inst);
}

template <class Impl>
typename Impl::DynInstPtr
InstructionQueue<Impl>::getInstToExecute()
{
    assert(!instsToExecute.empty());
    DynInstPtr inst = std::move(instsToExecute.front());
    instsToExecute.pop_front();
    if (inst->isFloating()) {
        iqIOStats.fpInstQueueReads++;
    } else if (inst->isVector()) {
        iqIOStats.vecInstQueueReads++;
    } else {
        iqIOStats.intInstQueueReads++;
    }
    return inst;
}

template <class Impl>
void
InstructionQueue<Impl>::cleanPredictables()
{
    for (int i = 0; i < instsPredictable.size(); i++) {
        DynInstPtr inst = instsPredictable.at(i);
        if (inst->isCommitted()) {
            instsPredictable.erase(instsPredictable.begin() + i);
            i--;
        } else if (inst->isSquashed()) {
            instsPredictable.erase(instsPredictable.begin() + i);
            i--;
        } else if (inst->hasRequest()) {
            instsPredictable.erase(instsPredictable.begin() + i);
            i--;
        }
    }
}


template <class Impl>
typename Impl::DynInstPtr
InstructionQueue<Impl>::getPredictable()
{
    assert(!instsPredictable.empty());
    DynInstPtr inst = std::move(instsPredictable.front());
    instsPredictable.erase(instsPredictable.begin());
    assert(!inst->isSquashed());
    assert(inst->isLoad());
    DPRINTF(IQ, "Returning [sn:%llu] for prediction\n",
        inst->seqNum);
    return inst;
}

template <class Impl>
bool
InstructionQueue<Impl>::hasPredictable()
{
    return !instsPredictable.empty();
}

template <class Impl>
void
InstructionQueue<Impl>::addToOrderList(OpClass op_class)
{
    assert(!readyInsts[op_class].empty());

    ListOrderEntry queue_entry;

    queue_entry.queueType = op_class;

    queue_entry.oldestInst = readyInsts[op_class].top()->seqNum;

    ListOrderIt list_it = listOrder.begin();
    ListOrderIt list_end_it = listOrder.end();

    while (list_it != list_end_it) {
        if ((*list_it).oldestInst > queue_entry.oldestInst) {
            break;
        }

        list_it++;
    }

    readyIt[op_class] = listOrder.insert(list_it, queue_entry);
    queueOnList[op_class] = true;
}

template <class Impl>
void
InstructionQueue<Impl>::moveToYoungerInst(ListOrderIt list_order_it)
{
    // Get iterator of next item on the list
    // Delete the original iterator
    // Determine if the next item is either the end of the list or younger
    // than the new instruction.  If so, then add in a new iterator right here.
    // If not, then move along.
    ListOrderEntry queue_entry;
    OpClass op_class = (*list_order_it).queueType;
    ListOrderIt next_it = list_order_it;

    ++next_it;

    queue_entry.queueType = op_class;
    queue_entry.oldestInst = readyInsts[op_class].top()->seqNum;

    while (next_it != listOrder.end() &&
           (*next_it).oldestInst < queue_entry.oldestInst) {
        ++next_it;
    }

    readyIt[op_class] = listOrder.insert(next_it, queue_entry);
}

template <class Impl>
void
InstructionQueue<Impl>::processFUCompletion(const DynInstPtr &inst, int fu_idx)
{
    DPRINTF(IQ, "Processing FU completion [sn:%llu]\n", inst->seqNum);
    assert(!cpu->switchedOut());
    // The CPU could have been sleeping until this op completed (*extremely*
    // long latency op).  Wake it if it was.  This may be overkill.
   --wbOutstanding;
    iewStage->wakeCPU();

    if (fu_idx > -1)
        fuPool->freeUnitNextCycle(fu_idx);

    // @todo: Ensure that these FU Completions happen at the beginning
    // of a cycle, otherwise they could add too many instructions to
    // the queue.
    issueToExecuteQueue->access(-1)->size++;
    instsToExecute.push_back(inst);
}

// @todo: Figure out a better way to remove the squashed items from the
// lists.  Checking the top item of each list to see if it's squashed
// wastes time and forces jumps.
template <class Impl>
void
InstructionQueue<Impl>::scheduleReadyInsts()
{
    DPRINTF(IQ, "Attempting to schedule ready instructions from "
            "the IQ.\n");

    IssueStruct *i2e_info = issueToExecuteQueue->access(0);

    DynInstPtr mem_inst;
    DPRINTF(DebugDOM, "num delayed mems: %d\n",
        delayedMemInsts.size());

    while ((mem_inst = std::move(getDeferredMemInstToExecute()))) {
        addReadyMemInst(mem_inst);
    }

    // See if any cache blocked instructions are able to be executed
    while ((mem_inst = std::move(getBlockedMemInstToExecute()))) {
        addReadyMemInst(mem_inst);
    }

    // Have iterator to head of the list
    // While I haven't exceeded bandwidth or reached the end of the list,
    // Try to get a FU that can do what this op needs.
    // If successful, change the oldestInst to the new top of the list, put
    // the queue in the proper place in the list.
    // Increment the iterator.
    // This will avoid trying to schedule a certain op class if there are no
    // FUs that handle it.
    int total_issued = 0;
    ListOrderIt order_it = listOrder.begin();
    ListOrderIt order_end_it = listOrder.end();

    while (total_issued < totalWidth && order_it != order_end_it) {
        OpClass op_class = (*order_it).queueType;

        assert(!readyInsts[op_class].empty());

        DynInstPtr issuing_inst = readyInsts[op_class].top();

        if (issuing_inst->isFloating()) {
            iqIOStats.fpInstQueueReads++;
        } else if (issuing_inst->isVector()) {
            iqIOStats.vecInstQueueReads++;
        } else {
            iqIOStats.intInstQueueReads++;
        }

        assert(issuing_inst->seqNum == (*order_it).oldestInst);

        if (issuing_inst->isSquashed()) {
            readyInsts[op_class].pop();

            if (!readyInsts[op_class].empty()) {
                moveToYoungerInst(order_it);
            } else {
                readyIt[op_class] = listOrder.end();
                queueOnList[op_class] = false;
            }

            listOrder.erase(order_it++);

            ++iqStats.squashedInstsIssued;

            continue;
        }

        int idx = FUPool::NoCapableFU;
        Cycles op_latency = Cycles(1);
        ThreadID tid = issuing_inst->threadNumber;

        if (op_class != No_OpClass) {
            idx = fuPool->getUnit(op_class);
            if (issuing_inst->isFloating()) {
                iqIOStats.fpAluAccesses++;
            } else if (issuing_inst->isVector()) {
                iqIOStats.vecAluAccesses++;
            } else {
                iqIOStats.intAluAccesses++;
            }
            if (idx > FUPool::NoFreeFU) {
                op_latency = fuPool->getOpLatency(op_class);
            }
        }

        // If we have an instruction that doesn't require a FU, or a
        // valid FU, then schedule for execution.
        if (idx != FUPool::NoFreeFU) {
            if (op_latency == Cycles(1)) {
                i2e_info->size++;
                instsToExecute.push_back(issuing_inst);

                // Add the FU onto the list of FU's to be freed next
                // cycle if we used one.
                if (idx >= 0)
                    fuPool->freeUnitNextCycle(idx);
            } else {
                bool pipelined = fuPool->isPipelined(op_class);
                // Generate completion event for the FU
                ++wbOutstanding;
                FUCompletion *execution = new FUCompletion(issuing_inst,
                                                           idx, this);

                cpu->schedule(execution,
                              cpu->clockEdge(Cycles(op_latency - 1)));

                if (!pipelined) {
                    // If FU isn't pipelined, then it must be freed
                    // upon the execution completing.
                    execution->setFreeFU();
                } else {
                    // Add the FU onto the list of FU's to be freed next cycle.
                    fuPool->freeUnitNextCycle(idx);
                }
            }

            DPRINTF(IQ, "Thread %i: Issuing instruction PC %s "
                    "[sn:%llu]\n",
                    tid, issuing_inst->pcState(),
                    issuing_inst->seqNum);

            readyInsts[op_class].pop();

            if (!readyInsts[op_class].empty()) {
                moveToYoungerInst(order_it);
            } else {
                readyIt[op_class] = listOrder.end();
                queueOnList[op_class] = false;
            }

            issuing_inst->setIssued();
            ++total_issued;

#if TRACING_ON
            issuing_inst->issueTick = curTick() - issuing_inst->fetchTick;
#endif

            if (!issuing_inst->isMemRef()) {
                // Memory instructions can not be freed from the IQ until they
                // complete.
                ++freeEntries;
                count[tid]--;
                issuing_inst->clearInIQ();
            } else {
                memDepUnit[tid].issue(issuing_inst);
            }

            listOrder.erase(order_it++);
            iqStats.statIssuedInstType[tid][op_class]++;
        } else {
            iqStats.statFuBusy[op_class]++;
            iqStats.fuBusy[tid]++;
            ++order_it;
        }
    }

    iqStats.numIssuedDist.sample(total_issued);
    iqStats.instsIssued+= total_issued;

    // If we issued any instructions, tell the CPU we had activity.
    // @todo If the way deferred memory instructions are handeled due to
    // translation changes then the deferredMemInsts condition should be removed
    // from the code below.
    if (total_issued || !retryMemInsts.empty() || !deferredMemInsts.empty()) {
        cpu->activityThisCycle();
    } else {
        DPRINTF(IQ, "Not able to schedule any instructions.\n");
    }
}

template <class Impl>
void
InstructionQueue<Impl>::scheduleNonSpec(const InstSeqNum &inst)
{
    DPRINTF(IQ, "Marking nonspeculative instruction [sn:%llu] as ready "
            "to execute.\n", inst);

    NonSpecMapIt inst_it = nonSpecInsts.find(inst);

    assert(inst_it != nonSpecInsts.end());

    ThreadID tid = (*inst_it).second->threadNumber;

    (*inst_it).second->setAtCommit();

    (*inst_it).second->setCanIssue();

    if (!(*inst_it).second->isMemRef()) {
        addIfReady((*inst_it).second);
    } else {
        memDepUnit[tid].nonSpecInstReady((*inst_it).second);
    }

    (*inst_it).second = NULL;

    nonSpecInsts.erase(inst_it);
}

template <class Impl>
void
InstructionQueue<Impl>::commit(const InstSeqNum &inst, ThreadID tid)
{
    DPRINTF(IQ, "[tid:%i] Committing instructions older than [sn:%llu]\n",
            tid,inst);

    ListIt iq_it = instList[tid].begin();

    while (iq_it != instList[tid].end() &&
           (*iq_it)->seqNum <= inst) {
        ++iq_it;
        instList[tid].pop_front();
    }

    assert(freeEntries == (numEntries - countInsts()));
}

template <class Impl>
int
InstructionQueue<Impl>::wakeDependents(const DynInstPtr &completed_inst)
{
    int dependents = 0;

    // The instruction queue here takes care of both floating and int ops
    if (completed_inst->isFloating()) {
        iqIOStats.fpInstQueueWakeupAccesses++;
    } else if (completed_inst->isVector()) {
        iqIOStats.vecInstQueueWakeupAccesses++;
    } else {
        iqIOStats.intInstQueueWakeupAccesses++;
    }

    DPRINTF(IQ, "Waking dependents of completed instruction.\n");

    assert(!completed_inst->isSquashed());

    // Tell the memory dependence unit to wake any dependents on this
    // instruction if it is a memory instruction.  Also complete the memory
    // instruction at this point since we know it executed without issues.
    ThreadID tid = completed_inst->threadNumber;
    if (completed_inst->isMemRef()) {
        memDepUnit[tid].completeInst(completed_inst);

        DPRINTF(IQ, "Completing mem instruction PC: %s [sn:%llu]\n",
            completed_inst->pcState(), completed_inst->seqNum);

        ++freeEntries;
        completed_inst->memOpDone(true);
        count[tid]--;
    } else if (completed_inst->isReadBarrier() ||
               completed_inst->isWriteBarrier()) {
        // Completes a non mem ref barrier
        memDepUnit[tid].completeInst(completed_inst);
    }

    for (int dest_reg_idx = 0;
         dest_reg_idx < completed_inst->numDestRegs();
         dest_reg_idx++)
    {
        PhysRegIdPtr dest_reg =
            completed_inst->regs.renamedDestIdx(dest_reg_idx);

        // Special case of uniq or control registers.  They are not
        // handled by the IQ and thus have no dependency graph entry.
        if (dest_reg->isFixedMapping()) {
            DPRINTF(IQ, "Reg %d [%s] is part of a fix mapping, skipping\n",
                    dest_reg->index(), dest_reg->className());
            continue;
        }

        // Avoid waking up dependents if the register is pinned
        dest_reg->decrNumPinnedWritesToComplete();
        if (dest_reg->isPinned())
            completed_inst->setPinnedRegsWritten();

        if (dest_reg->getNumPinnedWritesToComplete() != 0) {
            DPRINTF(IQ, "Reg %d [%s] is pinned, skipping\n",
                    dest_reg->index(), dest_reg->className());
            continue;
        }

        DPRINTF(IQ, "Waking any dependents on register %i (%s).\n",
                dest_reg->index(),
                dest_reg->className());

        //Go through the dependency chain, marking the registers as
        //ready within the waiting instructions.
        DynInstPtr dep_inst = dependGraph.pop(dest_reg->flatIndex());

        while (dep_inst) {
            DPRINTF(IQ, "Waking up a dependent instruction, [sn:%llu] "
                    "PC %s.\n", dep_inst->seqNum, dep_inst->pcState());

            // Might want to give more information to the instruction
            // so that it knows which of its source registers is
            // ready.  However that would mean that the dependency
            // graph entries would need to hold the src_reg_idx.
            dep_inst->markSrcRegReady();

            addIfReady(dep_inst);

            dep_inst = dependGraph.pop(dest_reg->flatIndex());

            ++dependents;
        }

        // Reset the head node now that all of its dependents have
        // been woken up.
        assert(dependGraph.empty(dest_reg->flatIndex()));
        dependGraph.clearInst(dest_reg->flatIndex());

        // Mark the scoreboard as having that register ready.
        regScoreboard[dest_reg->flatIndex()] = true;
    }
    return dependents;
}

template <class Impl>
void
InstructionQueue<Impl>::addReadyMemInst(const DynInstPtr &ready_inst)
{
    OpClass op_class = ready_inst->opClass();

    readyInsts[op_class].push(ready_inst);

    // Will need to reorder the list if either a queue is not on the list,
    // or it has an older instruction than last time.
    if (!queueOnList[op_class]) {
        addToOrderList(op_class);
    } else if (readyInsts[op_class].top()->seqNum  <
               (*readyIt[op_class]).oldestInst) {
        listOrder.erase(readyIt[op_class]);
        addToOrderList(op_class);
    }

    DPRINTF(IQ, "Instruction is ready to issue, putting it onto "
            "the ready list, PC %s opclass:%i [sn:%llu].\n",
            ready_inst->pcState(), op_class, ready_inst->seqNum);
}

template <class Impl>
void
InstructionQueue<Impl>::rescheduleMemInst(const DynInstPtr &resched_inst)
{
    DPRINTF(IQ, "Rescheduling mem inst [sn:%llu]\n", resched_inst->seqNum);

    // Reset DTB translation state
    resched_inst->translationStarted(false);
    resched_inst->translationCompleted(false);

    resched_inst->clearCanIssue();
    memDepUnit[resched_inst->threadNumber].reschedule(resched_inst);
}


template <class Impl>
void
InstructionQueue<Impl>::delayMemInst(const DynInstPtr &delayed_inst)
{
    DPRINTF(DOM, "Delaying mem inst [sn:%llu]\n", delayed_inst->seqNum);
    delayedMemInsts.push_back(delayed_inst);
    ++iqStats.delayedLoads;
}


template <class Impl>
void
InstructionQueue<Impl>::replayMemInst(const DynInstPtr &replay_inst)
{
    memDepUnit[replay_inst->threadNumber].replay();
}

template <class Impl>
void
InstructionQueue<Impl>::deferMemInst(const DynInstPtr &deferred_inst)
{
    deferredMemInsts.push_back(deferred_inst);
}

template <class Impl>
void
InstructionQueue<Impl>::blockMemInst(const DynInstPtr &blocked_inst)
{
    blocked_inst->clearIssued();
    blocked_inst->clearCanIssue();
    blockedMemInsts.push_back(blocked_inst);
}

template <class Impl>
void
InstructionQueue<Impl>::cacheUnblocked()
{
    retryMemInsts.splice(retryMemInsts.end(), blockedMemInsts);
    // Get the CPU ticking again
    cpu->wakeCPU();
}

template <class Impl>
typename Impl::DynInstPtr
InstructionQueue<Impl>::getDeferredMemInstToExecute()
{
    for (ListIt it = deferredMemInsts.begin(); it != deferredMemInsts.end();
         ++it) {
        if ((*it)->translationCompleted() || (*it)->isSquashed()) {
            DynInstPtr mem_inst = std::move(*it);
            deferredMemInsts.erase(it);
            return mem_inst;
        }
    }
    return nullptr;
}

template <class Impl>
typename Impl::DynInstPtr
InstructionQueue<Impl>::getBlockedMemInstToExecute()
{
    if (retryMemInsts.empty()) {
        return nullptr;
    } else {
        DynInstPtr mem_inst = std::move(retryMemInsts.front());
        retryMemInsts.erase(retryMemInsts.begin());
        return mem_inst;
    }
}

template <class Impl>
void
InstructionQueue<Impl>::completeSafeLoads()
{
    for (int i = 0; i < delayedMemInsts.size(); i++) {
        DynInstPtr inst = delayedMemInsts.at(i);
        if (inst->isCommitted()) {
            assert(!inst->underShadow());
            DPRINTF(DOM, "Removed committed load in delay queue [sn:%llu]\n",
                    inst->seqNum);
            delayedMemInsts.erase(delayedMemInsts.begin() + i);
            if (inst->hasResp() &&
                inst->savedReq != nullptr &&
                inst->savedReq->isSplit())
                    inst->delResp();
            i--;
        } else if (inst->isSquashed()) {
            DPRINTF(DOM, "Squashed a load in delay queue [sn:%llu]\n",
                    (*(delayedMemInsts.begin()+i))->seqNum);
            delayedMemInsts.erase(delayedMemInsts.begin() + i);
            if (inst->hasResp() &&
                inst->savedReq != nullptr &&
                inst->savedReq->isSplit())
                    inst->delResp();
            i--;
        } else if (inst->savedReq->isPartialFault()) {
            DPRINTF(DOM, "Squashed a partial fault,"
                    " ROB will handle inst [sn:%llu]\n",
                    delayedMemInsts.at(i)->seqNum);
            delayedMemInsts.erase(delayedMemInsts.begin() + i);
            if (inst->hasResp() &&
                inst->savedReq != nullptr &&
                inst->savedReq->isSplit())
                    inst->delResp();
            i--;
            ++iqStats.faultLoads;
        } else if (!inst->underShadow()) {
            DPRINTF(DebugDOM, "Checking [sn:%llu] for delayed completion\n",
                    inst->seqNum);
            if (inst->hasResp()) {
                DPRINTF(DOM, "Completing [sn:%llu] delayed\n",
                        inst->seqNum);
                iewStage->ldstQueue.completeInst(inst);
                delayedMemInsts.erase(delayedMemInsts.begin() + i);
                if (inst->savedReq != nullptr &&
                    inst->savedReq->isSplit()) inst->delResp();
                i--;
            } else if (inst->shouldForward &&
                          (inst->hasStoreData || inst->hasPredData)) {
                iewStage->ldstQueue.completeInst(inst);
                delayedMemInsts.erase(delayedMemInsts.begin() + i);
                i--;
            }
        }
    }
}

template <class Impl>
void
InstructionQueue<Impl>::violation(const DynInstPtr &store,
                                  const DynInstPtr &faulting_load)
{
    iqIOStats.intInstQueueWrites++;
    memDepUnit[store->threadNumber].violation(store, faulting_load);
}

template <class Impl>
void
InstructionQueue<Impl>::squash(ThreadID tid)
{
    DPRINTF(IQ, "[tid:%i] Starting to squash instructions in "
            "the IQ.\n", tid);

    // Read instruction sequence number of last instruction out of the
    // time buffer.
    squashedSeqNum[tid] = fromCommit->commitInfo[tid].doneSeqNum;

    doSquash(tid);

    // Also tell the memory dependence unit to squash.
    memDepUnit[tid].squash(squashedSeqNum[tid], tid);
}

template<class Impl>
void
InstructionQueue<Impl>::freeTaints()
{
    freeTaintedBranches();
    memDepUnit[0].freeTaints();
}

template <class Impl>
void
InstructionQueue<Impl>::doSquash(ThreadID tid)
{
    // Start at the tail.
    ListIt squash_it = instList[tid].end();
    --squash_it;

    DPRINTF(IQ, "[tid:%i] Squashing until sequence number %i!\n",
            tid, squashedSeqNum[tid]);

    // Squash any instructions younger than the squashed sequence number
    // given.
    while (squash_it != instList[tid].end() &&
           (*squash_it)->seqNum > squashedSeqNum[tid]) {

        DynInstPtr squashed_inst = (*squash_it);
        if (squashed_inst->isFloating()) {
            iqIOStats.fpInstQueueWrites++;
        } else if (squashed_inst->isVector()) {
            iqIOStats.vecInstQueueWrites++;
        } else {
            iqIOStats.intInstQueueWrites++;
        }

        // Only handle the instruction if it actually is in the IQ and
        // hasn't already been squashed in the IQ.
        if (squashed_inst->threadNumber != tid ||
            squashed_inst->isSquashedInIQ()) {
            --squash_it;
            continue;
        }

        if (!squashed_inst->isIssued() ||
            (squashed_inst->isMemRef() &&
             !squashed_inst->memOpDone())) {

            DPRINTF(IQ, "[tid:%i] Instruction [sn:%llu] PC %s squashed.\n",
                    tid, squashed_inst->seqNum, squashed_inst->pcState());

            bool is_acq_rel = squashed_inst->isFullMemBarrier() &&
                         (squashed_inst->isLoad() ||
                          (squashed_inst->isStore() &&
                             !squashed_inst->isStoreConditional()));

            // Remove the instruction from the dependency list.
            if (is_acq_rel ||
                (!squashed_inst->isNonSpeculative() &&
                 !squashed_inst->isStoreConditional() &&
                 !squashed_inst->isAtomic() &&
                 !squashed_inst->isReadBarrier() &&
                 !squashed_inst->isWriteBarrier())) {

                for (int src_reg_idx = 0;
                     src_reg_idx < squashed_inst->numSrcRegs();
                     src_reg_idx++)
                {
                    PhysRegIdPtr src_reg =
                        squashed_inst->regs.renamedSrcIdx(src_reg_idx);

                    // Only remove it from the dependency graph if it
                    // was placed there in the first place.

                    // Instead of doing a linked list traversal, we
                    // can just remove these squashed instructions
                    // either at issue time, or when the register is
                    // overwritten.  The only downside to this is it
                    // leaves more room for error.

                    if (!squashed_inst->regs.readySrcIdx(src_reg_idx) &&
                        !src_reg->isFixedMapping()) {
                        dependGraph.remove(src_reg->flatIndex(),
                                           squashed_inst);
                    }

                    ++iqStats.squashedOperandsExamined;
                }

            } else if (!squashed_inst->isStoreConditional() ||
                       !squashed_inst->isCompleted()) {
                NonSpecMapIt ns_inst_it =
                    nonSpecInsts.find(squashed_inst->seqNum);

                // we remove non-speculative instructions from
                // nonSpecInsts already when they are ready, and so we
                // cannot always expect to find them
                if (ns_inst_it == nonSpecInsts.end()) {
                    // loads that became ready but stalled on a
                    // blocked cache are alreayd removed from
                    // nonSpecInsts, and have not faulted
                    assert(squashed_inst->getFault() != NoFault ||
                           squashed_inst->isMemRef());
                } else {

                    (*ns_inst_it).second = NULL;

                    nonSpecInsts.erase(ns_inst_it);

                    ++iqStats.squashedNonSpecRemoved;
                }
            }

            // Might want to also clear out the head of the dependency graph.

            // Mark it as squashed within the IQ.
            squashed_inst->setSquashedInIQ();

            // @todo: Remove this hack where several statuses are set so the
            // inst will flow through the rest of the pipeline.
            squashed_inst->setIssued();
            squashed_inst->setCanCommit();
            squashed_inst->clearInIQ();

            //Update Thread IQ Count
            count[squashed_inst->threadNumber]--;

            ++freeEntries;
        }

        // IQ clears out the heads of the dependency graph only when
        // instructions reach writeback stage. If an instruction is squashed
        // before writeback stage, its head of dependency graph would not be
        // cleared out; it holds the instruction's DynInstPtr. This prevents
        // freeing the squashed instruction's DynInst.
        // Thus, we need to manually clear out the squashed instructions' heads
        // of dependency graph.
        for (int dest_reg_idx = 0;
             dest_reg_idx < squashed_inst->numDestRegs();
             dest_reg_idx++)
        {
            PhysRegIdPtr dest_reg =
                squashed_inst->regs.renamedDestIdx(dest_reg_idx);
            if (dest_reg->isFixedMapping()){
                continue;
            }
            assert(dependGraph.empty(dest_reg->flatIndex()));
            dependGraph.clearInst(dest_reg->flatIndex());
        }
        if (cpu->AP &&
           (*squash_it)->isLoad())
            removeFromPredictable(*squash_it);
        instList[tid].erase(squash_it--);
        ++iqStats.squashedInstsExamined;
    }
}

template <class Impl>
bool
InstructionQueue<Impl>::addToDependents(const DynInstPtr &new_inst)
{
    // Loop through the instruction's source registers, adding
    // them to the dependency list if they are not ready.
    int8_t total_src_regs = new_inst->numSrcRegs();
    bool return_val = false;

    for (int src_reg_idx = 0;
         src_reg_idx < total_src_regs;
         src_reg_idx++)
    {
        // Only add it to the dependency graph if it's not ready.
        if (!new_inst->regs.readySrcIdx(src_reg_idx)) {
            PhysRegIdPtr src_reg = new_inst->regs.renamedSrcIdx(src_reg_idx);

            // Check the IQ's scoreboard to make sure the register
            // hasn't become ready while the instruction was in flight
            // between stages.  Only if it really isn't ready should
            // it be added to the dependency graph.
            if (src_reg->isFixedMapping()) {
                continue;
            } else if (!regScoreboard[src_reg->flatIndex()]) {
                DPRINTF(IQ, "Instruction PC %s has src reg %i (%s) that "
                        "is being added to the dependency chain.\n",
                        new_inst->pcState(), src_reg->index(),
                        src_reg->className());

                dependGraph.insert(src_reg->flatIndex(), new_inst);

                // Change the return value to indicate that something
                // was added to the dependency graph.
                return_val = true;
            } else {
                DPRINTF(IQ, "Instruction PC %s has src reg %i (%s) that "
                        "became ready before it reached the IQ.\n",
                        new_inst->pcState(), src_reg->index(),
                        src_reg->className());
                // Mark a register ready within the instruction.
                new_inst->markSrcRegReady(src_reg_idx);
            }
        }
    }

    return return_val;
}

template <class Impl>
void
InstructionQueue<Impl>::addToProducers(const DynInstPtr &new_inst)
{
    // Nothing really needs to be marked when an instruction becomes
    // the producer of a register's value, but for convenience a ptr
    // to the producing instruction will be placed in the head node of
    // the dependency links.
    int8_t total_dest_regs = new_inst->numDestRegs();

    for (int dest_reg_idx = 0;
         dest_reg_idx < total_dest_regs;
         dest_reg_idx++)
    {
        PhysRegIdPtr dest_reg = new_inst->regs.renamedDestIdx(dest_reg_idx);

        // Some registers have fixed mapping, and there is no need to track
        // dependencies as these instructions must be executed at commit.
        if (dest_reg->isFixedMapping()) {
            continue;
        }

        if (!dependGraph.empty(dest_reg->flatIndex())) {
            dependGraph.dump();
            panic("Dependency graph %i (%s) (flat: %i) not empty!",
                  dest_reg->index(), dest_reg->className(),
                  dest_reg->flatIndex());
        }

        dependGraph.setInst(dest_reg->flatIndex(), new_inst);

        // Mark the scoreboard to say it's not yet ready.
        regScoreboard[dest_reg->flatIndex()] = false;
    }
}

template <class Impl>
void
InstructionQueue<Impl>::addIfReady(const DynInstPtr &inst)
{
    // If the instruction now has all of its source registers
    // available, then add it to the list of ready instructions.
    if (inst->readyToIssue()) {

        if (cpu->STT)
            propagateTaints(inst, inst->threadNumber);

        //Add the instruction to the proper ready list.
        if (inst->isMemRef()) {

            DPRINTF(IQ, "Checking if memory instruction can issue.\n");

            // Message to the mem dependence unit that this instruction has
            // its registers ready.
            memDepUnit[inst->threadNumber].regsReady(inst);

            return;
        }

        if (cpu->STT && inst->isControl()) {
            if (cpu->taintTracker.hasTaintedSrc(inst) &&
                inst->staticInst->getName() != "wrip") {
                addToTaintedBranches(inst);
                return;
            }
        }

        OpClass op_class = inst->opClass();

        DPRINTF(IQ, "Instruction is ready to issue, putting it onto "
                "the ready list, PC %s opclass:%i [sn:%llu].\n",
                inst->pcState(), op_class, inst->seqNum);

        readyInsts[op_class].push(inst);

        // Will need to reorder the list if either a queue is not on the list,
        // or it has an older instruction than last time.
        if (!queueOnList[op_class]) {
            addToOrderList(op_class);
        } else if (readyInsts[op_class].top()->seqNum  <
                   (*readyIt[op_class]).oldestInst) {
            listOrder.erase(readyIt[op_class]);
            addToOrderList(op_class);
        }
    }
}

template <class Impl>
void
InstructionQueue<Impl>::addToTaintedBranches(const DynInstPtr &inst)
{
    assert(inst->isControl());
    ++iqIOStats.taintedBranchesInserted;
    taintedBranches.push_back(inst);
}

template <class Impl>
void
InstructionQueue<Impl>::freeTaintedBranches()
{
    DPRINTF(IQ, "Freeing tainted branches, current size: %d\n",
            taintedBranches.size());
    for (int i = 0; i < taintedBranches.size(); i++) {
        DynInstPtr inst = taintedBranches.at(i);
        bool tainted = false;
        for (int j = 0; j < inst->numSrcRegs(); j++) {
            PhysRegIdPtr src_reg = inst->regs.renamedSrcIdx(j);
            if (cpu->taintTracker.isTainted(src_reg)) {
                tainted = true;
            }
        }
        if (tainted && !inst->isSquashed())  {
            continue;
        }

        DPRINTF(IQ, "Freeing untainted branch "
                "[sn:%llu] from taintedBranches, squashed: %d\n",
                inst->seqNum,
                inst->isSquashed());

        taintedBranches.erase(taintedBranches.begin() + i);
        i--;

        if (inst->isSquashed()) {
            ++iqIOStats.taintedBranchesSquashed;
            continue;
        }

        assert(!inst->isCommitted());

        ++iqIOStats.taintedBranchesFreed;
        OpClass op_class = inst->opClass();

        DPRINTF(IQ, "Instruction is ready to issue, putting it onto "
                "the ready list, PC %s opclass:%i [sn:%llu].\n",
                inst->pcState(), op_class, inst->seqNum);

        readyInsts[op_class].push(inst);

        // Will need to reorder the list if either a queue is not on the list,
        // or it has an older instruction than last time.
        if (!queueOnList[op_class]) {
            addToOrderList(op_class);
        } else if (readyInsts[op_class].top()->seqNum  <
                   (*readyIt[op_class]).oldestInst) {
            listOrder.erase(readyIt[op_class]);
            addToOrderList(op_class);
        }
    }
}

template <class Impl>
void
InstructionQueue<Impl>::pruneTaintedBranches()
{
    for (int i = 0; i < taintedBranches.size(); i++) {
        DynInstPtr inst = taintedBranches.at(i);
        if (inst->isSquashed()) {
            ++iqIOStats.taintedBranchesSquashed;
            DPRINTF(IQ, "Removing squashed branch from "
            "taintedBranches: [sn:%llu]\n", inst->seqNum);
            taintedBranches.erase(taintedBranches.begin() + i);
            i--;
        }
    }
}

template <class Impl>
int
InstructionQueue<Impl>::countInsts()
{
    return numEntries - freeEntries;
}

template <class Impl>
void
InstructionQueue<Impl>::dumpLists()
{
    for (int i = 0; i < Num_OpClasses; ++i) {
        cprintf("Ready list %i size: %i\n", i, readyInsts[i].size());

        cprintf("\n");
    }

    cprintf("Non speculative list size: %i\n", nonSpecInsts.size());

    NonSpecMapIt non_spec_it = nonSpecInsts.begin();
    NonSpecMapIt non_spec_end_it = nonSpecInsts.end();

    cprintf("Non speculative list: ");

    while (non_spec_it != non_spec_end_it) {
        cprintf("%s [sn:%llu]", (*non_spec_it).second->pcState(),
                (*non_spec_it).second->seqNum);
        ++non_spec_it;
    }

    cprintf("\n");

    ListOrderIt list_order_it = listOrder.begin();
    ListOrderIt list_order_end_it = listOrder.end();
    int i = 1;

    cprintf("List order: ");

    while (list_order_it != list_order_end_it) {
        cprintf("%i OpClass:%i [sn:%llu] ", i, (*list_order_it).queueType,
                (*list_order_it).oldestInst);

        ++list_order_it;
        ++i;
    }

    cprintf("\n");
}


template <class Impl>
void
InstructionQueue<Impl>::dumpInsts()
{
    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        int num = 0;
        int valid_num = 0;
        ListIt inst_list_it = instList[tid].begin();

        while (inst_list_it != instList[tid].end()) {
            cprintf("Instruction:%i\n", num);
            if (!(*inst_list_it)->isSquashed()) {
                if (!(*inst_list_it)->isIssued()) {
                    ++valid_num;
                    cprintf("Count:%i\n", valid_num);
                } else if ((*inst_list_it)->isMemRef() &&
                           !(*inst_list_it)->memOpDone()) {
                    // Loads that have not been marked as executed
                    // still count towards the total instructions.
                    ++valid_num;
                    cprintf("Count:%i\n", valid_num);
                }
            }

            cprintf("PC: %s\n[sn:%llu]\n[tid:%i]\n"
                    "Issued:%i\nSquashed:%i\n",
                    (*inst_list_it)->pcState(),
                    (*inst_list_it)->seqNum,
                    (*inst_list_it)->threadNumber,
                    (*inst_list_it)->isIssued(),
                    (*inst_list_it)->isSquashed());

            if ((*inst_list_it)->isMemRef()) {
                cprintf("MemOpDone:%i\n", (*inst_list_it)->memOpDone());
            }

            cprintf("\n");

            inst_list_it++;
            ++num;
        }
    }

    cprintf("Insts to Execute list:\n");

    int num = 0;
    int valid_num = 0;
    ListIt inst_list_it = instsToExecute.begin();

    while (inst_list_it != instsToExecute.end())
    {
        cprintf("Instruction:%i\n",
                num);
        if (!(*inst_list_it)->isSquashed()) {
            if (!(*inst_list_it)->isIssued()) {
                ++valid_num;
                cprintf("Count:%i\n", valid_num);
            } else if ((*inst_list_it)->isMemRef() &&
                       !(*inst_list_it)->memOpDone()) {
                // Loads that have not been marked as executed
                // still count towards the total instructions.
                ++valid_num;
                cprintf("Count:%i\n", valid_num);
            }
        }

        cprintf("PC: %s\n[sn:%llu]\n[tid:%i]\n"
                "Issued:%i\nSquashed:%i\n",
                (*inst_list_it)->pcState(),
                (*inst_list_it)->seqNum,
                (*inst_list_it)->threadNumber,
                (*inst_list_it)->isIssued(),
                (*inst_list_it)->isSquashed());

        if ((*inst_list_it)->isMemRef()) {
            cprintf("MemOpDone:%i\n", (*inst_list_it)->memOpDone());
        }

        cprintf("\n");

        inst_list_it++;
        ++num;
    }
}

template <class Impl>
void
InstructionQueue<Impl>::addToPredictable(const DynInstPtr &inst)
{
    instsPredictable.push_back(inst);
}

template <class Impl>
void
InstructionQueue<Impl>::removeFromPredictable(const DynInstPtr &inst)
{
    auto pred_it = instsPredictable.begin();
    DPRINTF(IQ, "Squashing from predictables list, entries: %d,"
            " leading inst [sn:%llu]",
            instsPredictable.size(),
            (*pred_it)->seqNum);
    while (pred_it != instsPredictable.end())
    {
        if (*pred_it == inst) {
            instsPredictable.erase(pred_it);
            DPRINTF(IQ, "Removed inst [sn:%llu] from predictables\n",
                    inst->seqNum);
            break;
        }
        pred_it++;
    }
    if (pred_it == instsPredictable.end())
        DPRINTF(IQ, "Could not find inst in predictables\n");
}

template <class Impl>
void
InstructionQueue<Impl>::tick()
{
    memDepUnit[0].pruneTaints();
    pruneTaintedBranches();
}


template <class Impl>
void
InstructionQueue<Impl>::propagateTaints(const DynInstPtr &inst, ThreadID tid)
{
    if (!cpu->STT) return;
    DynInstPtr youngestTaint = nullptr;

    DPRINTF(TaintTrackerDebug, "Propagating taints for [sn:%llu]\n",
            inst->seqNum);

    //If the inst is a store, it handles being ready other ways
    if (inst->isStore()) {
        assert(!inst->underShadow());
        return;
    }

    if (inst->isLoad()) {
        youngestTaint = inst;
    } else {
        for (int src_reg_idx = 0;
            src_reg_idx < inst->numSrcRegs();
            src_reg_idx++)
        {
            PhysRegIdPtr src_reg =
                inst->regs.renamedSrcIdx(src_reg_idx);
            if (src_reg->getNumPinnedWritesToComplete() != 0) continue;
            if (cpu->taintTracker.isTainted(src_reg)) {
                DynInstPtr taintInst =
                    cpu->taintTracker.getTaintInstruction(src_reg);
                if (!youngestTaint) {
                    youngestTaint = taintInst;
                } else if (taintInst->seqNum > youngestTaint->seqNum)
                    youngestTaint = taintInst;
            }
        }
    }

    if (youngestTaint) {
        assert(youngestTaint->seqNum <= inst->seqNum);

        for (int dest_reg_idx = 0;
         dest_reg_idx < inst->numDestRegs();
         dest_reg_idx++)
        {
            PhysRegIdPtr dest_reg =
                inst->regs.renamedDestIdx(dest_reg_idx);
            //idk why but 16 is always 0?
            if (dest_reg->flatIndex() == 16) continue;
            cpu->taintTracker.insertTaint(dest_reg, youngestTaint);
        }
    }
}


#endif//__CPU_O3_INST_QUEUE_IMPL_HH__
