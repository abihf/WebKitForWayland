/*
 * Copyright (C) 2015 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef AirLiveness_h
#define AirLiveness_h

#if ENABLE(B3_JIT)

#include "AirBasicBlock.h"
#include "AirInstInlines.h"
#include "AirTmpInlines.h"
#include "B3IndexMap.h"
#include "B3IndexSet.h"
#include <wtf/IndexSparseSet.h>

namespace JSC { namespace B3 { namespace Air {

template<Arg::Type adapterType>
struct TmpLivenessAdapter {
    typedef Tmp Thing;
    typedef HashSet<unsigned> IndexSet;

    TmpLivenessAdapter(Code&) { }

    static unsigned maxIndex(Code& code)
    {
        unsigned numTmps = code.numTmps(adapterType);
        return AbsoluteTmpMapper<adapterType>::absoluteIndex(numTmps);
    }
    static bool acceptsType(Arg::Type type) { return type == adapterType; }
    static unsigned valueToIndex(Tmp tmp) { return AbsoluteTmpMapper<adapterType>::absoluteIndex(tmp); }
    static Tmp indexToValue(unsigned index) { return AbsoluteTmpMapper<adapterType>::tmpFromAbsoluteIndex(index); }
};

struct StackSlotLivenessAdapter {
    typedef StackSlot* Thing;
    typedef HashSet<unsigned, DefaultHash<unsigned>::Hash, WTF::UnsignedWithZeroKeyHashTraits<unsigned>> IndexSet;

    StackSlotLivenessAdapter(Code& code)
        : m_code(code)
    {
    }

    static unsigned maxIndex(Code& code)
    {
        return code.stackSlots().size() - 1;
    }
    static bool acceptsType(Arg::Type) { return true; }
    static unsigned valueToIndex(StackSlot* stackSlot) { return stackSlot->index(); }
    StackSlot* indexToValue(unsigned index) { return m_code.stackSlots()[index]; }

private:
    Code& m_code;
};

template<typename Adapter>
class AbstractLiveness : private Adapter {
    struct Workset;
public:
    AbstractLiveness(Code& code)
        : Adapter(code)
        , m_workset(Adapter::maxIndex(code))
        , m_liveAtHead(code.size())
        , m_liveAtTail(code.size())
    {
        // The liveAtTail of each block automatically contains the LateUse's of the terminal.
        for (BasicBlock* block : code) {
            typename Adapter::IndexSet& liveAtTail = m_liveAtTail[block];

            block->last().forEach<typename Adapter::Thing>(
                [&] (typename Adapter::Thing& thing, Arg::Role role, Arg::Type type) {
                    if (Arg::isLateUse(role) && Adapter::acceptsType(type))
                        liveAtTail.add(Adapter::valueToIndex(thing));
                });
        }

        // Blocks with new live values at tail.
        BitVector dirtyBlocks;
        for (size_t blockIndex = 0; blockIndex < code.size(); ++blockIndex)
            dirtyBlocks.set(blockIndex);

        bool changed;
        do {
            changed = false;

            for (size_t blockIndex = code.size(); blockIndex--;) {
                BasicBlock* block = code.at(blockIndex);
                if (!block)
                    continue;

                if (!dirtyBlocks.quickClear(blockIndex))
                    continue;

                LocalCalc localCalc(*this, block);
                for (size_t instIndex = block->size(); instIndex--;)
                    localCalc.execute(instIndex);

                Vector<unsigned>& liveAtHead = m_liveAtHead[block];

                // We only care about Tmps that were discovered in this iteration. It is impossible
                // to remove a live value from the head.
                // We remove all the values we already knew about so that we only have to deal with
                // what is new in LiveAtHead.
                if (m_workset.size() == liveAtHead.size())
                    m_workset.clear();
                else {
                    for (unsigned liveIndexAtHead : liveAtHead)
                        m_workset.remove(liveIndexAtHead);
                }

                if (m_workset.isEmpty())
                    continue;

                liveAtHead.reserveCapacity(liveAtHead.size() + m_workset.size());
                for (unsigned newValue : m_workset)
                    liveAtHead.uncheckedAppend(newValue);

                for (BasicBlock* predecessor : block->predecessors()) {
                    typename Adapter::IndexSet& liveAtTail = m_liveAtTail[predecessor];
                    for (unsigned newValue : m_workset) {
                        if (liveAtTail.add(newValue)) {
                            if (dirtyBlocks.quickSet(predecessor->index()))
                                changed = true;
                        }
                    }
                }
            }
        } while (changed);

        m_liveAtHead.clear();
    }

    // This calculator has to be run in reverse.
    class LocalCalc {
    public:
        LocalCalc(AbstractLiveness& liveness, BasicBlock* block)
            : m_liveness(liveness)
            , m_block(block)
        {
            auto& workset = liveness.m_workset;
            workset.clear();
            typename Adapter::IndexSet& liveAtTail = liveness.m_liveAtTail[block];
            for (unsigned index : liveAtTail)
                workset.add(index);
        }

        struct Iterator {
            Iterator(Adapter& adapter, IndexSparseSet<UnsafeVectorOverflow>::const_iterator sparceSetIterator)
                : m_adapter(adapter)
                , m_sparceSetIterator(sparceSetIterator)
            {
            }

            Iterator& operator++()
            {
                ++m_sparceSetIterator;
                return *this;
            }

            typename Adapter::Thing operator*() const
            {
                return m_adapter.indexToValue(*m_sparceSetIterator);
            }

            bool operator==(const Iterator& other) { return m_sparceSetIterator == other.m_sparceSetIterator; }
            bool operator!=(const Iterator& other) { return m_sparceSetIterator != other.m_sparceSetIterator; }

        private:
            Adapter& m_adapter;
            IndexSparseSet<UnsafeVectorOverflow>::const_iterator m_sparceSetIterator;
        };

        struct Iterable {
            Iterable(AbstractLiveness& liveness)
                : m_liveness(liveness)
            {
            }

            Iterator begin() const { return Iterator(m_liveness, m_liveness.m_workset.begin()); }
            Iterator end() const { return Iterator(m_liveness, m_liveness.m_workset.end()); }

        private:
            AbstractLiveness& m_liveness;
        };

        Iterable live() const
        {
            return Iterable(m_liveness);
        }

        void execute(unsigned instIndex)
        {
            Inst& inst = m_block->at(instIndex);
            auto& workset = m_liveness.m_workset;
            // First handle def's.
            inst.forEach<typename Adapter::Thing>(
                [&] (typename Adapter::Thing& thing, Arg::Role role, Arg::Type type) {
                    if (Arg::isDef(role) && Adapter::acceptsType(type))
                        workset.remove(Adapter::valueToIndex(thing));
                });

            // Then handle use's.
            inst.forEach<typename Adapter::Thing>(
                [&] (typename Adapter::Thing& thing, Arg::Role role, Arg::Type type) {
                    if (Arg::isEarlyUse(role) && Adapter::acceptsType(type))
                        workset.add(Adapter::valueToIndex(thing));
                });

            // And finally, handle the late use's of the previous instruction.
            if (instIndex) {
                Inst& prevInst = m_block->at(instIndex - 1);
                prevInst.forEach<typename Adapter::Thing>(
                    [&] (typename Adapter::Thing& thing, Arg::Role role, Arg::Type type) {
                        if (Arg::isLateUse(role) && Adapter::acceptsType(type))
                            workset.add(Adapter::valueToIndex(thing));
                    });
            }
        }

    private:
        AbstractLiveness& m_liveness;
        BasicBlock* m_block;
    };

private:
    friend class LocalCalc;
    friend struct LocalCalc::Iterable;

    IndexSparseSet<UnsafeVectorOverflow> m_workset;
    IndexMap<BasicBlock, Vector<unsigned>> m_liveAtHead;
    IndexMap<BasicBlock, typename Adapter::IndexSet> m_liveAtTail;
};

template<Arg::Type type>
using TmpLiveness = AbstractLiveness<TmpLivenessAdapter<type>>;

typedef AbstractLiveness<TmpLivenessAdapter<Arg::GP>> GPLiveness;
typedef AbstractLiveness<TmpLivenessAdapter<Arg::FP>> FPLiveness;
typedef AbstractLiveness<StackSlotLivenessAdapter> StackSlotLiveness;

} } } // namespace JSC::B3::Air

#endif // ENABLE(B3_JIT)

#endif // AirLiveness_h

