#include "../binary_view.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/util/binary_view_util.h>
#include <gtest/gtest.h>

#include "antb1/common/narrow.h"
#include "antb1/exec/join_table.h"
#include "antb1/exec/memory_budget.h"
#include "antb1/exec/sort.h"

#include "../gather.h"
#include "exec_test_util.h"

namespace antb1::exec {
namespace {

using Values = std::vector<std::optional<std::string>>;
class BinaryViewTest : public testing::ExecTest {};

std::shared_ptr<arrow::Array> Binary(const Values& values,
                                     arrow::MemoryPool* pool = arrow::default_memory_pool()) {
  arrow::BinaryBuilder builder(pool);
  for (const auto& value : values) {
    const auto status = value.has_value() ? builder.Append(*value) : builder.AppendNull();
    EXPECT_TRUE(status.ok()) << status.ToString();
  }
  return builder.Finish().ValueOrDie();
}

std::shared_ptr<arrow::Array> Views(const std::shared_ptr<arrow::Array>& binary,
                                    arrow::MemoryPool* pool = arrow::default_memory_pool()) {
  const arrow::Array* source = binary.get();
  std::vector<JoinRowRef> refs;
  refs.reserve(Narrow<std::size_t>(binary->length()));
  for (int64_t row = 0; row < binary->length(); ++row) {
    refs.push_back({.row = Narrow<std::uint32_t>(row)});
  }
  return GatherBinaryViews<JoinRowRef>(std::span(&source, 1), refs, pool).ValueOrDie();
}

std::shared_ptr<arrow::RecordBatch> Batch(const std::shared_ptr<arrow::Array>& values) {
  return arrow::RecordBatch::Make(arrow::schema({arrow::field("payload", values->type())}),
                                  values->length(), {values});
}

void CheckValues(const arrow::Array& array, const Values& expected) {
  ASSERT_TRUE(array.ValidateFull().ok());
  ASSERT_EQ(array.length(), Narrow<int64_t>(expected.size()));
  for (std::size_t row = 0; row < expected.size(); ++row) {
    const auto i = Narrow<int64_t>(row);
    const auto& expected_value = expected[row];
    ASSERT_EQ(array.IsNull(i), !expected_value.has_value()) << row;
    if (expected_value.has_value()) {
      const auto actual = array.type_id() == arrow::Type::BINARY_VIEW
                              ? static_cast<const arrow::BinaryViewArray&>(array).GetView(i)
                              : static_cast<const arrow::BinaryArray&>(array).GetView(i);
      EXPECT_EQ(actual, *expected_value) << row;
    }
  }
}

TEST_F(BinaryViewTest, GatherPreservesBytesNullsSlicesMultipleChunksAndOwners) {
  MemoryBudget budget(std::nullopt);
  std::shared_ptr<arrow::Array> output;
  std::weak_ptr<arrow::Buffer> source_payload;
  const std::string twelve = std::string("bad\xff\0bytes!!", 12);
  const std::string thirteen = std::string("bad\xfe\0bytes!!!", 13);
  {
    const auto first = Binary({"prefix", "", std::nullopt, twelve, thirteen, "suffix"}, &budget);
    const auto sliced = first->Slice(1, 4);
    const auto second = Views(Binary({std::string(40, 'z'), "q"}, &budget), &budget);
    source_payload = first->data()->buffers[2];
    const std::vector<const arrow::Array*> arrays = {sliced.get(), second.get()};
    const std::vector<JoinRowRef> refs = {{.chunk = 0, .row = 3},          {.chunk = 1, .row = 0},
                                          {.chunk = 0, .row = 1},          {.chunk = 0, .row = 2},
                                          {.chunk = 0, .row = 0},          {.chunk = 1, .row = 1},
                                          {.chunk = kNoChunk, .row = 999}, {.chunk = 0, .row = 3}};
    const auto gathered = GatherBinaryViews<JoinRowRef>(arrays, refs, &budget);
    ASSERT_TRUE(gathered.ok()) << gathered.status().ToString();
    output = *gathered;
  }
  EXPECT_FALSE(source_payload.expired());
  EXPECT_EQ(output->type_id(), arrow::Type::BINARY_VIEW);
  CheckValues(*output, {thirteen, std::string(40, 'z'), std::nullopt, twelve, "", "q", std::nullopt,
                        thirteen});
  output.reset();
  EXPECT_TRUE(source_payload.expired());
  EXPECT_EQ(budget.bytes_allocated(), 0);
}

TEST_F(BinaryViewTest, SharedBinaryGatherVisitsSparseChunksInReferenceOrderAndOwnsOnlyLongValues) {
  const auto run = []<class Ref> {
    MemoryBudget budget(std::nullopt);
    std::shared_ptr<arrow::Array> output;
    std::shared_ptr<arrow::Array> generic;
    std::vector<std::weak_ptr<arrow::Buffer>> owners;
    std::weak_ptr<arrow::Buffer> aliased_parent;
    const std::string twelve("bad\xff\0bytes!!", 12);
    const std::string thirteen("bad\xfe\0bytes!!!", 13);
    const std::string other(81, 'q');
    {
      std::vector<std::shared_ptr<arrow::Array>> chunks;
      std::vector<const arrow::Array*> arrays;
      for (int chunk = 0; chunk < 64; ++chunk) {
        auto input =
            Binary({"prefix", chunk == 63 ? thirteen : other, twelve, std::nullopt, ""}, &budget);
        if (chunk == 63) {
          // The array slice has a nonzero row offset; its data buffer is itself a slice whose
          // local offsets differ from its allocating parent's offsets.
          const auto payload = input->data()->buffers[2];
          std::shared_ptr<arrow::Buffer> parent =
              arrow::AllocateBuffer(payload->size() + 64, &budget).ValueOrDie();
          std::memcpy(parent->mutable_data() + 17, payload->data(),
                      Narrow<std::size_t>(payload->size()));
          auto data = input->data()->Copy();
          data->buffers[2] = arrow::SliceBuffer(parent, 17, payload->size());
          input = arrow::MakeArray(data);
          aliased_parent = parent;
        }
        chunks.push_back(input->Slice(1));
        arrays.push_back(chunks.back().get());
        owners.push_back(chunks.back()->data()->buffers[2]);
      }
      const std::vector<Ref> refs = {{.chunk = 63, .row = 0},         {.chunk = 2, .row = 0},
                                     {.chunk = 63, .row = 0},         {.chunk = 31, .row = 1},
                                     {.chunk = 31, .row = 2},         {.chunk = 31, .row = 3},
                                     {.chunk = kNoChunk, .row = 999}, {.chunk = 63, .row = 1}};
      const auto gathered =
          GatherBinaryViews<Ref>(arrays, refs, &budget, BinaryViewRetention::kShare);
      ASSERT_TRUE(gathered.ok()) << gathered.status().ToString();
      output = *gathered;
      // An unused view array forces the generic sharing implementation on identical references.
      const auto unused_view = Views(Binary({}));
      arrays.push_back(unused_view.get());
      const auto reference =
          GatherBinaryViews<Ref>(arrays, refs, &budget, BinaryViewRetention::kShare);
      ASSERT_TRUE(reference.ok()) << reference.status().ToString();
      generic = *reference;
    }
    EXPECT_TRUE(output->Equals(*generic));
    generic.reset();
    ASSERT_EQ(output->data()->buffers.size(), 4);
    for (std::size_t chunk = 0; chunk < owners.size(); ++chunk) {
      EXPECT_EQ(owners[chunk].expired(), chunk != 2 && chunk != 63) << chunk;
    }
    EXPECT_FALSE(aliased_parent.expired());
    CheckValues(*output,
                {thirteen, other, thirteen, twelve, std::nullopt, "", std::nullopt, twelve});
    output.reset();
    EXPECT_TRUE(aliased_parent.expired());
    EXPECT_TRUE(std::ranges::all_of(owners, [](const auto& owner) { return owner.expired(); }));
    EXPECT_EQ(budget.bytes_allocated(), 0);
  };
  run.operator()<JoinRowRef>();
  run.operator()<SortBuffer::RowRef>();
}

TEST_F(BinaryViewTest, SharedBinaryGatherDoesNotCertifySparsePayloadAsBounded) {
  MemoryBudget budget(std::nullopt);
  std::shared_ptr<arrow::RecordBatch> compact;
  std::weak_ptr<arrow::Buffer> owner;
  const std::string kept(13, 'k');
  {
    const auto input = Binary({std::string(200000, 'x'), kept}, &budget);
    owner = input->data()->buffers[2];
    const arrow::Array* source = input.get();
    const std::vector<JoinRowRef> refs = {{.row = 1}};
    const auto shared = GatherBinaryViews<JoinRowRef>(std::span(&source, 1), refs, &budget,
                                                      BinaryViewRetention::kShare);
    ASSERT_TRUE(shared.ok()) << shared.status().ToString();
    const auto result = CompactBinaryViews(Batch(*shared), &budget);
    ASSERT_TRUE(result.ok()) << result.status().ToString();
    compact = *result;
    EXPECT_NE(compact->column(0)->data()->buffers[2], input->data()->buffers[2]);
  }
  EXPECT_TRUE(owner.expired());
  CheckValues(*compact->column(0), {kept});
  compact.reset();
  EXPECT_EQ(budget.bytes_allocated(), 0);
}

TEST_F(BinaryViewTest, InlineAndNullResultsRetainNoPayload) {
  MemoryBudget budget(std::nullopt);
  {
    const auto binary = Binary({std::string(200, 'x'), "inline", std::nullopt, ""}, &budget);
    const arrow::Array* source = binary.get();
    const std::vector<SortBuffer::RowRef> refs = {{.chunk = 0, .row = 1},
                                                  {.chunk = 0, .row = 2},
                                                  {.chunk = 0, .row = 3},
                                                  {.chunk = kNoChunk, .row = 0}};
    for (const auto retention : {BinaryViewRetention::kBounded, BinaryViewRetention::kShare,
                                 BinaryViewRetention::kHybrid}) {
      const auto gathered =
          GatherBinaryViews<SortBuffer::RowRef>(std::span(&source, 1), refs, &budget, retention);
      ASSERT_TRUE(gathered.ok()) << gathered.status().ToString();
      EXPECT_EQ((*gathered)->data()->buffers.size(), 2);
      CheckValues(**gathered, {"inline", std::nullopt, "", std::nullopt});
      const std::vector<JoinRowRef> none;
      const auto empty =
          GatherBinaryViews<JoinRowRef>(std::span(&source, 1), none, &budget, retention);
      ASSERT_TRUE(empty.ok());
      EXPECT_EQ((*empty)->length(), 0);
      EXPECT_TRUE((*empty)->ValidateFull().ok());
    }
  }
  EXPECT_EQ(budget.bytes_allocated(), 0);
}

TEST_F(BinaryViewTest, SparseAliasedBuffersCompactDistinctRangesInsteadOfFanoutBytes) {
  MemoryBudget budget(std::nullopt);
  std::shared_ptr<arrow::Array> output;
  std::weak_ptr<arrow::Buffer> original;
  {
    std::shared_ptr<arrow::Buffer> backing =
        arrow::AllocateBuffer(int64_t{1024} * 1024, &budget).ValueOrDie();
    std::fill_n(backing->mutable_data(), backing->size(), static_cast<std::uint8_t>('x'));
    original = backing;
    auto first_owner = arrow::SliceBuffer(backing, 100, 64);
    auto second_owner = arrow::SliceBuffer(backing, 132, 64);
    std::shared_ptr<arrow::Buffer> descriptors =
        arrow::AllocateBuffer(int64_t{2} * arrow::BinaryViewType::kSize, &budget).ValueOrDie();
    auto* views = descriptors->mutable_data_as<arrow::BinaryViewType::c_type>();
    views[0] = arrow::util::ToBinaryView(first_owner->data(), 64, 0, 0);
    views[1] = arrow::util::ToBinaryView(second_owner->data(), 64, 1, 0);
    const auto input = std::make_shared<arrow::BinaryViewArray>(
        arrow::binary_view(), 2, descriptors, arrow::BufferVector{first_owner, second_owner});
    ASSERT_TRUE(input->ValidateFull().ok());
    const arrow::Array* source = input.get();
    std::vector<JoinRowRef> refs(4096);
    for (std::size_t i = 0; i < refs.size(); ++i) {
      refs[i].row = Narrow<std::uint32_t>(i % 2);
    }
    const auto gathered = GatherBinaryViews<JoinRowRef>(std::span(&source, 1), refs, &budget);
    ASSERT_TRUE(gathered.ok()) << gathered.status().ToString();
    output = *gathered;
    ASSERT_EQ(output->data()->buffers.size(), 3);
    EXPECT_EQ(output->data()->buffers[2]->size(), 96);
  }
  EXPECT_TRUE(original.expired());
  ASSERT_TRUE(output->ValidateFull().ok());
  const auto& values = static_cast<const arrow::BinaryViewArray&>(*output);
  for (int64_t row = 0; row < values.length(); ++row) {
    EXPECT_EQ(values.GetView(row), std::string(64, 'x'));
  }
  output.reset();
  EXPECT_EQ(budget.bytes_allocated(), 0);
}

TEST_F(BinaryViewTest, BoundedArraysAreReusedButSlicesAreCompacted) {
  MemoryBudget budget(std::nullopt);
  {
    const auto binary = Binary(Values(1024, std::string(128, 'a')), &budget);
    const auto all = Batch(Views(binary, &budget));
    const int64_t before = budget.bytes_allocated();
    const auto unchanged = CompactBinaryViews(all, &budget);
    ASSERT_TRUE(unchanged.ok());
    EXPECT_EQ(*unchanged, all);
    EXPECT_EQ(budget.bytes_allocated(), before);
    const auto sliced = all->Slice(10, 1);
    const auto compact = CompactBinaryViews(sliced, &budget);
    ASSERT_TRUE(compact.ok()) << compact.status().ToString();
    EXPECT_NE(*compact, sliced);
    EXPECT_EQ((*compact)->column(0)->data()->buffers[2]->size(), 128);
    EXPECT_NE((*compact)->column(0)->data()->buffers[2], all->column(0)->data()->buffers[2]);
    CheckValues(*(*compact)->column(0), {std::string(128, 'a')});
  }
  EXPECT_EQ(budget.bytes_allocated(), 0);
}

TEST_F(BinaryViewTest, SharedSparseAliasesOwnSourcesWithoutClaimingBoundedRetention) {
  MemoryBudget budget(std::nullopt);
  std::shared_ptr<arrow::Array> output;
  std::weak_ptr<arrow::Buffer> backing_owner;
  {
    std::shared_ptr<arrow::Buffer> backing =
        arrow::AllocateBuffer(int64_t{1024} * 1024, &budget).ValueOrDie();
    std::fill_n(backing->mutable_data(), backing->size(), static_cast<std::uint8_t>('s'));
    backing_owner = backing;
    auto first = arrow::SliceBuffer(backing, 100, 64);
    auto second = arrow::SliceBuffer(backing, 132, 64);
    std::shared_ptr<arrow::Buffer> descriptors =
        arrow::AllocateBuffer(int64_t{2} * arrow::BinaryViewType::kSize, &budget).ValueOrDie();
    auto* views = descriptors->mutable_data_as<arrow::BinaryViewType::c_type>();
    views[0] = arrow::util::ToBinaryView(first->data(), 64, 0, 0);
    views[1] = arrow::util::ToBinaryView(second->data(), 64, 1, 0);
    const auto input = std::make_shared<arrow::BinaryViewArray>(
        arrow::binary_view(), 2, descriptors, arrow::BufferVector{first, second});
    const arrow::Array* source = input.get();
    const std::vector<JoinRowRef> refs = {{.row = 1}, {.row = 0}, {.row = 1}};
    const auto result = GatherBinaryViews<JoinRowRef>(std::span(&source, 1), refs, &budget,
                                                      BinaryViewRetention::kShare);
    ASSERT_TRUE(result.ok()) << result.status().ToString();
    output = *result;
    ASSERT_EQ(output->data()->buffers.size(), 4);
    EXPECT_EQ(output->data()->buffers[2], first);
    EXPECT_EQ(output->data()->buffers[3], second);
  }
  EXPECT_FALSE(backing_owner.expired());
  CheckValues(*output, Values(3, std::string(64, 's')));
  {
    const auto input = Batch(output);
    const auto bounded = CompactBinaryViews(input, &budget);
    ASSERT_TRUE(bounded.ok()) << bounded.status().ToString();
    EXPECT_NE(*bounded, input);
    EXPECT_EQ((*bounded)->column(0)->data()->buffers[2]->size(), 96);
    CheckValues(*(*bounded)->column(0), Values(3, std::string(64, 's')));
  }
  output.reset();
  EXPECT_TRUE(backing_owner.expired());
  EXPECT_EQ(budget.bytes_allocated(), 0);
}

TEST_F(BinaryViewTest, HybridFanoutSharesAtEqualityWithoutClaimingExactBound) {
  MemoryBudget budget(std::nullopt);
  std::shared_ptr<arrow::Array> output;
  std::weak_ptr<arrow::Buffer> backing;
  {
    const auto binary = Binary(Values(1024, std::string(128, 'd')), &budget);
    ASSERT_EQ(binary->data()->buffers[2]->capacity(), int64_t{1024} * 128);
    backing = binary->data()->buffers[2];
    const arrow::Array* source = binary.get();
    const std::vector<JoinRowRef> refs(1024, JoinRowRef{.row = 17});
    const auto result = GatherBinaryViews<JoinRowRef>(std::span(&source, 1), refs, &budget,
                                                      BinaryViewRetention::kHybrid);
    ASSERT_TRUE(result.ok()) << result.status().ToString();
    output = *result;
    ASSERT_EQ(output->data()->buffers.size(), 3);
    EXPECT_EQ(output->data()->buffers[2], binary->data()->buffers[2]);
    EXPECT_FALSE(output->data()->buffers[1]->is_mutable());
  }
  EXPECT_FALSE(backing.expired());
  CheckValues(*output, Values(1024, std::string(128, 'd')));
  {
    const auto batch = Batch(output);
    const auto retained = RetainBinaryViewPayload(batch, &budget);
    ASSERT_TRUE(retained.ok()) << retained.status().ToString();
    EXPECT_EQ((*retained)->column(0)->data()->buffers[2], output->data()->buffers[2]);
    const auto bounded = CompactBinaryViews(batch, &budget);
    ASSERT_TRUE(bounded.ok()) << bounded.status().ToString();
    EXPECT_NE(*bounded, batch);
    EXPECT_EQ((*bounded)->column(0)->data()->buffers[2]->size(), 128);
    EXPECT_NE((*bounded)->column(0)->data()->buffers[2], output->data()->buffers[2]);
  }
  output.reset();
  EXPECT_TRUE(backing.expired());
  EXPECT_EQ(budget.bytes_allocated(), 0);
}

TEST_F(BinaryViewTest, HybridSparseParentUsesFullCapacityAndReleasesAliases) {
  MemoryBudget budget(std::nullopt);
  std::shared_ptr<arrow::Array> output;
  std::weak_ptr<arrow::Buffer> backing;
  {
    auto allocated = arrow::AllocateResizableBuffer(128, &budget).ValueOrDie();
    ASSERT_TRUE(allocated->Reserve(4096).ok());
    ASSERT_EQ(allocated->size(), 128);
    ASSERT_GE(allocated->capacity(), 4096);
    std::fill_n(allocated->mutable_data(), allocated->size(), static_cast<std::uint8_t>('s'));
    std::shared_ptr<arrow::Buffer> root = std::move(allocated);
    backing = root;
    const auto middle = arrow::SliceBuffer(root, 16, 96);
    const auto alias = arrow::SliceBuffer(middle, 16, 64);
    std::shared_ptr<arrow::Buffer> descriptors =
        arrow::AllocateBuffer(arrow::BinaryViewType::kSize, &budget).ValueOrDie();
    descriptors->mutable_data_as<arrow::BinaryViewType::c_type>()[0] =
        arrow::util::ToBinaryView(alias->data(), 64, 0, 0);
    const auto input = std::make_shared<arrow::BinaryViewArray>(
        arrow::binary_view(), 1, descriptors, arrow::BufferVector{alias});
    const arrow::Array* source = input.get();
    const std::vector<JoinRowRef> refs(2, JoinRowRef{.row = 0});
    const auto result = GatherBinaryViews<JoinRowRef>(std::span(&source, 1), refs, &budget,
                                                      BinaryViewRetention::kHybrid);
    ASSERT_TRUE(result.ok()) << result.status().ToString();
    output = *result;
    ASSERT_EQ(output->data()->buffers.size(), 3);
    EXPECT_NE(output->data()->buffers[2], alias);
    EXPECT_EQ(output->data()->buffers[2]->size(), 128);
  }
  EXPECT_TRUE(backing.expired());
  CheckValues(*output, Values(2, std::string(64, 's')));
  output.reset();
  EXPECT_EQ(budget.bytes_allocated(), 0);
}

TEST_F(BinaryViewTest, HybridCombinesAliasedRootsAcrossChunksAndPreservesLocalOffsets) {
  MemoryBudget budget(std::nullopt);
  std::shared_ptr<arrow::Array> output;
  std::weak_ptr<arrow::Buffer> backing;
  {
    std::shared_ptr<arrow::Buffer> root = arrow::AllocateBuffer(128, &budget).ValueOrDie();
    ASSERT_EQ(root->capacity(), 128);
    std::fill_n(root->mutable_data(), root->size(), static_cast<std::uint8_t>('a'));
    backing = root;
    const auto first = arrow::SliceBuffer(root, 0, 64);
    const auto second = arrow::SliceBuffer(root, 64, 64);
    arrow::ArrayVector inputs;
    for (const auto& owner : {first, second}) {
      std::shared_ptr<arrow::Buffer> descriptors =
          arrow::AllocateBuffer(arrow::BinaryViewType::kSize, &budget).ValueOrDie();
      const int32_t offset = owner == first ? 16 : 8;
      descriptors->mutable_data_as<arrow::BinaryViewType::c_type>()[0] =
          arrow::util::ToBinaryView(owner->data() + offset, 32, 0, offset);
      inputs.push_back(std::make_shared<arrow::BinaryViewArray>(
          arrow::binary_view(), 1, descriptors, arrow::BufferVector{owner}));
    }
    const std::vector<const arrow::Array*> arrays = {inputs[0].get(), inputs[1].get()};
    const std::vector<JoinRowRef> refs = {{.chunk = 1, .row = 0},
                                          {.chunk = 0, .row = 0},
                                          {.chunk = 1, .row = 0},
                                          {.chunk = 0, .row = 0}};
    const auto result =
        GatherBinaryViews<JoinRowRef>(arrays, refs, &budget, BinaryViewRetention::kHybrid);
    ASSERT_TRUE(result.ok()) << result.status().ToString();
    output = *result;
    ASSERT_EQ(output->data()->buffers.size(), 4);
    EXPECT_EQ(output->data()->buffers[2], first);
    EXPECT_EQ(output->data()->buffers[3], second);
    const auto& values = static_cast<const arrow::BinaryViewArray&>(*output);
    EXPECT_EQ(values.raw_values()[0].ref.buffer_index, 1);
    EXPECT_EQ(values.raw_values()[0].ref.offset, 8);
    EXPECT_EQ(values.raw_values()[1].ref.buffer_index, 0);
    EXPECT_EQ(values.raw_values()[1].ref.offset, 16);
  }
  EXPECT_FALSE(backing.expired());
  CheckValues(*output, Values(4, std::string(32, 'a')));
  output.reset();
  EXPECT_TRUE(backing.expired());
  EXPECT_EQ(budget.bytes_allocated(), 0);
}

TEST_F(BinaryViewTest, HybridSharesAndCopiesDifferentRootsInOneOutput) {
  MemoryBudget budget(std::nullopt);
  std::shared_ptr<arrow::Array> output;
  std::weak_ptr<arrow::Buffer> compact_owner;
  std::weak_ptr<arrow::Buffer> sparse_owner;
  {
    const auto compact = Binary({std::string(32, 'c')}, &budget);
    const auto sparse = Binary({std::string(32, 's'), std::string(4096, 'x')}, &budget);
    ASSERT_EQ(compact->data()->buffers[2]->capacity(), 64);
    compact_owner = compact->data()->buffers[2];
    sparse_owner = sparse->data()->buffers[2];
    const std::vector<const arrow::Array*> arrays = {compact.get(), sparse.get()};
    const std::vector<JoinRowRef> refs = {{.chunk = 1, .row = 0}, {.chunk = 0, .row = 0},
                                          {.chunk = 0, .row = 0}, {.chunk = 1, .row = 0},
                                          {.chunk = 0, .row = 0}, {.chunk = 0, .row = 0}};
    const auto result =
        GatherBinaryViews<JoinRowRef>(arrays, refs, &budget, BinaryViewRetention::kHybrid);
    ASSERT_TRUE(result.ok()) << result.status().ToString();
    output = *result;
    ASSERT_EQ(output->data()->buffers.size(), 4);
    EXPECT_EQ(output->data()->buffers[2], compact->data()->buffers[2]);
    EXPECT_NE(output->data()->buffers[3], sparse->data()->buffers[2]);
    EXPECT_EQ(output->data()->buffers[3]->size(), 64);
    const auto& values = static_cast<const arrow::BinaryViewArray&>(*output);
    EXPECT_EQ(values.raw_values()[0].ref.buffer_index, 1);
    EXPECT_EQ(values.raw_values()[0].ref.offset, 0);
    EXPECT_EQ(values.raw_values()[3].ref.buffer_index, 1);
    EXPECT_EQ(values.raw_values()[3].ref.offset, 32);
  }
  EXPECT_FALSE(compact_owner.expired());
  EXPECT_TRUE(sparse_owner.expired());
  CheckValues(*output, {std::string(32, 's'), std::string(32, 'c'), std::string(32, 'c'),
                        std::string(32, 's'), std::string(32, 'c'), std::string(32, 'c')});
  output.reset();
  EXPECT_TRUE(compact_owner.expired());
  EXPECT_EQ(budget.bytes_allocated(), 0);
}

TEST_F(BinaryViewTest, RetentionPoliciesKeepByteSemanticsAcrossSlicesChunksAndPadding) {
  const std::string twelve("bad\xff\0bytes!!", 12);
  const std::string thirteen("bad\xfe\0bytes!!!", 13);
  for (const auto retention :
       {BinaryViewRetention::kShare, BinaryViewRetention::kCopy, BinaryViewRetention::kHybrid}) {
    MemoryBudget budget(std::nullopt);
    std::shared_ptr<arrow::Array> output;
    std::weak_ptr<arrow::Buffer> source_owner;
    {
      const auto first = Binary({"skip", "", std::nullopt, twelve, thirteen}, &budget);
      const auto slice = first->Slice(1);
      source_owner = first->data()->buffers[2];
      const auto second = Views(Binary({std::string(128, 'z'), "short"}, &budget), &budget);
      const std::vector<const arrow::Array*> arrays = {slice.get(), second.get()};
      const std::vector<SortBuffer::RowRef> refs = {
          {.chunk = 1, .row = 0}, {.chunk = 0, .row = 3}, {.chunk = 0, .row = 1},
          {.chunk = 0, .row = 0}, {.chunk = 0, .row = 2}, {.chunk = kNoChunk, .row = 999},
          {.chunk = 1, .row = 1}, {.chunk = 0, .row = 3}};
      const auto result = GatherBinaryViews<SortBuffer::RowRef>(arrays, refs, &budget, retention);
      ASSERT_TRUE(result.ok()) << result.status().ToString();
      output = *result;
    }
    EXPECT_EQ(source_owner.expired(), retention != BinaryViewRetention::kShare);
    CheckValues(*output, {std::string(128, 'z'), thirteen, std::nullopt, "", twelve, std::nullopt,
                          "short", thirteen});
    output.reset();
    EXPECT_TRUE(source_owner.expired());
    EXPECT_EQ(budget.bytes_allocated(), 0);
  }
}

TEST_F(BinaryViewTest, CopyPacksMultipleFreshPayloadBlocksAndReleasesPreviousOwners) {
  MemoryBudget budget(std::nullopt);
  std::shared_ptr<arrow::Array> output;
  std::weak_ptr<arrow::Buffer> source_owner;
  const std::string large(70000, 'l');
  const std::string medium(40000, 'm');
  const std::string long_value(13, 's');
  const Values expected = {large, medium, long_value, medium, long_value, large};
  {
    const auto input = Binary({large, medium, long_value}, &budget);
    source_owner = input->data()->buffers[2];
    const arrow::Array* source = input.get();
    const std::vector<JoinRowRef> refs = {{.row = 0}, {.row = 1}, {.row = 2},
                                          {.row = 1}, {.row = 2}, {.row = 0}};
    const auto result = GatherBinaryViews<JoinRowRef>(std::span(&source, 1), refs, &budget,
                                                      BinaryViewRetention::kCopy);
    ASSERT_TRUE(result.ok()) << result.status().ToString();
    output = *result;
  }
  EXPECT_TRUE(source_owner.expired());
  CheckValues(*output, expected);
  ASSERT_EQ(output->data()->buffers.size(), 6);
  EXPECT_EQ(output->data()->buffers[2]->size(), 70000);
  EXPECT_EQ(output->data()->buffers[3]->size(), 40013);
  EXPECT_EQ(output->data()->buffers[4]->size(), 40013);
  EXPECT_EQ(output->data()->buffers[5]->size(), 70000);
  std::weak_ptr<arrow::Buffer> first_copy = output->data()->buffers[2];
  {
    const auto input = Batch(output);
    EXPECT_EQ(CompactBinaryViews(input, &budget).ValueOrDie(), input);
    const auto copied = CopyBinaryViewPayload(input, &budget);
    ASSERT_TRUE(copied.ok()) << copied.status().ToString();
    EXPECT_NE((*copied)->column(0)->data()->buffers[2], output->data()->buffers[2]);
    output = (*copied)->column(0);
  }
  EXPECT_TRUE(first_copy.expired());
  CheckValues(*output, expected);
  output.reset();
  EXPECT_EQ(budget.bytes_allocated(), 0);
}

TEST_F(BinaryViewTest, RetentionSelectionsPreserveNullsAndNonViewColumns) {
  const auto payload = Views(Binary({std::string(40, 'x'), std::nullopt, "", "short"}));
  const auto numbers = testing::Int64s({10, 20, 30, 40});
  const auto schema =
      arrow::schema({arrow::field("n", arrow::int64()), arrow::field("p", arrow::binary_view())});
  const auto batch = arrow::RecordBatch::Make(schema, 4, {numbers, payload});
  auto* pool = arrow::default_memory_pool();
  for (const auto retention :
       {BinaryViewRetention::kShare, BinaryViewRetention::kCopy, BinaryViewRetention::kHybrid}) {
    for (const bool wide : {false, true}) {
      const auto indices = wide ? testing::Int64s({3, std::nullopt, 0, 1, 2})
                                : testing::ArrayOf<arrow::UInt32Builder, std::uint32_t>(
                                      arrow::uint32(), {3, std::nullopt, 0, 1, 2});
      const auto taken = TakeViewBatch(batch, indices, pool, retention);
      ASSERT_TRUE(taken.ok()) << taken.status().ToString();
      EXPECT_TRUE((*taken)->column(0)->Equals(testing::Int64s({40, std::nullopt, 10, 20, 30})));
      CheckValues(*(*taken)->column(1),
                  {"short", std::nullopt, std::string(40, 'x'), std::nullopt, ""});
    }
    const auto mask = std::static_pointer_cast<arrow::BooleanArray>(
        testing::Bools({true, true, false, std::nullopt}));
    const auto filtered = FilterViewBatch(batch, mask, pool, retention);
    ASSERT_TRUE(filtered.ok()) << filtered.status().ToString();
    CheckValues(*(*filtered)->column(1), {std::string(40, 'x'), std::nullopt});
    const auto all =
        std::static_pointer_cast<arrow::BooleanArray>(testing::Bools({true, true, true, true}));
    const auto unchanged_rows = FilterViewBatch(batch, all, pool, retention);
    ASSERT_TRUE(unchanged_rows.ok());
    EXPECT_EQ(*unchanged_rows == batch, retention == BinaryViewRetention::kShare);
    EXPECT_EQ((*unchanged_rows)->column(0), numbers);
    CheckValues(*(*unchanged_rows)->column(1), {std::string(40, 'x'), std::nullopt, "", "short"});
    const auto none =
        std::static_pointer_cast<arrow::BooleanArray>(testing::Bools({false, false, false, false}));
    const auto empty = FilterViewBatch(batch, none, pool, retention);
    ASSERT_TRUE(empty.ok());
    EXPECT_EQ((*empty)->num_rows(), 0);
    EXPECT_TRUE((*empty)->ValidateFull().ok());
    EXPECT_EQ((*empty)->column(1)->data()->buffers.size(), 2);
  }
  const auto ordinary = Batch(numbers);
  EXPECT_EQ(CopyBinaryViewPayload(ordinary, pool).ValueOrDie(), ordinary);
  EXPECT_TRUE(CopyBinaryViewPayload(nullptr, pool).status().IsInvalid());
  EXPECT_TRUE(CopyBinaryViewPayload(batch, nullptr).status().IsInvalid());
  EXPECT_EQ(RetainBinaryViewPayload(ordinary, pool).ValueOrDie(), ordinary);
  EXPECT_TRUE(RetainBinaryViewPayload(nullptr, pool).status().IsInvalid());
  EXPECT_TRUE(RetainBinaryViewPayload(batch, nullptr).status().IsInvalid());
}

TEST_F(BinaryViewTest, CheckedTakeAndFilterPreserveOrdinaryColumnsAndNullableIndices) {
  const auto payload = Views(Binary({"", std::nullopt, std::string(40, 'x'), "inline"}));
  const auto numbers = testing::Int64s({10, 20, 30, 40});
  const auto schema =
      arrow::schema({arrow::field("n", arrow::int64()), arrow::field("p", arrow::binary_view())});
  const auto batch = arrow::RecordBatch::Make(schema, 4, {numbers, payload});
  auto* pool = arrow::default_memory_pool();
  for (const bool wide : {false, true}) {
    std::shared_ptr<arrow::Array> indices;
    if (wide) {
      indices = testing::Int64s({3, std::nullopt, 2, 1, 0});
    } else {
      indices = testing::ArrayOf<arrow::UInt32Builder, std::uint32_t>(arrow::uint32(),
                                                                      {3, std::nullopt, 2, 1, 0});
    }
    const auto taken = TakeViewBatch(batch, indices, pool);
    ASSERT_TRUE(taken.ok()) << taken.status().ToString();
    EXPECT_TRUE((*taken)->column(0)->Equals(testing::Int64s({40, std::nullopt, 30, 20, 10})));
    CheckValues(*(*taken)->column(1),
                {"inline", std::nullopt, std::string(40, 'x'), std::nullopt, ""});
  }
  const auto selection = std::static_pointer_cast<arrow::BooleanArray>(
      testing::Bools({false, true, true, std::nullopt}));
  const auto filtered = FilterViewBatch(batch, selection, pool);
  ASSERT_TRUE(filtered.ok()) << filtered.status().ToString();
  EXPECT_TRUE((*filtered)->column(0)->Equals(testing::Int64s({20, 30})));
  CheckValues(*(*filtered)->column(1), {std::nullopt, std::string(40, 'x')});
  const auto empty_mask =
      std::static_pointer_cast<arrow::BooleanArray>(testing::Bools({false, false, false, false}));
  const auto empty = FilterViewBatch(batch, empty_mask, pool);
  ASSERT_TRUE(empty.ok());
  EXPECT_EQ((*empty)->num_rows(), 0);
  EXPECT_TRUE((*empty)->ValidateFull().ok());
  const auto all_mask =
      std::static_pointer_cast<arrow::BooleanArray>(testing::Bools({true, true, true, true}));
  EXPECT_EQ(FilterViewBatch(batch, all_mask, pool).ValueOrDie(), batch);
  EXPECT_TRUE(TakeViewBatch(batch, testing::Int64s({-1}), pool).status().IsIndexError());
  EXPECT_TRUE(TakeViewBatch(batch, testing::Int64s({4}), pool).status().IsIndexError());
  EXPECT_TRUE(TakeViewBatch(batch, numbers->Slice(0, 0), pool).ok());
  EXPECT_TRUE(TakeViewBatch(batch, testing::Int32s({0}), pool).status().IsTypeError());
  EXPECT_TRUE(FilterViewBatch(
                  batch, std::static_pointer_cast<arrow::BooleanArray>(testing::Bools({})), pool)
                  .status()
                  .IsInvalid());
}

TEST_F(BinaryViewTest, FinalMaterializationRestoresBinaryAndOwnsItsResults) {
  MemoryBudget budget(std::nullopt);
  std::shared_ptr<arrow::RecordBatch> output;
  const std::string bytes("invalid\xff\0utf8!", 14);
  {
    const auto input = Batch(Views(Binary({bytes, "", std::nullopt}, &budget), &budget));
    const auto result = MaterializeBinaryViews(input, &budget);
    ASSERT_TRUE(result.ok()) << result.status().ToString();
    output = *result;
    EXPECT_FALSE(HasBinaryViews(*output->schema()));
    EXPECT_EQ(output->schema()->field(0)->type()->id(), arrow::Type::BINARY);
    EXPECT_EQ(MaterializeBinaryViews(output, &budget).ValueOrDie(), output);
  }
  CheckValues(*output->column(0), {bytes, "", std::nullopt});
  output.reset();
  EXPECT_EQ(budget.bytes_allocated(), 0);
}

TEST_F(BinaryViewTest, EmptySchemasAndInvalidArgumentsAreChecked) {
  auto* pool = arrow::default_memory_pool();
  const auto empty_columns = arrow::RecordBatch::Make(arrow::schema({}), 3, arrow::ArrayVector{});
  const auto taken = TakeViewBatch(empty_columns, testing::Int64s({2, 1, std::nullopt}), pool);
  ASSERT_TRUE(taken.ok());
  EXPECT_EQ((*taken)->num_rows(), 3);
  EXPECT_EQ((*taken)->num_columns(), 0);
  EXPECT_TRUE(TakeViewBatch(empty_columns, testing::Int64s({3}), pool).status().IsIndexError());
  EXPECT_TRUE(TakeViewBatch(nullptr, testing::Int64s({}), pool).status().IsInvalid());
  EXPECT_TRUE(TakeViewBatch(empty_columns, nullptr, pool).status().IsInvalid());
  EXPECT_TRUE(TakeViewBatch(empty_columns, testing::Int64s({}), nullptr).status().IsInvalid());
  EXPECT_TRUE(CompactBinaryViews(nullptr, pool).status().IsInvalid());
  EXPECT_TRUE(CompactBinaryViews(empty_columns, nullptr).status().IsInvalid());
  EXPECT_TRUE(MaterializeBinaryViews(nullptr, pool).status().IsInvalid());
  EXPECT_TRUE(MaterializeBinaryViews(empty_columns, nullptr).status().IsInvalid());
  const auto input = Binary({"a"});
  const arrow::Array* source = input.get();
  const std::vector<JoinRowRef> bad_row = {{.chunk = 0, .row = 1}};
  const std::vector<JoinRowRef> bad_chunk = {{.chunk = 1, .row = 0}};
  EXPECT_TRUE(
      GatherBinaryViews<JoinRowRef>(std::span(&source, 1), bad_row, pool).status().IsIndexError());
  EXPECT_TRUE(GatherBinaryViews<JoinRowRef>(std::span(&source, 1), bad_chunk, pool)
                  .status()
                  .IsIndexError());
  EXPECT_TRUE(
      GatherBinaryViews<JoinRowRef>(std::span(&source, 1), {}, nullptr).status().IsInvalid());
  const arrow::Array* absent = nullptr;
  EXPECT_TRUE(GatherBinaryViews<JoinRowRef>(std::span(&absent, 1), {}, pool).status().IsInvalid());
  const auto numbers = testing::Int64s({1});
  source = numbers.get();
  EXPECT_TRUE(
      GatherBinaryViews<JoinRowRef>(std::span(&source, 1), {}, pool).status().IsTypeError());
}

TEST_F(BinaryViewTest, SharedBinaryGatherChecksUnusedSourcesAndEveryNonPaddingReference) {
  auto* pool = arrow::default_memory_pool();
  const auto input = Binary({std::string(13, 'a')});
  const auto numbers = testing::Int64s({1});
  const arrow::Array* source = input.get();
  const std::vector<JoinRowRef> first = {{.row = 0}};
  const std::vector<JoinRowRef> none;
  for (const auto& refs : {first, none}) {
    const std::vector<const arrow::Array*> absent = {source, nullptr};
    EXPECT_TRUE(GatherBinaryViews<JoinRowRef>(absent, refs, pool, BinaryViewRetention::kShare)
                    .status()
                    .IsInvalid());
    const std::vector<const arrow::Array*> wrong_type = {source, numbers.get()};
    EXPECT_TRUE(GatherBinaryViews<JoinRowRef>(wrong_type, refs, pool, BinaryViewRetention::kShare)
                    .status()
                    .IsTypeError());
  }
  for (const JoinRowRef ref : {JoinRowRef{.row = 1}, JoinRowRef{.chunk = 1, .row = 0}}) {
    EXPECT_TRUE(GatherBinaryViews<JoinRowRef>(std::span(&source, 1), std::span(&ref, 1), pool,
                                              BinaryViewRetention::kShare)
                    .status()
                    .IsIndexError());
  }
  const std::vector<SortBuffer::RowRef> outside = {{.row = 999}};
  EXPECT_TRUE(GatherBinaryViews<SortBuffer::RowRef>(std::span(&source, 1), outside, pool,
                                                    BinaryViewRetention::kShare)
                  .status()
                  .IsIndexError());
  const std::vector<SortBuffer::RowRef> padding = {{.chunk = kNoChunk, .row = 999}};
  const auto null = GatherBinaryViews<SortBuffer::RowRef>(std::span(&source, 1), padding, pool,
                                                          BinaryViewRetention::kShare);
  ASSERT_TRUE(null.ok()) << null.status().ToString();
  CheckValues(**null, {std::nullopt});
  EXPECT_EQ((*null)->data()->buffers.size(), 2);
  EXPECT_TRUE(GatherBinaryViews<JoinRowRef>(std::span(&source, 1), none, nullptr,
                                            BinaryViewRetention::kShare)
                  .status()
                  .IsInvalid());
  const auto empty = GatherBinaryViews<JoinRowRef>({}, none, pool, BinaryViewRetention::kShare);
  ASSERT_TRUE(empty.ok()) << empty.status().ToString();
  CheckValues(**empty, {});
  EXPECT_EQ((*empty)->data()->buffers.size(), 2);
}

TEST_F(BinaryViewTest, AllocationFailuresReleaseAllScratchAndPermitRetry) {
  const auto input = Binary(Values(4096, std::string(128, 'q')));
  const arrow::Array* source = input.get();
  const std::vector<JoinRowRef> refs(32, JoinRowRef{.row = 17});
  int failed = 0;
  bool succeeded = false;
  for (int64_t limit = 0; limit <= 8192; limit += 64) {
    MemoryBudget budget(limit);
    {
      const auto gathered = GatherBinaryViews<JoinRowRef>(std::span(&source, 1), refs, &budget);
      if (gathered.ok()) {
        CheckValues(**gathered, Values(refs.size(), std::string(128, 'q')));
        succeeded = true;
      } else {
        EXPECT_TRUE(gathered.status().IsOutOfMemory()) << gathered.status().ToString();
        ++failed;
      }
    }
    EXPECT_EQ(budget.bytes_allocated(), 0) << limit;
    if (succeeded) {
      break;
    }
  }
  EXPECT_TRUE(succeeded);
  EXPECT_GT(failed, 4);
}

TEST_F(BinaryViewTest, RetentionAllocationFailuresReleaseScratchAndPermitRetry) {
  const auto input = Binary(Values(4096, std::string(128, 'q')));
  const arrow::Array* source = input.get();
  const std::vector<JoinRowRef> refs(32, JoinRowRef{.row = 17});
  for (const auto retention :
       {BinaryViewRetention::kShare, BinaryViewRetention::kCopy, BinaryViewRetention::kHybrid}) {
    int failed = 0;
    bool succeeded = false;
    for (int64_t limit = 0; limit <= 8192; limit += 64) {
      MemoryBudget budget(limit);
      {
        const auto result =
            GatherBinaryViews<JoinRowRef>(std::span(&source, 1), refs, &budget, retention);
        if (result.ok()) {
          CheckValues(**result, Values(refs.size(), std::string(128, 'q')));
          succeeded = true;
        } else {
          EXPECT_TRUE(result.status().IsOutOfMemory()) << result.status().ToString();
          ++failed;
        }
      }
      EXPECT_EQ(budget.bytes_allocated(), 0) << limit;
      if (succeeded) {
        break;
      }
    }
    EXPECT_TRUE(succeeded);
    EXPECT_GT(failed, 4);
  }
}

TEST_F(BinaryViewTest, SharedBinaryGatherAllocationFailuresReleaseBitmapOwnersAndScratch) {
  const std::string long_value(81, 'q');
  const std::string second_value(37, 's');
  const std::string third_value(13, 't');
  const std::string short_value(12, 'i');
  const auto input = Binary({long_value, std::nullopt, short_value});
  const auto second = Binary({second_value});
  const auto third = Binary({third_value});
  const std::vector<const arrow::Array*> sources = {input.get(), second.get(), third.get()};
  const std::vector<JoinRowRef> refs = {
      {.chunk = 2, .row = 0}, {.row = 1}, {.chunk = kNoChunk, .row = 999}, {.row = 2},
      {.chunk = 1, .row = 0}, {.row = 0}, {.chunk = 2, .row = 0}};
  int failed = 0;
  bool succeeded = false;
  // Sweep budgets rather than pinning a particular allocation layout or host allocator size.
  for (int64_t limit = 0; limit <= 8192; limit += 64) {
    MemoryBudget budget(limit);
    {
      const auto result =
          GatherBinaryViews<JoinRowRef>(sources, refs, &budget, BinaryViewRetention::kShare);
      if (result.ok()) {
        CheckValues(**result, {third_value, std::nullopt, std::nullopt, short_value, second_value,
                               long_value, third_value});
        succeeded = true;
      } else {
        EXPECT_TRUE(result.status().IsOutOfMemory()) << result.status().ToString();
        ++failed;
      }
    }
    EXPECT_EQ(budget.bytes_allocated(), 0) << limit;
    if (succeeded) {
      break;
    }
  }
  EXPECT_TRUE(succeeded);
  EXPECT_GT(failed, 0);
}

TEST_F(BinaryViewTest, PayloadCopyFailureReleasesFinishedColumnsAndPermitsRetry) {
  constexpr std::size_t kPayloadBytes = std::size_t{64} * 1024;
  const std::string first(kPayloadBytes, 'a');
  const std::string second(kPayloadBytes, 'b');
  const auto input =
      arrow::RecordBatch::Make(arrow::schema({arrow::field("first", arrow::binary_view()),
                                              arrow::field("second", arrow::binary_view())}),
                               1, {Views(Binary({first})), Views(Binary({second}))});
  MemoryBudget limited(int64_t{96} * 1024);
  const auto failed = CopyBinaryViewPayload(input, &limited);
  ASSERT_TRUE(failed.status().IsOutOfMemory()) << failed.status().ToString();
  EXPECT_GT(limited.max_memory(), Narrow<int64_t>(kPayloadBytes));
  EXPECT_EQ(limited.bytes_allocated(), 0);
  CheckValues(*input->column(0), {first});
  CheckValues(*input->column(1), {second});
  MemoryBudget retry(std::nullopt);
  {
    const auto result = CopyBinaryViewPayload(input, &retry);
    ASSERT_TRUE(result.ok()) << result.status().ToString();
    CheckValues(*(*result)->column(0), {first});
    CheckValues(*(*result)->column(1), {second});
    EXPECT_TRUE(HasBinaryViews(*(*result)->schema()));
  }
  EXPECT_EQ(retry.bytes_allocated(), 0);
}

TEST_F(BinaryViewTest, HybridFailureReleasesFinishedColumnsAndPermitsRetry) {
  constexpr std::size_t kPayloadBytes = std::size_t{64} * 1024;
  const std::string first(kPayloadBytes, 'a');
  const std::string second(kPayloadBytes, 'b');
  const auto input = arrow::RecordBatch::Make(
      arrow::schema({arrow::field("first", arrow::binary_view()),
                     arrow::field("second", arrow::binary_view())}),
      1,
      {Views(Binary({first, first}))->Slice(0, 1), Views(Binary({second, second}))->Slice(0, 1)});
  MemoryBudget limited(int64_t{96} * 1024);
  const auto failed = RetainBinaryViewPayload(input, &limited);
  ASSERT_TRUE(failed.status().IsOutOfMemory()) << failed.status().ToString();
  EXPECT_GT(limited.max_memory(), Narrow<int64_t>(kPayloadBytes));
  EXPECT_EQ(limited.bytes_allocated(), 0);
  CheckValues(*input->column(0), {first});
  CheckValues(*input->column(1), {second});
  MemoryBudget retry(std::nullopt);
  {
    const auto result = RetainBinaryViewPayload(input, &retry);
    ASSERT_TRUE(result.ok()) << result.status().ToString();
    CheckValues(*(*result)->column(0), {first});
    CheckValues(*(*result)->column(1), {second});
    EXPECT_EQ((*result)->column(0)->data()->buffers[2]->size(), Narrow<int64_t>(kPayloadBytes));
    EXPECT_NE((*result)->column(0)->data()->buffers[2], input->column(0)->data()->buffers[2]);
  }
  EXPECT_EQ(retry.bytes_allocated(), 0);
}

TEST_F(BinaryViewTest, MaterializationFailureReleasesAlreadyFinishedColumnsAndPermitsRetry) {
  constexpr std::size_t kPayloadBytes = std::size_t{64} * 1024;
  const std::string first(kPayloadBytes, 'a');
  const std::string second(kPayloadBytes, 'b');
  const auto input =
      arrow::RecordBatch::Make(arrow::schema({arrow::field("first", arrow::binary_view()),
                                              arrow::field("second", arrow::binary_view())}),
                               1, {Views(Binary({first})), Views(Binary({second}))});
  MemoryBudget limited(int64_t{96} * 1024);
  const auto failed = MaterializeBinaryViews(input, &limited);
  ASSERT_TRUE(failed.status().IsOutOfMemory()) << failed.status().ToString();
  // The first payload and its offsets/validity fit. The second payload fails after that
  // finished Binary array has entered the output vector, which must then release it.
  EXPECT_GT(limited.max_memory(), Narrow<int64_t>(kPayloadBytes));
  EXPECT_EQ(limited.bytes_allocated(), 0);
  CheckValues(*input->column(0), {first});
  CheckValues(*input->column(1), {second});
  MemoryBudget retry(std::nullopt);
  {
    const auto result = MaterializeBinaryViews(input, &retry);
    ASSERT_TRUE(result.ok()) << result.status().ToString();
    CheckValues(*(*result)->column(0), {first});
    CheckValues(*(*result)->column(1), {second});
    EXPECT_FALSE(HasBinaryViews(*(*result)->schema()));
  }
  EXPECT_EQ(retry.bytes_allocated(), 0);
}

class ThrowingAllocationPool final : public arrow::ProxyMemoryPool {
 public:
  ThrowingAllocationPool() : arrow::ProxyMemoryPool(arrow::default_memory_pool()) {}
  using arrow::ProxyMemoryPool::Allocate;
  arrow::Status Allocate(int64_t /*size*/, int64_t /*alignment*/, std::uint8_t** /*out*/) override {
    throw std::bad_alloc();
  }
};

TEST_F(BinaryViewTest, AllocationExceptionsBecomeStatusesAtEachBoundary) {
  ThrowingAllocationPool pool;
  const auto input = Binary({std::string(50, 'a'), std::nullopt});
  const auto view = Views(input);
  const auto batch = Batch(view);
  const arrow::Array* source = input.get();
  const std::vector<JoinRowRef> refs = {{.chunk = 0, .row = 0}};
  EXPECT_TRUE(
      GatherBinaryViews<JoinRowRef>(std::span(&source, 1), refs, &pool).status().IsOutOfMemory());
  EXPECT_TRUE(
      GatherBinaryViews<JoinRowRef>(std::span(&source, 1), refs, &pool, BinaryViewRetention::kShare)
          .status()
          .IsOutOfMemory());
  EXPECT_TRUE(TakeViewBatch(batch, testing::Int64s({0}), &pool).status().IsOutOfMemory());
  EXPECT_TRUE(
      FilterViewBatch(batch,
                      std::static_pointer_cast<arrow::BooleanArray>(testing::Bools({true, false})),
                      &pool)
          .status()
          .IsOutOfMemory());
  EXPECT_TRUE(CompactBinaryViews(batch->Slice(0, 1), &pool).status().IsOutOfMemory());
  EXPECT_TRUE(CopyBinaryViewPayload(batch, &pool).status().IsOutOfMemory());
  EXPECT_TRUE(RetainBinaryViewPayload(batch, &pool).status().IsOutOfMemory());
  EXPECT_TRUE(MaterializeBinaryViews(batch, &pool).status().IsOutOfMemory());
  EXPECT_EQ(pool.bytes_allocated(), 0);
}

}  // namespace
}  // namespace antb1::exec
