/* This file is part of VoltDB.
 * Copyright (C) 2008-2013 VoltDB Inc.
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

package org.voltdb.planner.parseinfo;

import org.voltdb.catalog.Database;
import org.voltdb.expressions.TupleValueExpression;
import org.voltdb.planner.AbstractParsedStmt;
import org.voltdb.planner.PartitioningForStatement;
import org.voltdb.planner.ParsedSelectStmt.ParsedColInfo;

/**
 * StmtTableScan caches data related to a given instance of a sub-query within the statement scope
 */
public class StmtSubqueryScan extends StmtTableScan {

    public StmtSubqueryScan(TempTable tempTable, String tableAlias) {
        super(tableAlias);
        assert (tempTable != null);
        m_tempTable = tempTable;
    }

    @Override
    public TABLE_SCAN_TYPE getScanType() {
        return TABLE_SCAN_TYPE.TEMP_TABLE_SCAN;
    }

    @Override
    public String getTableName() {
        return m_tempTable.getTableName();
    }

    @Override
    public TempTable getTempTable() {
        assert (m_tempTable != null);
        return m_tempTable;
    }

    @Override
    public boolean getIsreplicated() {
        return m_tempTable.getIsreplicated();
    }

    @Override
    public void setPartitioning(PartitioningForStatement partitioning) {
        m_partitioning = partitioning;
    }

    @Override
    public boolean isPartitioningColumn(String columnName) {
        assert (m_partitioning != null);
        return m_partitioning.isPartitionColumnByAnyTable(columnName);
    }

    @Override
    public TupleValueExpression resolveTVEForDB(Database db, TupleValueExpression tve) {
        String columnName = tve.getColumnName();
        for (ParsedColInfo colInfo : m_tempTable.getOrigSchema()) {
            boolean match = columnName.equals(colInfo.alias) ||
                    (colInfo.alias == null && columnName.equals(colInfo.columnName));
            if (match) {
                AbstractParsedStmt subQuery = m_tempTable.getSubQuery();
                assert(subQuery.tableAliasIndexMap.containsKey(colInfo.tableAlias));
                StmtTableScan origTable = subQuery.stmtCache.get(subQuery.tableAliasIndexMap.get(colInfo.tableAlias));
                assert(origTable != null);
                // Prepare the tve to go the level down
                tve.setTableName(colInfo.tableName);
                tve.setColumnName(colInfo.columnName);
                tve = origTable.resolveTVEForDB(db, tve);
                // restore the table and column names and the index from the current level
                tve.setTableName(getTableName());
                tve.setColumnName(columnName);
                tve.setColumnIndex(colInfo.index);
            }
        }
        assert (tve.getColumnIndex() != -1);
        return tve;
    }

    // Sub-Query
    private TempTable m_tempTable = null;
    // The partitioning object for that sub-query
    PartitioningForStatement m_partitioning = null;

}