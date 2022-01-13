#ifndef __CPU_PRED_ADD_PRED_SIMPLE_PRED_CC__
#define __CPU_PRED_ADD_PRED_SIMPLE_PRED_CC__

#include "cpu/o3/add_pred/simple_pred.hh"

#include <iostream>

#include "debug/AddrPredDebug.hh"
#include "debug/AddrPrediction.hh"

SimplePred::SimplePred(const Params &params)
    : BaseAddPred(params)
{

}

SimplePred::~SimplePred()
{

}

Addr
SimplePred::predictFromPC(Addr pc, int runAhead)
{
    DPRINTF(AddrPrediction, "Attempting to predict for pc "
        "%llx with runahead %d\n", pc, runAhead);
    int index = pc % numEntries;
    struct AddrHistory* entry = entries[index];
    if (entry && entry->confidence >= 8) {
        return entry->lastAddr + (entry->strideHistory.front()*runAhead);
    }
    return 0;
}

void
SimplePred::updatePredictor(Addr realAddr, Addr pc,
                                   InstSeqNum seqNum,
                                   int packetSize)
{
    DPRINTF(AddrPrediction, "Updating predictor for [sn:%llu],"
            "with pc %llx and realAddr %llx\n",
            seqNum, pc, realAddr);
    int index = pc % numEntries;
    struct AddrHistory* entry = entries[index];
    if (!entry) {
        AddrHistory* new_entry =
            new AddrHistory(seqNum, pc, realAddr, 1, packetSize);
        entries[index] = new_entry;
        DPRINTF(AddrPredDebug, "New entry created, returning\n");
        return;
    }
    assert(pc = entry->pc);

    if (entry->strideHistory.empty()) {
        entry->strideHistory.push(realAddr - entry->lastAddr);
        DPRINTF(AddrPredDebug, "Empty stridehistory, making new\n");
        return;
    }

    Addr strideAddr = entry->lastAddr + entry->strideHistory.front();

    if (strideAddr == 0) {
    } else if (strideAddr == realAddr) {
        entry->confidence++;
    } else {
        entry->confidence--;
    }
    entry->strideHistory.pop();
    entry->strideHistory.push(
                              realAddr - entry->lastAddr);
    entry->lastAddr = realAddr;
}

int
SimplePred::getPacketSize(Addr pc)
{
    int index = pc % numEntries;
    struct AddrHistory* entry = entries[index];
    if (!entry) return 0;
    return entry->packetSize;
}

const std::string
SimplePred::name() const
{
    return "simple_predictor";
}

#endif
