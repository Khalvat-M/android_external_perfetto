/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SRC_TRACE_PROCESSOR_DB_COLUMN_H_
#define SRC_TRACE_PROCESSOR_DB_COLUMN_H_

#include <stdint.h>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/optional.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/db/row_map.h"
#include "src/trace_processor/db/sparse_vector.h"
#include "src/trace_processor/string_pool.h"

namespace perfetto {
namespace trace_processor {

// Represents the possible filter operations on a column.
enum class FilterOp {
  kEq,
  kNe,
  kGt,
  kLt,
  kGe,
  kLe,
  kIsNull,
  kIsNotNull,
  kLike,
};

// Represents a constraint on a column.
struct Constraint {
  uint32_t col_idx;
  FilterOp op;
  SqlValue value;
};

// Represents an order by operation on a column.
struct Order {
  uint32_t col_idx;
  bool desc;
};

// Represents a column which is to be joined on.
struct JoinKey {
  uint32_t col_idx;
};

class Table;

// Represents a named, strongly typed list of data.
class Column {
 public:
  // Flags which indicate properties of the data in the column. These features
  // are used to speed up column methods like filtering/sorting.
  enum Flag : uint32_t {
    // Indicates that this column has no special properties.
    kNoFlag = 0,

    // Indicates the data in the column is sorted. This can be used to speed
    // up filtering and skip sorting.
    kSorted = 1 << 0,

    // Indicates the data in the column is non-null. That is, the SparseVector
    // passed in will never have any null entries. This is only used for
    // numeric columns (string columns and id columns both have special
    // handling which ignores this flag).
    //
    // This is used to speed up filters as we can safely index SparseVector
    // directly if this flag is set.
    kNonNull = 1 << 1,
  };

  template <typename T>
  Column(const char* name,
         SparseVector<T>* storage,
         /* Flag */ uint32_t flags,
         Table* table,
         uint32_t col_idx,
         uint32_t row_map_idx)
      : Column(name,
               ToColumnType<T>(),
               flags,
               table,
               col_idx,
               row_map_idx,
               storage) {}

  // Create a Column has the same name and is backed by the same data as
  // |column| but is associated to a different table.
  Column(const Column& column,
         Table* table,
         uint32_t col_idx,
         uint32_t row_map_idx);

  // Columns are movable but not copyable.
  Column(Column&&) noexcept = default;
  Column& operator=(Column&&) = default;

  // Creates a Column which returns the index as the value of the row.
  static Column IdColumn(Table* table, uint32_t col_idx, uint32_t row_map_idx);

  // Gets the value of the Column at the given |row|.
  SqlValue Get(uint32_t row) const { return GetAtIdx(row_map().Get(row)); }

  // Returns the row containing the given value in the Column.
  base::Optional<uint32_t> IndexOf(SqlValue value) const {
    switch (type_) {
      // TODO(lalitm): investigate whether we could make this more efficient
      // by first checking the type of the column and comparing explicitly
      // based on that type.
      case ColumnType::kInt32:
      case ColumnType::kUint32:
      case ColumnType::kInt64:
      case ColumnType::kString: {
        for (uint32_t i = 0; i < row_map().size(); i++) {
          if (Get(i) == value)
            return i;
        }
        return base::nullopt;
      }
      case ColumnType::kId: {
        if (value.type != SqlValue::Type::kLong)
          return base::nullopt;
        return row_map().IndexOf(static_cast<uint32_t>(value.long_value));
      }
    }
    PERFETTO_FATAL("For GCC");
  }

  // Sorts |idx| in ascending or descending order (determined by |desc|) based
  // on the contents of this column.
  void StableSort(bool desc, std::vector<uint32_t>* idx) const {
    if (desc) {
      StableSort<true /* desc */>(idx);
    } else {
      StableSort<false /* desc */>(idx);
    }
  }

  // Updates the given RowMap by only keeping rows where this column meets the
  // given filter constraint.
  void FilterInto(FilterOp op, SqlValue value, RowMap* rm) const {
    if (IsId() && op == FilterOp::kEq) {
      // If this is an equality constraint on an id column, try and find the
      // single row with the id (if it exists).
      auto opt_idx = IndexOf(value);
      if (opt_idx) {
        rm->Intersect(RowMap::SingleRow(*opt_idx));
      } else {
        rm->Intersect(RowMap());
      }
      return;
    }

    if (IsSorted() && value.type == type()) {
      // If the column is sorted and the value has the same type as the column,
      // we should be able to just do a binary search to find the range of rows
      // instead of a full table scan.
      const Iterator b(this, 0);
      const Iterator e(this, row_map().size());
      switch (op) {
        case FilterOp::kEq: {
          uint32_t beg = std::distance(b, std::lower_bound(b, e, value));
          uint32_t end = std::distance(b, std::upper_bound(b, e, value));
          rm->Intersect(RowMap(beg, end));
          return;
        }
        case FilterOp::kLe: {
          uint32_t end = std::distance(b, std::upper_bound(b, e, value));
          rm->Intersect(RowMap(0, end));
          return;
        }
        case FilterOp::kLt: {
          uint32_t end = std::distance(b, std::lower_bound(b, e, value));
          rm->Intersect(RowMap(0, end));
          return;
        }
        case FilterOp::kGe: {
          uint32_t beg = std::distance(b, std::lower_bound(b, e, value));
          rm->Intersect(RowMap(beg, row_map().size()));
          return;
        }
        case FilterOp::kGt: {
          uint32_t beg = std::distance(b, std::upper_bound(b, e, value));
          rm->Intersect(RowMap(beg, row_map().size()));
          return;
        }
        case FilterOp::kNe:
        case FilterOp::kIsNull:
        case FilterOp::kIsNotNull:
        case FilterOp::kLike:
          break;
      }
    }

    switch (type_) {
      case ColumnType::kInt32: {
        if (IsNullable()) {
          FilterIntoLongSlow<int32_t, true /* is_nullable */>(op, value, rm);
        } else {
          FilterIntoLongSlow<int32_t, false /* is_nullable */>(op, value, rm);
        }
        break;
      }
      case ColumnType::kUint32: {
        if (IsNullable()) {
          FilterIntoLongSlow<uint32_t, true /* is_nullable */>(op, value, rm);
        } else {
          FilterIntoLongSlow<uint32_t, false /* is_nullable */>(op, value, rm);
        }
        break;
      }
      case ColumnType::kInt64: {
        if (IsNullable()) {
          FilterIntoLongSlow<int64_t, true /* is_nullable */>(op, value, rm);
        } else {
          FilterIntoLongSlow<int64_t, false /* is_nullable */>(op, value, rm);
        }
        break;
      }
      case ColumnType::kString: {
        FilterIntoStringSlow(op, value, rm);
        break;
      }
      case ColumnType::kId: {
        FilterIntoIdSlow(op, value, rm);
        break;
      }
    }
  }

  // Returns true if this column is considered an id column.
  bool IsId() const { return type_ == ColumnType::kId; }

  // Returns true if this column is a nullable column.
  bool IsNullable() const { return (flags_ & Flag::kNonNull) == 0; }

  // Returns true if this column is a sorted column.
  bool IsSorted() const { return (flags_ & Flag::kSorted) != 0; }

  const RowMap& row_map() const;
  const char* name() const { return name_; }
  SqlValue::Type type() const {
    switch (type_) {
      case ColumnType::kInt32:
      case ColumnType::kUint32:
      case ColumnType::kInt64:
      case ColumnType::kId:
        return SqlValue::Type::kLong;
      case ColumnType::kString:
        return SqlValue::Type::kString;
    }
    PERFETTO_FATAL("For GCC");
  }

  // Returns a Constraint for each type of filter operation for this Column.
  Constraint eq(SqlValue value) const {
    return Constraint{col_idx_, FilterOp::kEq, value};
  }
  Constraint gt(SqlValue value) const {
    return Constraint{col_idx_, FilterOp::kGt, value};
  }
  Constraint lt(SqlValue value) const {
    return Constraint{col_idx_, FilterOp::kLt, value};
  }
  Constraint ne(SqlValue value) const {
    return Constraint{col_idx_, FilterOp::kNe, value};
  }
  Constraint ge(SqlValue value) const {
    return Constraint{col_idx_, FilterOp::kGe, value};
  }
  Constraint le(SqlValue value) const {
    return Constraint{col_idx_, FilterOp::kLe, value};
  }
  Constraint is_not_null() const {
    return Constraint{col_idx_, FilterOp::kIsNotNull, SqlValue()};
  }
  Constraint is_null() const {
    return Constraint{col_idx_, FilterOp::kIsNull, SqlValue()};
  }

  // Returns an Order for each Order type for this Column.
  Order ascending() const { return Order{col_idx_, false}; }
  Order descending() const { return Order{col_idx_, true}; }

  // Returns the JoinKey for this Column.
  JoinKey join_key() const { return JoinKey{col_idx_}; }

 protected:
  NullTermStringView GetStringPoolStringAtIdx(uint32_t idx) const {
    return string_pool_->Get(sparse_vector<StringPool::Id>().GetNonNull(idx));
  }

  template <typename T>
  SparseVector<T>* mutable_sparse_vector() {
    PERFETTO_DCHECK(ToColumnType<T>() == type_);
    return static_cast<SparseVector<T>*>(sparse_vector_);
  }

  template <typename T>
  const SparseVector<T>& sparse_vector() const {
    PERFETTO_DCHECK(ToColumnType<T>() == type_);
    return *static_cast<const SparseVector<T>*>(sparse_vector_);
  }

 private:
  enum class ColumnType {
    // Standard primitive types.
    kInt32,
    kUint32,
    kInt64,
    kString,

    // Types generated on the fly.
    kId,
  };

  // Iterator over a column which conforms to std iterator interface
  // to allow using std algorithms (e.g. upper_bound, lower_bound etc.).
  class Iterator {
   public:
    using iterator_category = std::random_access_iterator_tag;
    using value_type = SqlValue;
    using difference_type = uint32_t;
    using pointer = uint32_t*;
    using reference = uint32_t&;

    Iterator(const Column* col, uint32_t row) : col_(col), row_(row) {}

    Iterator(const Iterator&) = default;
    Iterator& operator=(const Iterator&) = default;

    bool operator==(const Iterator& other) const { return other.row_ == row_; }
    bool operator!=(const Iterator& other) const { return !(*this == other); }
    bool operator<(const Iterator& other) const { return other.row_ < row_; }
    bool operator>(const Iterator& other) const { return other < *this; }
    bool operator<=(const Iterator& other) const { return !(other < *this); }
    bool operator>=(const Iterator& other) const { return !(*this < other); }

    SqlValue operator*() const { return col_->Get(row_); }
    Iterator& operator++() {
      row_++;
      return *this;
    }
    Iterator& operator--() {
      row_--;
      return *this;
    }

    Iterator& operator+=(uint32_t diff) {
      row_ += diff;
      return *this;
    }
    uint32_t operator-(const Iterator& other) { return row_ - other.row_; }

   private:
    const Column* col_ = nullptr;
    uint32_t row_ = 0;
  };

  friend class Table;

  Column(const char* name,
         ColumnType type,
         uint32_t flags,
         Table* table,
         uint32_t col_idx,
         uint32_t row_map_idx,
         void* sparse_vector);

  Column(const Column&) = delete;
  Column& operator=(const Column&) = delete;

  // Gets the value of the Column at the given |row|.
  SqlValue GetAtIdx(uint32_t idx) const {
    switch (type_) {
      case ColumnType::kInt32: {
        auto opt_value = sparse_vector<int32_t>().Get(idx);
        return opt_value ? SqlValue::Long(*opt_value) : SqlValue();
      }
      case ColumnType::kUint32: {
        auto opt_value = sparse_vector<uint32_t>().Get(idx);
        return opt_value ? SqlValue::Long(*opt_value) : SqlValue();
      }
      case ColumnType::kInt64: {
        auto opt_value = sparse_vector<int64_t>().Get(idx);
        return opt_value ? SqlValue::Long(*opt_value) : SqlValue();
      }
      case ColumnType::kString: {
        auto str = GetStringPoolStringAtIdx(idx).c_str();
        return str == nullptr ? SqlValue() : SqlValue::String(str);
      }
      case ColumnType::kId:
        return SqlValue::Long(idx);
    }
    PERFETTO_FATAL("For GCC");
  }

  template <typename T, bool is_nullable>
  void FilterIntoLongSlow(FilterOp op, SqlValue value, RowMap* rm) const {
    if (op == FilterOp::kIsNull) {
      PERFETTO_DCHECK(value.is_null());
      if (is_nullable) {
        row_map().FilterInto(rm, [this](uint32_t row) {
          return !sparse_vector<T>().Get(row).has_value();
        });
      } else {
        rm->Intersect(RowMap());
      }
      return;
    } else if (op == FilterOp::kIsNotNull) {
      PERFETTO_DCHECK(value.is_null());
      if (is_nullable) {
        row_map().FilterInto(rm, [this](uint32_t row) {
          return sparse_vector<T>().Get(row).has_value();
        });
      }
      return;
    }

    int64_t long_value = value.long_value;
    switch (op) {
      case FilterOp::kLt:
        row_map().FilterInto(rm, [this, long_value](uint32_t idx) {
          if (is_nullable)
            return sparse_vector<T>().Get(idx) < long_value;
          return sparse_vector<T>().GetNonNull(idx) < long_value;
        });
        break;
      case FilterOp::kEq:
        row_map().FilterInto(rm, [this, long_value](uint32_t idx) {
          if (is_nullable)
            return sparse_vector<T>().Get(idx) == long_value;
          return sparse_vector<T>().GetNonNull(idx) == long_value;
        });
        break;
      case FilterOp::kGt:
        row_map().FilterInto(rm, [this, long_value](uint32_t idx) {
          if (is_nullable)
            return sparse_vector<T>().Get(idx) > long_value;
          return sparse_vector<T>().GetNonNull(idx) > long_value;
        });
        break;
      case FilterOp::kNe:
        row_map().FilterInto(rm, [this, long_value](uint32_t idx) {
          if (is_nullable)
            return sparse_vector<T>().GetNonNull(idx) != long_value;
          return sparse_vector<T>().Get(idx) != long_value;
        });
        break;
      case FilterOp::kLe:
        row_map().FilterInto(rm, [this, long_value](uint32_t idx) {
          if (is_nullable)
            return sparse_vector<T>().GetNonNull(idx) <= long_value;
          return sparse_vector<T>().Get(idx) <= long_value;
        });
        break;
      case FilterOp::kGe:
        row_map().FilterInto(rm, [this, long_value](uint32_t idx) {
          if (is_nullable)
            return sparse_vector<T>().GetNonNull(idx) >= long_value;
          return sparse_vector<T>().Get(idx) >= long_value;
        });
        break;
      case FilterOp::kLike:
        rm->Intersect(RowMap());
        break;
      case FilterOp::kIsNull:
      case FilterOp::kIsNotNull:
        PERFETTO_FATAL("Should be handled above");
    }
  }

  void FilterIntoStringSlow(FilterOp op, SqlValue value, RowMap* rm) const {
    if (op == FilterOp::kIsNull) {
      PERFETTO_DCHECK(value.is_null());
      row_map().FilterInto(rm, [this](uint32_t row) {
        return GetStringPoolStringAtIdx(row).data() == nullptr;
      });
      return;
    } else if (op == FilterOp::kIsNotNull) {
      PERFETTO_DCHECK(value.is_null());
      row_map().FilterInto(rm, [this](uint32_t row) {
        return GetStringPoolStringAtIdx(row).data() != nullptr;
      });
      return;
    }

    NullTermStringView str_value = value.string_value;
    switch (op) {
      case FilterOp::kLt:
        row_map().FilterInto(rm, [this, str_value](uint32_t idx) {
          return GetStringPoolStringAtIdx(idx) < str_value;
        });
        break;
      case FilterOp::kEq:
        row_map().FilterInto(rm, [this, str_value](uint32_t idx) {
          return GetStringPoolStringAtIdx(idx) == str_value;
        });
        break;
      case FilterOp::kGt:
        row_map().FilterInto(rm, [this, str_value](uint32_t idx) {
          return GetStringPoolStringAtIdx(idx) > str_value;
        });
        break;
      case FilterOp::kNe:
        row_map().FilterInto(rm, [this, str_value](uint32_t idx) {
          return GetStringPoolStringAtIdx(idx) != str_value;
        });
        break;
      case FilterOp::kLe:
        row_map().FilterInto(rm, [this, str_value](uint32_t idx) {
          return GetStringPoolStringAtIdx(idx) <= str_value;
        });
        break;
      case FilterOp::kGe:
        row_map().FilterInto(rm, [this, str_value](uint32_t idx) {
          return GetStringPoolStringAtIdx(idx) >= str_value;
        });
        break;
      case FilterOp::kLike:
        // TODO(lalitm): either call through to SQLite or reimplement
        // like ourselves.
        PERFETTO_DLOG("Ignoring like constraint on string column");
        break;
      case FilterOp::kIsNull:
      case FilterOp::kIsNotNull:
        PERFETTO_FATAL("Should be handled above");
    }
  }

  void FilterIntoIdSlow(FilterOp op, SqlValue value, RowMap* rm) const {
    if (op == FilterOp::kIsNull) {
      PERFETTO_DCHECK(value.is_null());
      row_map().FilterInto(rm, [](uint32_t) { return false; });
      return;
    } else if (op == FilterOp::kIsNotNull) {
      PERFETTO_DCHECK(value.is_null());
      row_map().FilterInto(rm, [](uint32_t) { return true; });
      return;
    }

    uint32_t id_value = static_cast<uint32_t>(value.long_value);
    switch (op) {
      case FilterOp::kLt:
        row_map().FilterInto(
            rm, [id_value](uint32_t idx) { return idx < id_value; });
        break;
      case FilterOp::kEq:
        row_map().FilterInto(
            rm, [id_value](uint32_t idx) { return idx == id_value; });
        break;
      case FilterOp::kGt:
        row_map().FilterInto(
            rm, [id_value](uint32_t idx) { return idx > id_value; });
        break;
      case FilterOp::kNe:
        row_map().FilterInto(
            rm, [id_value](uint32_t idx) { return idx != id_value; });
        break;
      case FilterOp::kLe:
        row_map().FilterInto(
            rm, [id_value](uint32_t idx) { return idx <= id_value; });
        break;
      case FilterOp::kGe:
        row_map().FilterInto(
            rm, [id_value](uint32_t idx) { return idx >= id_value; });
        break;
      case FilterOp::kLike:
        rm->Intersect(RowMap());
        break;
      case FilterOp::kIsNull:
      case FilterOp::kIsNotNull:
        PERFETTO_FATAL("Should be handled above");
    }
  }

  template <bool desc>
  void StableSort(std::vector<uint32_t>* out) const {
    switch (type_) {
      case ColumnType::kInt32: {
        if (IsNullable()) {
          StableSort<desc, int32_t, true /* is_nullable */>(out);
        } else {
          StableSort<desc, int32_t, false /* is_nullable */>(out);
        }
        break;
      }
      case ColumnType::kUint32: {
        if (IsNullable()) {
          StableSort<desc, uint32_t, true /* is_nullable */>(out);
        } else {
          StableSort<desc, uint32_t, false /* is_nullable */>(out);
        }
        break;
      }
      case ColumnType::kInt64: {
        if (IsNullable()) {
          StableSort<desc, int64_t, true /* is_nullable */>(out);
        } else {
          StableSort<desc, int64_t, false /* is_nullable */>(out);
        }
        break;
      }
      case ColumnType::kString: {
        row_map().StableSort(out, [this](uint32_t a_idx, uint32_t b_idx) {
          auto a_str = GetStringPoolStringAtIdx(a_idx);
          auto b_str = GetStringPoolStringAtIdx(b_idx);
          return desc ? b_str < a_str : a_str < b_str;
        });
        break;
      }
      case ColumnType::kId:
        row_map().StableSort(out, [](uint32_t a_idx, uint32_t b_idx) {
          return desc ? b_idx < a_idx : a_idx < b_idx;
        });
    }
  }

  template <bool desc, typename T, bool is_nullable>
  void StableSort(std::vector<uint32_t>* out) const {
    const auto& sv = sparse_vector<T>();
    row_map().StableSort(out, [&sv](uint32_t a_idx, uint32_t b_idx) {
      if (is_nullable) {
        auto a_val = sv.Get(a_idx);
        auto b_val = sv.Get(b_idx);
        return desc ? b_val < a_val : a_val < b_val;
      }
      auto a_val = sv.GetNonNull(a_idx);
      auto b_val = sv.GetNonNull(b_idx);
      return desc ? b_val < a_val : a_val < b_val;
    });
  }

  template <typename T>
  static ColumnType ToColumnType() {
    if (std::is_same<T, uint32_t>::value) {
      return ColumnType::kUint32;
    } else if (std::is_same<T, int64_t>::value) {
      return ColumnType::kInt64;
    } else if (std::is_same<T, int32_t>::value) {
      return ColumnType::kInt32;
    } else if (std::is_same<T, StringPool::Id>::value) {
      return ColumnType::kString;
    } else {
      PERFETTO_FATAL("Unsupported type of column");
    }
  }

  // type_ is used to cast sparse_vector_ to the correct type.
  ColumnType type_ = ColumnType::kInt64;
  void* sparse_vector_ = nullptr;

  const char* name_ = nullptr;
  uint32_t flags_ = Flag::kNoFlag;
  const Table* table_ = nullptr;
  uint32_t col_idx_ = 0;
  uint32_t row_map_idx_ = 0;
  const StringPool* string_pool_ = nullptr;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_DB_COLUMN_H_
