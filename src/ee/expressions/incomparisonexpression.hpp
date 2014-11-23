/* This file is part of VoltDB.
 * Copyright (C) 2008-2014 VoltDB Inc.
 *
 * This file contains original code and/or modifications of original code.
 * Any modifications made by VoltDB Inc. are licensed under the following
 * terms and conditions:
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
/* Copyright (C) 2008 by H-Store Project
 * Brown University
 * Massachusetts Institute of Technology
 * Yale University
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef HSTOREINCOMPARISONEXPRESSION_H
#define HSTOREINCOMPARISONEXPRESSION_H

#include "common/common.h"
#include "common/executorcontext.hpp"
#include "common/serializeio.h"
#include "common/tabletuple.h"
#include "common/TupleSchema.h"
#include "common/ValuePeeker.hpp"
#include "expressions/abstractexpression.h"
#include "expressions/comparisonexpression.h"
#include "storage/table.h"
#include "storage/tableiterator.h"

#include <string>
#include <cassert>

namespace voltdb {

//Assumption - quantifier is on the right
template <typename OP, typename ValueExtractorLeft, typename ValueExtractorRight>
class VectorComparisonExpression : public AbstractExpression {
public:
    VectorComparisonExpression(ExpressionType et,
                           AbstractExpression *left,
                           AbstractExpression *right,
                           QuantifierType quantifier,
                           bool leftQuantifier)
        : AbstractExpression(et, left, right),
          m_quantifier(quantifier), m_leftQuantifier(leftQuantifier)
    {
        assert(left != NULL);
        assert(right != NULL);
    };

    NValue eval(const TableTuple *tuple1, const TableTuple *tuple2) const;

    std::string debugInfo(const std::string &spacer) const {
        return (spacer + "VectorComparisonExpression\n");
    }

private:
    QuantifierType m_quantifier;
    bool m_leftQuantifier;
};

struct NValueExtractor
{
    typedef NValue ValueType;

    NValueExtractor(NValue value) :
        m_value(value), m_hasNext(true)
    {}

    int resultSize() const
    {
        return hasNullValue() ? 0 : 1;
    }

    bool hasNullValue() const
    {
        return m_value.isNull();
    }

    bool hasNext()
    {
        return m_hasNext;
    }

    ValueType next()
    {
        m_hasNext = false;
        return m_value;
    }

    bool isNullValue(const ValueType& value) const
    {
        return value.isNull();
    }

    template<typename OP>
    bool compare(OP comp, const TableTuple& tuple) const
    {
        assert(tuple.getSchema()->columnCount() == 1);
        return comp.cmp(m_value, tuple.getNValue(0)).isTrue();
    }

    template<typename OP>
    bool compare(OP comp, const NValue& nvalue) const
    {
        return comp.cmp(m_value, nvalue).isTrue();
    }

    std::string debug() const
    {
        return m_value.isNull() ? "NULL" : m_value.debug();
    }

    ValueType m_value;
    bool m_hasNext;
};

struct TupleExtractor
{
    typedef TableTuple ValueType;

    TupleExtractor(NValue value) :
        m_table(getOutputTable(value)),
        m_iterator(m_table->iterator()),
        m_tuple(m_table->schema()),
        m_size(m_table->activeTupleCount())
    {}

    int resultSize() const
    {
        return m_size;
    }

    bool hasNext()
    {
        return m_iterator.next(m_tuple);
    }

    ValueType next()
    {
        return m_tuple;
    }

    bool hasNullValue() const
    {
        return isNullValue(m_tuple);
    }

    bool isNullValue(const ValueType& value) const
    {
        if (!value.isNullTuple()){
            int schemaSize = m_tuple.getSchema()->columnCount();
            for (int columnIdx = 0; columnIdx < schemaSize; ++columnIdx)
            {
                if (m_tuple.isNull(columnIdx)) {
                    return true;
                }
            }
            return false;
        }
        return true;
    }

    template<typename OP>
    bool compare(OP comp, const TableTuple& tuple) const
    {
        assert(m_tuple.getSchema()->columnCount() == tuple.getSchema()->columnCount());
        int schemaSize = m_tuple.getSchema()->columnCount();
        // Lexicographical compare two sequences
        for (int columnIdx = 0; columnIdx < schemaSize; ++columnIdx)
        {
            if (comp.cmp(m_tuple.getNValue(columnIdx), tuple.getNValue(columnIdx)).isTrue())
            {
                return true;
            }
            if (comp.cmp(tuple.getNValue(columnIdx), m_tuple.getNValue(columnIdx)).isTrue())
            {
                return false;
            }
        }
        return false;
    }

    template<typename OP>
    bool compare(OP comp, const NValue& nvalue) const
    {
        assert(m_tuple.getSchema()->columnCount() == 1);
        return comp.cmp(m_tuple.getNValue(0), nvalue).isTrue();
    }

    std::string debug() const
    {
        return m_tuple.isNullTuple() ? "NULL" : m_tuple.debug("TEMP");
    }

private:
    static Table* getOutputTable(const NValue& value)
    {
        int subqueryId = ValuePeeker::peekInteger(value);
        ExecutorContext* exeContext = ExecutorContext::getExecutorContext();
        Table* table = exeContext->getSubqueryOutputTable(subqueryId);
        assert(table != NULL);
        return table;
    }

    Table* m_table;
    TableIterator& m_iterator;
    ValueType m_tuple;
    int m_size;
};

template <typename OP, typename ValueExtractorOuter, typename ValueExtractorInner>
NValue VectorComparisonExpression<OP, ValueExtractorOuter, ValueExtractorInner>::eval(const TableTuple *tuple1, const TableTuple *tuple2) const
{
    // Outer expression can be either a row (expr1, expr2, expr3...) or a single expr
    // Inner expression is a select subquery
    // The outer_expr OP (SELECT inner_expr ...) evaluates as follows:
    // There is an exact match OP (outer_expr, inner_expr) == true => TRUE
    // There no match and the inner_expr produces a row where inner_expr is NULL => NULL
    // There no match and the inner_expr produces only non- NULL rows or empty => FASLE
    // The outer_expr is NULL or empty and the inner_expr is empty => FASLE
    // The outer_expr is NULL or empty and the inner_expr produces any row => NULL

    // Evaluate the outer_expr. The return value can be either the value itself or a subquery id
    // in case of the row expression on the left side
    NValue lvalue = m_left->eval(tuple1, tuple2);
    ValueExtractorOuter outerExtractor(lvalue);
    if (outerExtractor.resultSize() > 1)
    {
        // throw runtime exception
        char message[256];
        snprintf(message, 256, "More than one row returned by a scalar/row subquery");
        throw SerializableEEException(VOLT_EE_EXCEPTION_TYPE_EEEXCEPTION, message);
    }

    // Evaluate the inner_expr. The return value is a subquery id
    NValue rvalue = m_right->eval(tuple1, tuple2);
    ValueExtractorInner innerExtractor(rvalue);
    if (m_quantifier == QUANTIFIER_TYPE_NONE && innerExtractor.resultSize() > 1)
    {
        // throw runtime exception
        char message[256];
        snprintf(message, 256, "More than one row returned by a scalar/row subquery");
        throw SerializableEEException(VOLT_EE_EXCEPTION_TYPE_EEEXCEPTION, message);
    }

    if (!outerExtractor.hasNext() || outerExtractor.hasNullValue()) {
        NValue retval = NValue::getFalse();
        if (innerExtractor.resultSize() > 0) {
            retval.setNull();
        }
        return retval;
    }

    //  Iterate over the inner results until
    //  no qualifier - the first match ( single row at most)
    //  ANY qualifier - the first match
    //  ALL qualifier - the first mismatch
    int tuple_ctr = 0;
    bool hasInnerNull = false;
    bool hasMatch = false;
    while (innerExtractor.hasNext())
    {
        ++tuple_ctr;
        typename ValueExtractorInner::ValueType innerValue = innerExtractor.next();
        if (innerExtractor.isNullValue(innerValue))
        {
            hasInnerNull = true;
            continue;
        }
        if (outerExtractor.template compare<OP>(OP(), innerValue)) {
            hasMatch = true;
            if (m_quantifier == QUANTIFIER_TYPE_ANY) {
                return NValue::getTrue();
            }
        } else if (m_quantifier == QUANTIFIER_TYPE_ALL) {
            return NValue::getFalse();
        }
    }

    if (hasMatch) {
        return NValue::getTrue();
    } else {
        // No match
        NValue retval = NValue::getFalse();
        if (hasInnerNull == true) {
            retval.setNull();
        }
        return retval;
    }
}

}
#endif
