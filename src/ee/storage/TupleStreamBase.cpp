/* This file is part of VoltDB.
 * Copyright (C) 2008-2015 VoltDB Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with VoltDB.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "storage/TupleStreamBase.h"

#include "common/TupleSchema.h"
#include "common/types.h"
#include "common/NValue.hpp"
#include "common/ValuePeeker.hpp"
#include "common/tabletuple.h"
#include "common/ExportSerializeIo.h"
#include "common/executorcontext.hpp"

#include <cstdio>
#include <limits>
#include <iostream>
#include <cassert>
#include <ctime>
#include <utility>
#include <math.h>

using namespace std;
using namespace voltdb;

const int MAX_BUFFER_AGE = 4000;

TupleStreamBase::TupleStreamBase()
    : m_lastFlush(0), m_defaultCapacity(EL_BUFFER_SIZE),
      m_uso(0), m_currBlock(NULL),
      // snapshot restores will call load table which in turn
      // calls appendTupple with LONG_MIN transaction ids
      // this allows initial ticks to succeed after rejoins
      m_openSpHandle(0),
      m_openSequenceNumber(-1),
      m_openUniqueId(0),
      m_openTransactionUso(0),
      m_committedSpHandle(0), m_committedUso(0),
      m_committedSequenceNumber(-1),
      m_committedUniqueId(0)
{
    extendBufferChain(m_defaultCapacity);
}

void
TupleStreamBase::setDefaultCapacity(size_t capacity)
{
    assert (capacity > 0);
    if (m_uso != 0 || m_openSpHandle != 0 ||
        m_openTransactionUso != 0 || m_committedSpHandle != 0)
    {
        throwFatalException("setDefaultCapacity only callable before "
                            "TupleStreamBase is used");
    }
    cleanupManagedBuffers();
    m_defaultCapacity = capacity;
    extendBufferChain(m_defaultCapacity);
}



/*
 * Essentially, shutdown.
 */
void TupleStreamBase::cleanupManagedBuffers()
{
    StreamBlock *sb = NULL;

    discardBlock(m_currBlock);
    m_currBlock = NULL;

    while (m_pendingBlocks.empty() != true) {
        sb = m_pendingBlocks.front();
        m_pendingBlocks.pop_front();
        discardBlock(sb);
    }
}

/*
 * Handoff fully committed blocks to the top end.
 *
 * This is the only function that should modify m_openSpHandle,
 * m_openTransactionUso.
 */
size_t TupleStreamBase::commit(int64_t lastCommittedSpHandle, int64_t currentSpHandle, int64_t txnId, int64_t uniqueId, bool sync, bool flush)
{
    size_t beginUso = 0;

    if (currentSpHandle < m_openSpHandle)
    {
        throwFatalException(
                "Active transactions moving backwards: openSpHandle is %jd, while the current spHandle is %jd",
                (intmax_t)m_openSpHandle, (intmax_t)currentSpHandle
                );
    }

    // more data for an ongoing transaction with no new committed data
    if ((currentSpHandle == m_openSpHandle) &&
        (lastCommittedSpHandle == m_committedSpHandle))
    {
        //std::cout << "Current spHandle(" << currentSpHandle << ") == m_openSpHandle(" << m_openSpHandle <<
        //") && lastCommittedSpHandle(" << lastCommittedSpHandle << ") m_committedSpHandle(" <<
        //m_committedSpHandle << ")" << std::endl;
        if (sync) {
            pushExportBuffer(
                    NULL,
                    true,
                    false);
        }

        if (flush) {
            extendBufferChain(0);
        }

        return beginUso;
    }

    // If the current TXN ID has advanced, then we know that:
    // - The old open transaction has been committed
    // - The current transaction is now our open transaction
    // If last committed and current sphandle are the same
    // this isn't a new transaction, the block below handles it better
    // by ending the current open transaction and not starting a new one
    if (m_openSpHandle < currentSpHandle && currentSpHandle != lastCommittedSpHandle)
    {
        if (m_openSpHandle > 0 && m_openSpHandle > m_committedSpHandle) {
            endTransaction(m_openSequenceNumber, m_openUniqueId);
        }
        //std::cout << "m_openSpHandle(" << m_openSpHandle << ") < currentSpHandle("
        //<< currentSpHandle << ")" << std::endl;
        m_committedUso = m_uso;
        m_committedSequenceNumber = m_openSequenceNumber;
        m_committedUniqueId = m_openUniqueId;
        // Advance the tip to the new transaction.
        m_committedSpHandle = m_openSpHandle;
        m_openSpHandle = currentSpHandle;
        m_openSequenceNumber++;
        m_openUniqueId = uniqueId;

        if (flush) {
            // Don't move the txn we've just finished to the new buffer
            extendBufferChain(0, false);
        }

        beginUso = beginTransaction(m_openSequenceNumber, uniqueId);
    }

    // now check to see if the lastCommittedSpHandle tells us that our open
    // transaction should really be committed.  If so, update the
    // committed state.
    if (m_openSpHandle <= lastCommittedSpHandle)
    {
        //std::cout << "m_openSpHandle(" << m_openSpHandle << ") <= lastCommittedSpHandle(" <<
        //lastCommittedSpHandle << ")" << std::endl;
        if (m_openSpHandle > 0 && m_openSpHandle > m_committedSpHandle) {
            endTransaction(m_openSequenceNumber, m_openUniqueId);
        }
        m_committedSequenceNumber = m_openSequenceNumber;
        m_committedUso = m_uso;
        m_committedSpHandle = m_openSpHandle;
        m_committedUniqueId = m_openUniqueId;

        if (flush) {
            // Don't move the txn we've just finished to the new buffer
            extendBufferChain(0, false);
        }
    }

    pushPendingBlocks();

    if (sync) {
        pushExportBuffer(
                NULL,
                true,
                false);
    }

    return beginUso;
}

void TupleStreamBase::pushPendingBlocks() {
    while (!m_pendingBlocks.empty())
    {
        StreamBlock* block = m_pendingBlocks.front();
        //std::cout << "m_committedUso(" << m_committedUso << "), block->uso() + block->offset() == "
        //<< (block->uso() + block->offset()) << std::endl;

        // check that the entire remainder is committed
        if (m_committedUso >= (block->uso() + block->offset()))
        {
            //The block is handed off to the topend which is responsible for releasing the
            //memory associated with the block data. The metadata is deleted here.
            pushExportBuffer(
                    block,
                    false,
                    false);
            delete block;
            m_pendingBlocks.pop_front();
        }
        else
        {
            break;
        }
    }
}

/*
 * Discard all data with a uso gte mark
 */
void TupleStreamBase::rollbackTo(size_t mark)
{
    if (mark > m_uso) {
        throwFatalException("Truncating the future.");
    }

    // back up the universal stream counter
    m_uso = mark;

    // working from newest to oldest block, throw
    // away blocks that are fully after mark; truncate
    // the block that contains mark.
    if (!(m_currBlock->uso() >= mark)) {
        m_currBlock->truncateTo(mark);
    }
    else {
        StreamBlock *sb = NULL;
        discardBlock(m_currBlock);
        m_currBlock = NULL;
        while (m_pendingBlocks.empty() != true) {
            sb = m_pendingBlocks.back();
            m_pendingBlocks.pop_back();
            if (sb->uso() >= mark) {
                discardBlock(sb);
            }
            else {
                sb->truncateTo(mark);
                m_currBlock = sb;
                break;
            }
        }
        if (m_currBlock == NULL) {
            extendBufferChain(m_defaultCapacity);
        }
        m_currBlock->recordCompletedTxnForDR(m_committedSequenceNumber, m_committedUniqueId);
    }
    m_openSequenceNumber = m_committedSequenceNumber;
    m_openSpHandle = m_committedSpHandle;
    m_openUniqueId = m_committedUniqueId;
}

/*
 * Correctly release and delete a managed buffer that won't
 * be handed off
 */
void TupleStreamBase::discardBlock(StreamBlock *sb) {
    delete [] sb->rawPtr();
    delete sb;
}

/*
 * Allocate another buffer, preserving the current buffer's content in
 * the pending queue.
 */
void TupleStreamBase::extendBufferChain(size_t minLength, bool continueTxn /*= true*/)
{
    if (m_defaultCapacity < minLength) {
        // exportxxx: rollback instead?
        throwFatalException("Default capacity is less than required buffer size.");
    }
    StreamBlock *oldBlock = NULL;
    size_t uso = m_uso;

    if (m_currBlock) {
        if (m_currBlock->offset() > 0) {
            m_pendingBlocks.push_back(m_currBlock);
            oldBlock = m_currBlock;
            m_currBlock = NULL;
        }
        // fully discard empty blocks. makes valgrind/testcase
        // conclusion easier.
        else {
            discardBlock(m_currBlock);
            m_currBlock = NULL;
        }
    }
    size_t blockSize = m_defaultCapacity;
    bool openTransaction = checkOpenTransaction(oldBlock, minLength, blockSize, uso, continueTxn);

    char *buffer = new char[blockSize];
    if (!buffer) {
        throwFatalException("Failed to claim managed buffer for Export.");
    }
    m_currBlock = new StreamBlock(buffer, blockSize, uso);
    if (blockSize > m_defaultCapacity) {
        m_currBlock->setType(LARGE_STREAM_BLOCK);
    }

    if (blockSize == 0) {
        rollbackTo(uso);
        throw SQLException(SQLException::volt_output_buffer_overflow, "Transaction is bigger than DR Buffer size");
    }

    if (openTransaction) {
        size_t partialTxnLength = oldBlock->offset() - oldBlock->lastDRBeginTxnOffset();
        ::memcpy(m_currBlock->mutableDataPtr(), oldBlock->mutableLastBeginTxnDataPtr(), partialTxnLength);
        m_currBlock->startDRSequenceNumber(m_openSequenceNumber);
        m_currBlock->recordLastBeginTxnOffset();
        m_currBlock->consumed(partialTxnLength);
        ::memset(oldBlock->mutableLastBeginTxnDataPtr(), 0, partialTxnLength);
        oldBlock->truncateTo(uso);
        oldBlock->clearLastBeginTxnOffset();
        // If the whole previous block has been moved to new block, discards the empty one.
        if (oldBlock->offset() == 0) {
            m_pendingBlocks.pop_back();
            discardBlock(oldBlock);
        }
    }


    pushPendingBlocks();
}

/*
 * Create a new buffer and flush all pending committed data.
 * Creating a new buffer will push all queued data into the
 * pending list for commit to operate against.
 */
void
TupleStreamBase::periodicFlush(int64_t timeInMillis,
                                  int64_t lastCommittedSpHandle)
{
    // negative timeInMillis instructs a mandatory flush
    if (timeInMillis < 0 || (timeInMillis - m_lastFlush > MAX_BUFFER_AGE)) {
        int64_t maxSpHandle = std::max(m_openSpHandle, lastCommittedSpHandle);
        if (timeInMillis > 0) {
            m_lastFlush = timeInMillis;
        }

        /*
         * handle cases when the currentSpHandle was set by rejoin
         * and snapshot restore load table statements that send in
         * Long.MIN_VALUE as their tx id. ee's tick which results
         * in calls to this procedure may be called right after
         * these.
         */
        commit(lastCommittedSpHandle, maxSpHandle, maxSpHandle, std::numeric_limits<int64_t>::min(), timeInMillis < 0 ? true : false, true);
    }
}
