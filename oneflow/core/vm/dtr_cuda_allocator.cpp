/*
Copyright 2020 The OneFlow Authors. All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#ifdef WITH_CUDA

#include "oneflow/core/vm/dtr_cuda_allocator.h"
#include "oneflow/core/device/cuda_util.h"
#include "oneflow/core/framework/tensor_pool.h"
#include "oneflow/core/job/env_global_objects_scope.h"
#include "oneflow/core/job/job_build_and_infer_ctx_mgr.h"
#include "oneflow/core/eager/eager_blob_object.h"
#include <iostream>

namespace oneflow {
namespace vm {

namespace {

inline size_t CudaMemAlignedBytes(size_t bytes) { return RoundUp(bytes, kCudaMemAllocAlignSize); }

inline bool IsAlignedSize(size_t size) { return size % kCudaMemAllocAlignSize == 0; }

static const size_t kPieceSplitThreshold = 128 << 20;  // 128MiB

inline double bytes2Mb(size_t bytes) { return bytes * 1. / 1024 / 1024; }

}  // namespace

DtrCudaAllocator::DtrCudaAllocator(int64_t device_id)
    : Allocator(), device_id_(device_id), total_memory_bytes_(0) {
  bins_.resize(kBinNumSize);
  for (int i = 0; i < kBinNumSize; ++i) {
    size_t bin_size = BinSize4BinNum(i);
    bins_.at(i).size = bin_size;
    CHECK_EQ(BinNum4BinSize(bin_size), i);
    CHECK_EQ(BinNum4BinSize(bin_size + kCudaMemAllocAlignSize - 1), i);
    CHECK_EQ(BinNum4BinSize(bin_size * 2 - 1), i);
    CHECK_EQ(BinNum4BinSize(bin_size * 2), i == (kBinNumSize - 1) ? i : i + 1);
  }
}

DtrCudaAllocator::~DtrCudaAllocator() {
  cudaSetDevice(device_id_);
  if (memory_ != nullptr) { OF_CUDA_CHECK(cudaFree(memory_)); }
}

void DtrCudaAllocator::Mark(DTREagerBlobObject* ebo, char* mem_ptr) {
  if (oneflow::DTRDebugEnabled()) { LOG(INFO) << "mark " << ebo << " " << (void*)mem_ptr; }
  Piece* piece = ptr2piece_.at(mem_ptr);
  piece->tensor = ebo;
}

void DtrCudaAllocator::InsertPiece2Bin(Piece* piece) {
  CHECK(piece->is_free && piece->bin_num == kInvalidBinNum);
  int32_t bin_num = BinNum4BinSize(piece->size);
  piece->bin_num = bin_num;
  CHECK(bins_.at(bin_num).pieces.insert(piece).second);
  piece_ordered_by_ptr.insert(piece);
}

void DtrCudaAllocator::RemovePieceFromBin(Piece* piece) {
  CHECK(piece->is_free);
  CHECK_NE(piece->bin_num, kInvalidBinNum);
  CHECK_GT(bins_.at(piece->bin_num).pieces.erase(piece), 0);
  piece->bin_num = kInvalidBinNum;
  piece_ordered_by_ptr.erase(piece);
}

DtrCudaAllocator::Piece* DtrCudaAllocator::AllocatePiece() {
  Piece* piece = new Piece();
  return piece;
}

void DtrCudaAllocator::DeallocatePiece(Piece* piece) {
  free(piece);
}

void DtrCudaAllocator::MarkPiece(Piece* piece) {
  CHECK_NOTNULL(piece->ptr);
  CHECK(ptr2piece_.emplace(piece->ptr, piece).second);
}
void DtrCudaAllocator::UnMarkPiece(Piece* piece) {
  CHECK_NOTNULL(piece->ptr);
  auto it = ptr2piece_.find(piece->ptr);
  CHECK(it != ptr2piece_.end());
  ptr2piece_.erase(it);
}

void DtrCudaAllocator::DisplayAllPieces() {
  for (const auto& pair : ptr2piece_) {
    Piece* piece = pair.second;
    std::stringstream ss;
    ss << "piece " << piece << ", " << (void*)piece->ptr << ", " << piece->size << ", "
       << piece->bin_num;
    if (piece->tensor) {
      ss << ", tensor: " << piece->tensor
         << ", compute op: " << piece->tensor->compute_op_type_name();
    } else {
      ss << ", no tensor";
    }
    LOG(INFO) << ss.str();
  }
}

void DtrCudaAllocator::Display() {
  double total_free_piece_bytes = 0.;
  for (int32_t bin_num = 0; bin_num < kBinNumSize; ++bin_num) {
    Bin* bin = &bins_.at(bin_num);
    for (auto it = bin->pieces.begin(); it != bin->pieces.end(); ++it) {
      Piece* piece = *it;
      CHECK(piece->is_free);
      CHECK_NOTNULL(piece->ptr);
      CHECK_EQ(piece->bin_num, bin_num);
      CHECK(IsAlignedSize(piece->size));
      std::cout << "piece in bin " << bin_num << ", memory: " << piece->size * 1. / 1024 / 1024
                << "MB" << std::endl;
      total_free_piece_bytes += piece->size;
    }
  }
  std::cout << "total_free_piece_bytes: " << bytes2Mb(total_free_piece_bytes) << "MB"
            << ", total allocate bytes: " << bytes2Mb(total_allocate_bytes_) << "MB"
            << ", total deallocate bytes: " << bytes2Mb(total_deallocate_bytes_) << "MB"
            << ", total memory bytes: " << bytes2Mb(total_memory_bytes_) << "MB" << std::endl;
}

DtrCudaAllocator::Piece* DtrCudaAllocator::FindPiece(size_t aligned_size) {
  CHECK(IsAlignedSize(aligned_size));

  if (memory_ == nullptr) {
    const size_t size = oneflow::GetDTRMemoryThreshold();
    OF_CUDA_CHECK(cudaMalloc(&memory_, size));
    Piece* piece = AllocatePiece();
    piece->size = size;
    piece->ptr = static_cast<char*>(memory_);
    piece->prev = nullptr;
    piece->next = nullptr;
    piece->is_free = true;
    piece->tensor = nullptr;
    piece->bin_num = kInvalidBinNum;
    InsertPiece2Bin(piece);
    MarkPiece(piece);
  }

  for (int32_t bin_num = BinNum4BinSize(aligned_size); bin_num < kBinNumSize; ++bin_num) {
    Bin* bin = &bins_.at(bin_num);
    for (auto it = bin->pieces.begin(); it != bin->pieces.end(); ++it) {
      Piece* piece = *it;
      CHECK(piece->is_free);
      CHECK_NOTNULL(piece->ptr);
      CHECK_EQ(piece->bin_num, bin_num);
      CHECK(IsAlignedSize(piece->size));
      if (piece->size >= aligned_size) {
        bin->pieces.erase(it);

        if (piece->size == aligned_size) {
          piece->bin_num = kInvalidBinNum;
          piece->is_free = false;
        } else if (piece->size > aligned_size) {
          const std::string& name = Global<one::DTRTensorPool>::Get()->current_op_type_name();
          const bool choose_left = [&]() {
            if (std::getenv("OF_DTR_NLR") != nullptr) {
              std::vector<std::string> high_compute_cost_names{"conv2d", "conv_data_grad",
                                                               "conv_filter_grad"};
              if (std::find(high_compute_cost_names.cbegin(), high_compute_cost_names.cend(), name)
                  != high_compute_cost_names.cend()) {
                return true;
              }
              return false;

            } else {
              return left_;
            }
          }();
          if (choose_left) {
            if (oneflow::DTRDebugEnabled()) { LOG(INFO) << "left: " << name; }
            piece->bin_num = kInvalidBinNum;
            piece->is_free = false;

            Piece* new_piece = AllocatePiece();
            new_piece->ptr = piece->ptr + aligned_size;
            new_piece->size = piece->size - aligned_size;
            piece->size = aligned_size;

            Piece* next_p = piece->next;
            piece->next = new_piece;
            new_piece->prev = piece;
            new_piece->next = next_p;
            if (next_p != nullptr) { next_p->prev = new_piece; }

            new_piece->is_free = true;
            new_piece->bin_num = kInvalidBinNum;
            CHECK(IsAlignedSize(piece->size));
            CHECK(IsAlignedSize(new_piece->size));
            InsertPiece2Bin(new_piece);
            MarkPiece(new_piece);
          } else {
            // is right
            piece->bin_num = kInvalidBinNum;
            // piece is still free

            Piece* new_piece = AllocatePiece();
            new_piece->ptr = piece->ptr + piece->size - aligned_size;
            new_piece->size = aligned_size;
            piece->size -= aligned_size;

            Piece* next_p = piece->next;
            piece->next = new_piece;
            new_piece->prev = piece;
            new_piece->next = next_p;
            if (next_p != nullptr) { next_p->prev = new_piece; }

            new_piece->is_free = false;
            new_piece->bin_num = kInvalidBinNum;
            CHECK(IsAlignedSize(piece->size));
            CHECK(IsAlignedSize(new_piece->size));
            InsertPiece2Bin(piece);
            MarkPiece(new_piece);
            return new_piece;
          }
        }
        return piece;
      }
    }
  }
  return nullptr;
}

void DtrCudaAllocator::MergeNeighbourFreePiece(Piece* lhs, Piece* rhs) {
  CHECK(lhs->is_free);
  CHECK(rhs->is_free);
  CHECK(lhs->next == rhs);
  CHECK(lhs == rhs->prev);
  CHECK(lhs->ptr + lhs->size == rhs->ptr);

  lhs->size += rhs->size;
  lhs->next = rhs->next;
  if (rhs->next != nullptr) { rhs->next->prev = lhs; }
  UnMarkPiece(rhs);
  DeallocatePiece(rhs);
}

double get_cost(const vm::DTREagerBlobObject* ebo) {
  if (ebo == nullptr) { return 0.; }
  const double cost = CHECK_JUST(ebo->cost());
  CHECK(!isinf(cost));
  CHECK(!isnan(cost));
  return cost;
}

DtrCudaAllocator::Piece* DtrCudaAllocator::EvictAndFindPiece(size_t size) {
  if (oneflow::DTRDebugEnabled()) { LOG(INFO) << "size: " << size; }
  auto start = ptr2piece_.begin();
  auto end = ptr2piece_.begin();
  size_t total_size = 0;
  double cost = 0;
  double min_cost = std::numeric_limits<double>::max();
  auto min_start = start;
  auto min_end = start;
  while (end != ptr2piece_.end()) {
    if (total_size < size) {
      const auto* end_tensor = end->second->tensor;
      if (end_tensor != nullptr && (end_tensor->is_pinned() || !end_tensor->is_evictable())) {
        if (oneflow::DTRDebugEnabled()) {
          LOG(INFO) << "skip tensor: " << end_tensor
                    << ", size: " << end_tensor->blob_body_bytes_double() << ", compute op "
                    << end_tensor->compute_op_type_name();
          LOG(INFO) << "num_pinned: " << end_tensor->num_pinned()
                    << ", is_evictable: " << end_tensor->is_evictable();
        }
        end++;
        start = end;
        total_size = 0;
        cost = 0;
        continue;
      }
      total_size += end->second->size;
      cost += get_cost(end_tensor);
      end++;

      if (oneflow::DTRDebugEnabled()) {
        LOG(INFO) << "move end, compute op: "
                  << (end_tensor != nullptr ? end_tensor->compute_op_type_name() : "no tensor")
                  << ", size: " << end->second->size
                  << ", total_size: " << total_size << ", cost: " << cost;
      }
    } else {
      if (min_cost > cost) {
        min_cost = cost;
        min_start = start;
        min_end = end;
        if (oneflow::DTRDebugEnabled()) { LOG(INFO) << "record, min_cost: " << min_cost; }
      }
      const auto* start_tensor = start->second->tensor;
      total_size -= start->second->size;
      cost -= get_cost(start_tensor);
      if (oneflow::DTRDebugEnabled()) {
        LOG(INFO) << "move start, compute op: "
                  << (start_tensor != nullptr ? start_tensor->compute_op_type_name() : "no tensor")
                  << ", size: " << start->second->size
                  << ", total_size: " << total_size << ", cost: " << cost;
      }
      start++;
    }
  }
  // CHECK(min_end != start);
  size_t size2 = 0;
  // collect piece ptrs into a new container, because evict() will devalidate the iterators
  std::vector<Piece*> pieces_to_be_evicted;
  for (auto it = min_start; it != min_end; ++it) {
    Piece* piece = it->second;
    pieces_to_be_evicted.push_back(piece);
  }
  for (auto* piece : pieces_to_be_evicted) {
    if (oneflow::DTRDebugEnabled()) {
      LOG(INFO) << "release ebo: " << piece->tensor << " dptr: " << (void*)piece->ptr << ", size: " << piece->size
                << ", cost: " << get_cost(piece->tensor) << ", compute op: "
                << (piece->tensor != nullptr ? piece->tensor->compute_op_type_name() : "no tensor");
    }
    size2 += piece->size;
    if (piece->tensor != nullptr) { CHECK_JUST(piece->tensor->evict()); }
  }
  if (oneflow::DTRDebugEnabled()) { LOG(INFO) << "evict size: " << size2; }
  return FindPiece(size);
}

void DtrCudaAllocator::Allocate(char** mem_ptr, std::size_t size) {
  if (size == 0) {
    *mem_ptr = nullptr;
    return;
  }
  size_t aligned_size = CudaMemAlignedBytes(size);

  Piece* piece = FindPiece(aligned_size);

  if (piece == nullptr) { piece = EvictAndFindPiece(aligned_size); }

  if (piece == nullptr) {
    CHECK_JUST(Global<one::DTRTensorPool>::Get()->display2());
    Display();
  }

  CHECK(piece != nullptr) << "Error! : Out of memory when allocate size : " << size;
  CHECK_NOTNULL(piece->ptr);
  CHECK(ptr2piece_.find(piece->ptr) != ptr2piece_.end());
  *mem_ptr = piece->ptr;
  total_allocate_bytes_ += size;

  if (std::getenv("OF_DTR_LR") != nullptr) { left_ = !left_; }
  // if (oneflow::DTRDebugEnabled()) {
  //   std::cout << "aid " << id_ << ", allocate " << (size / 1024. / 1024.)
  //             << "MB, total mem: " << (total_memory_bytes_ / 1024. / 1024.)
  //             << "MB, total allocate bytes: " << (total_allocate_bytes_ / 1024. / 1024.)
  //             << std::endl;
  // }
}

void DtrCudaAllocator::Deallocate(char* mem_ptr, std::size_t size) {
  if (mem_ptr == nullptr) { return; }

  auto it = ptr2piece_.find(mem_ptr);
  CHECK(it != ptr2piece_.end()) << "Error! : Try deallocate mem_ptr non-existent. mem ptr = "
                                << mem_ptr << " size = " << size;
  Piece* piece = it->second;
  CHECK_NOTNULL(piece);
  CHECK_EQ(piece->ptr, mem_ptr);
  CHECK(!piece->is_free);

  piece->is_free = true;
  piece->tensor = nullptr;

  Piece* last_piece_insert_to_bin = piece;
  Piece* next_p = piece->next;
  Piece* prev_p = piece->prev;

  if (next_p != nullptr && next_p->is_free) {
    CHECK_EQ(next_p->ptr, piece->ptr + piece->size);
    RemovePieceFromBin(next_p);
    MergeNeighbourFreePiece(piece, next_p);
  }

  if (prev_p != nullptr && prev_p->is_free) {
    CHECK_EQ(piece->ptr, prev_p->ptr + prev_p->size);
    RemovePieceFromBin(prev_p);
    MergeNeighbourFreePiece(prev_p, piece);
    last_piece_insert_to_bin = prev_p;
  }
  InsertPiece2Bin(last_piece_insert_to_bin);
  total_deallocate_bytes_ += size;
  // if (oneflow::DTRDebugEnabled()) {
  //   std::cout << "id: " << id_ << "deallocate " << (size / 1024. / 1024.)
  //             << "MB, total deallocate bytes: " << (total_deallocate_bytes_ / 1024. / 1024.)
  //             << std::endl;
  // }
}

}  // namespace vm
}  // namespace oneflow

#endif
