#include "storage/table_heap.h"

/**
 * TODO: Student Implement
 */
//向堆表中插入一条记录，插入记录后生成的RowId需要通过row对象返回（即row.rid_）
bool TableHeap::InsertTuple(Row &row, Transaction *txn)
{
  //检查序列化大小是否超过最大行大小
  uint32_t serialized_size = row.GetSerializedSize(schema_);
  if (serialized_size > TablePage::SIZE_MAX_ROW)
    return false;

  //获取第一个页面
  auto cur_page = static_cast<TablePage *>(buffer_pool_manager_->FetchPage(first_page_id_));
  if (cur_page == nullptr)
    return false;

  //查找合适的页面来插入元组
  while (true)
  {
    if (cur_page->InsertTuple(row, schema_, txn, lock_manager_, log_manager_)) {
      //元组插入成功，更新页面并返回 true
      buffer_pool_manager_->UnpinPage(cur_page->GetTablePageId(), true);
      return true;
    }

    //获取下一页的页面 ID
    page_id_t next_page_id = cur_page->GetNextPageId();

    if (next_page_id != INVALID_PAGE_ID)
    {
      //如果下一页有效，则获取下一页
      buffer_pool_manager_->UnpinPage(cur_page->GetTablePageId(), false);
      cur_page = static_cast<TablePage *>(buffer_pool_manager_->FetchPage(next_page_id));
    }
    else
    {
      //如果下一页无效，则创建新页面
      auto *new_page = static_cast<TablePage *>(buffer_pool_manager_->NewPage(next_page_id));
      if (new_page == nullptr)
      {
        buffer_pool_manager_->UnpinPage(cur_page->GetTablePageId(), false);
        return false;
      }

      //初始化新页面并更新当前页面下一页 ID
      new_page->Init(next_page_id, cur_page->GetTablePageId(), log_manager_, txn);
      cur_page->SetNextPageId(next_page_id);

      buffer_pool_manager_->UnpinPage(cur_page->GetTablePageId(), true);
      cur_page = new_page;
    }
  }
}

bool TableHeap::MarkDelete(const RowId &rid, Transaction *txn)
{
  //获取包含该元组的页
  auto page = reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(rid.GetPageId()));
  //如果找不到包含该元组的页，则终止当前事务
  if (page == nullptr)
    return false;
  //标记该元组为已被删除
  page->WLatch();
  page->MarkDelete(rid, txn, lock_manager_, log_manager_);
  page->WUnlatch();
  buffer_pool_manager_->UnpinPage(page->GetTablePageId(), true);
  return true;
}

void TableHeap::FreeHeap()
{
  page_id_t cur_page_id = first_page_id_;

  while (cur_page_id != INVALID_PAGE_ID)
  {
    auto cur_page = (TablePage *)buffer_pool_manager_->FetchPage(cur_page_id);
    ASSERT(cur_page != nullptr, "Can not free an empty mem heap.");

    page_id_t next_page_id = cur_page->GetNextPageId();
    buffer_pool_manager_->DeletePage(cur_page_id);

    cur_page_id = next_page_id;
  }
}


/**
 * TODO: Student Implement
 */
//将RowId为rid的记录old_row替换成新的记录new_row，并将new_row的RowId通过new_row.rid_返回
bool TableHeap::UpdateTuple(const Row &row, const RowId &rid, Transaction *txn)
{
  auto page = (TablePage *)buffer_pool_manager_->FetchPage(rid.GetPageId());

  if (page == nullptr)
    return false;

  Row old_row_(row);
  TablePage::RetState ret_state = page->UpdateTuple(row, &old_row_, schema_, txn, lock_manager_, log_manager_);

  //如果返回状态是非法调用
  if (ret_state == TablePage::RetState::ILLEGAL_CALL)
  {
    buffer_pool_manager_->UnpinPage(page->GetTablePageId(), false);
    return false;
  }
  //如果返回状态是表页不足
  else if (ret_state == TablePage::RetState::INSUFFICIENT_TABLE_PAGE)
  {
    //对旧行执行删除
    MarkDelete(rid, txn);
    //执行插入操作
    InsertTuple(old_row_, txn);
    buffer_pool_manager_->UnpinPage(old_row_.GetRowId().GetPageId(), true);
    return true;
  }
  //如果返回状态是重复删除
  else if (ret_state == TablePage::RetState::DOUBLE_DELETE)
  {
    buffer_pool_manager_->UnpinPage(page->GetTablePageId(), false);
    return false;
  }
  //如果返回状态成功
  else if (ret_state == TablePage::RetState::SUCCESS)
  {
    buffer_pool_manager_->UnpinPage(page->GetTablePageId(), true);
    return true;
  }
  //其他情况
  else
  {
    return false;
  }
}



/**
 * TODO: Student Implement
 */
//从物理意义上删除这条记录
void TableHeap::ApplyDelete(const RowId &rid, Transaction *txn)
{
  //获取包含该元组的页
  auto page = reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(rid.GetPageId()));
  assert(page != nullptr);
  //删除该元组
  page->ApplyDelete(rid, txn, log_manager_);
  //Unpin该页
  buffer_pool_manager_->UnpinPage(rid.GetPageId(), true);
}

void TableHeap::RollbackDelete(const RowId &rid, Transaction *txn)
{
  //获取包含该元组的页
  auto page = reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(rid.GetPageId()));
  assert(page != nullptr);

  //回滚该删除操作
  page->WLatch();
  page->RollbackDelete(rid, txn, log_manager_);
  page->WUnlatch();
  buffer_pool_manager_->UnpinPage(page->GetTablePageId(), true);
}

/**
 * TODO: Student Implement
 */
//获取RowId为row->rid_的记录
bool TableHeap::GetTuple(Row *row, Transaction *txn)
{
  //获取包含该元组的页
  auto page = (TablePage *)buffer_pool_manager_->FetchPage(row->GetRowId().GetPageId());

  if (page == nullptr)
    return false;

  bool success = page->GetTuple(row, schema_, txn, lock_manager_);

  buffer_pool_manager_->UnpinPage(row->GetRowId().GetPageId(), false);

  return success;
}

void TableHeap::DeleteTable(page_id_t page_id)
{
  if (page_id != INVALID_PAGE_ID)
  {
    auto temp_table_page = reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(page_id));  // 删除table_heap

    if (temp_table_page->GetNextPageId() != INVALID_PAGE_ID)
      DeleteTable(temp_table_page->GetNextPageId());

    buffer_pool_manager_->UnpinPage(page_id, false);
    buffer_pool_manager_->DeletePage(page_id);
  }
  else
    DeleteTable(first_page_id_);
}

/**
 * TODO: Student Implement
 */
//获取堆表的首迭代器
TableIterator TableHeap::Begin(Transaction *txn)
{
  if (first_page_id_ == INVALID_PAGE_ID)
    //堆表为空，则返回一个的非法的构造器
    return TableIterator(this, INVALID_ROWID, txn);

  auto *first_page = (TablePage *)buffer_pool_manager_->FetchPage(first_page_id_);
  if (!first_page)
    //第一页不存在，则返回一个的非法的构造器
    return TableIterator(this, INVALID_ROWID, txn);


  RowId first_row_id;
  if (!first_page->GetFirstTupleRid(&first_row_id))
  {
    //第一页为空，则返回一个的非法的构造器
    buffer_pool_manager_->UnpinPage(first_page_id_, false);
    return TableIterator(this, INVALID_ROWID, txn);
  }

  //在操作结束后，unpin该页
  buffer_pool_manager_->UnpinPage(first_page_id_, false);

  return TableIterator(this, first_row_id, txn);
}

/**
 * TODO: Student Implement
 */
//获取堆表的尾迭代器
TableIterator TableHeap::End()
{
  return TableIterator(this, RowId(INVALID_PAGE_ID, 0), nullptr);
}
