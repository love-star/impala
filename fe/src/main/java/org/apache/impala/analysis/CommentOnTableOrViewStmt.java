// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

package org.apache.impala.analysis;

import com.google.common.base.Preconditions;
import org.apache.impala.authorization.Privilege;
import org.apache.impala.common.AnalysisException;
import org.apache.impala.thrift.TCommentOnParams;

import java.util.List;

/**
 * A base class for COMMENT ON TABLE/VIEW.
 */
public abstract class CommentOnTableOrViewStmt extends CommentOnStmt
    implements SingleTableStmt {
  protected TableName tableName_;

  public CommentOnTableOrViewStmt(TableName tableName, String comment) {
    super(comment);
    Preconditions.checkArgument(tableName != null && !tableName.isEmpty());
    tableName_ = tableName;
  }

  @Override
  public TableName getTableName() { return tableName_;}

  @Override
  public void collectTableRefs(List<TableRef> tblRefs) {
    tblRefs.add(new TableRef(tableName_.toPath(), null));
  }

  @Override
  public void analyze(Analyzer analyzer) throws AnalysisException {
    super.analyze(analyzer);
    tableName_ = analyzer.getFqTableName(tableName_);
    TableRef tableRef = new TableRef(tableName_.toPath(), null, Privilege.ALTER);
    tableRef = analyzer.resolveTableRef(tableRef);
    Preconditions.checkNotNull(tableRef);
    tableRef.analyze(analyzer);
    validateType(tableRef);
  }

  /**
   * Validates the type of the given TableRef.
   */
  protected abstract void validateType(TableRef tableRef) throws AnalysisException;

  @Override
  public TCommentOnParams toThrift() {
    TCommentOnParams params = super.toThrift();
    params.setTable_name(tableName_.toThrift());
    return params;
  }
}
