/*
 * Copyright (C) 2011 Google Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "platform/wtf/HashMap.h"

#include <memory>

#include "base/memory/ptr_util.h"
#include "base/memory/scoped_refptr.h"
#include "platform/wtf/RefCounted.h"
#include "platform/wtf/Vector.h"
#include "platform/wtf/WTFTestHelper.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace WTF {

int DummyRefCounted::ref_invokes_count_ = 0;

namespace {

using IntHashMap = HashMap<int, int>;

TEST(HashMapTest, IteratorComparison) {
  IntHashMap map;
  map.insert(1, 2);
  EXPECT_TRUE(map.begin() != map.end());
  EXPECT_FALSE(map.begin() == map.end());

  IntHashMap::const_iterator begin = map.begin();
  EXPECT_TRUE(begin == map.begin());
  EXPECT_TRUE(map.begin() == begin);
  EXPECT_TRUE(begin != map.end());
  EXPECT_TRUE(map.end() != begin);
  EXPECT_FALSE(begin != map.begin());
  EXPECT_FALSE(map.begin() != begin);
  EXPECT_FALSE(begin == map.end());
  EXPECT_FALSE(map.end() == begin);
}

struct TestDoubleHashTraits : HashTraits<double> {
  static const unsigned kMinimumTableSize = 8;
};

using DoubleHashMap =
    HashMap<double, int64_t, DefaultHash<double>::Hash, TestDoubleHashTraits>;

int BucketForKey(double key) {
  return DefaultHash<double>::Hash::GetHash(key) &
         (TestDoubleHashTraits::kMinimumTableSize - 1);
}

TEST(HashMapTest, DoubleHashCollisions) {
  // The "clobber" key here is one that ends up stealing the bucket that the -0
  // key originally wants to be in. This makes the 0 and -0 keys collide and
  // the test then fails unless the FloatHash::equals() implementation can
  // distinguish them.
  const double kClobberKey = 6;
  const double kZeroKey = 0;
  const double kNegativeZeroKey = -kZeroKey;

  DoubleHashMap map;

  map.insert(kClobberKey, 1);
  map.insert(kZeroKey, 2);
  map.insert(kNegativeZeroKey, 3);

  EXPECT_EQ(BucketForKey(kClobberKey), BucketForKey(kNegativeZeroKey));
  EXPECT_EQ(1, map.at(kClobberKey));
  EXPECT_EQ(2, map.at(kZeroKey));
  EXPECT_EQ(3, map.at(kNegativeZeroKey));
}

using OwnPtrHashMap = HashMap<int, std::unique_ptr<DestructCounter>>;

TEST(HashMapTest, OwnPtrAsValue) {
  int destruct_number = 0;
  OwnPtrHashMap map;
  map.insert(1, std::make_unique<DestructCounter>(1, &destruct_number));
  map.insert(2, std::make_unique<DestructCounter>(2, &destruct_number));

  DestructCounter* counter1 = map.at(1);
  EXPECT_EQ(1, counter1->Get());
  DestructCounter* counter2 = map.at(2);
  EXPECT_EQ(2, counter2->Get());
  EXPECT_EQ(0, destruct_number);

  for (OwnPtrHashMap::iterator iter = map.begin(); iter != map.end(); ++iter) {
    std::unique_ptr<DestructCounter>& own_counter = iter->value;
    EXPECT_EQ(iter->key, own_counter->Get());
  }
  ASSERT_EQ(0, destruct_number);

  std::unique_ptr<DestructCounter> own_counter1 = map.Take(1);
  EXPECT_EQ(own_counter1.get(), counter1);
  EXPECT_FALSE(map.Contains(1));
  EXPECT_EQ(0, destruct_number);

  map.erase(2);
  EXPECT_FALSE(map.Contains(2));
  EXPECT_EQ(0UL, map.size());
  EXPECT_EQ(1, destruct_number);

  own_counter1.reset();
  EXPECT_EQ(2, destruct_number);
}

TEST(HashMapTest, RefPtrAsKey) {
  bool is_deleted = false;
  DummyRefCounted::ref_invokes_count_ = 0;
  scoped_refptr<DummyRefCounted> ptr =
      base::AdoptRef(new DummyRefCounted(is_deleted));
  EXPECT_EQ(0, DummyRefCounted::ref_invokes_count_);
  HashMap<scoped_refptr<DummyRefCounted>, int> map;
  map.insert(ptr, 1);
  // Referenced only once (to store a copy in the container).
  EXPECT_EQ(1, DummyRefCounted::ref_invokes_count_);
  EXPECT_EQ(1, map.at(ptr));

  DummyRefCounted* raw_ptr = ptr.get();

  EXPECT_TRUE(map.Contains(raw_ptr));
  EXPECT_NE(map.end(), map.find(raw_ptr));
  EXPECT_TRUE(map.Contains(ptr));
  EXPECT_NE(map.end(), map.find(ptr));
  EXPECT_EQ(1, DummyRefCounted::ref_invokes_count_);

  ptr = nullptr;
  EXPECT_FALSE(is_deleted);

  map.erase(raw_ptr);
  EXPECT_EQ(1, DummyRefCounted::ref_invokes_count_);
  EXPECT_TRUE(is_deleted);
  EXPECT_TRUE(map.IsEmpty());
}

TEST(HashMaptest, RemoveAdd) {
  DummyRefCounted::ref_invokes_count_ = 0;
  bool is_deleted = false;

  typedef HashMap<int, scoped_refptr<DummyRefCounted>> Map;
  Map map;

  scoped_refptr<DummyRefCounted> ptr =
      base::AdoptRef(new DummyRefCounted(is_deleted));
  EXPECT_EQ(0, DummyRefCounted::ref_invokes_count_);

  map.insert(1, ptr);
  // Referenced only once (to store a copy in the container).
  EXPECT_EQ(1, DummyRefCounted::ref_invokes_count_);
  EXPECT_EQ(ptr, map.at(1));

  ptr = nullptr;
  EXPECT_FALSE(is_deleted);

  map.erase(1);
  EXPECT_EQ(1, DummyRefCounted::ref_invokes_count_);
  EXPECT_TRUE(is_deleted);
  EXPECT_TRUE(map.IsEmpty());

  // Add and remove until the deleted slot is reused.
  for (int i = 1; i < 100; i++) {
    bool is_deleted2 = false;
    scoped_refptr<DummyRefCounted> ptr2 =
        base::AdoptRef(new DummyRefCounted(is_deleted2));
    map.insert(i, ptr2);
    EXPECT_FALSE(is_deleted2);
    ptr2 = nullptr;
    EXPECT_FALSE(is_deleted2);
    map.erase(i);
    EXPECT_TRUE(is_deleted2);
  }
}

class SimpleClass {
 public:
  explicit SimpleClass(int v) : v_(v) {}
  int V() { return v_; }

 private:
  int v_;
};
using IntSimpleMap = HashMap<int, std::unique_ptr<SimpleClass>>;

TEST(HashMapTest, AddResult) {
  IntSimpleMap map;
  IntSimpleMap::AddResult result = map.insert(1, nullptr);
  EXPECT_TRUE(result.is_new_entry);
  EXPECT_EQ(1, result.stored_value->key);
  EXPECT_EQ(nullptr, result.stored_value->value.get());

  SimpleClass* simple1 = new SimpleClass(1);
  result.stored_value->value = base::WrapUnique(simple1);
  EXPECT_EQ(simple1, map.at(1));

  IntSimpleMap::AddResult result2 =
      map.insert(1, std::make_unique<SimpleClass>(2));
  EXPECT_FALSE(result2.is_new_entry);
  EXPECT_EQ(1, result.stored_value->key);
  EXPECT_EQ(1, result.stored_value->value->V());
  EXPECT_EQ(1, map.at(1)->V());
}

TEST(HashMapTest, AddResultVectorValue) {
  using IntVectorMap = HashMap<int, Vector<int>>;
  IntVectorMap map;
  IntVectorMap::AddResult result = map.insert(1, Vector<int>());
  EXPECT_TRUE(result.is_new_entry);
  EXPECT_EQ(1, result.stored_value->key);
  EXPECT_EQ(0u, result.stored_value->value.size());

  result.stored_value->value.push_back(11);
  EXPECT_EQ(1u, map.find(1)->value.size());
  EXPECT_EQ(11, map.find(1)->value.front());

  IntVectorMap::AddResult result2 = map.insert(1, Vector<int>());
  EXPECT_FALSE(result2.is_new_entry);
  EXPECT_EQ(1, result.stored_value->key);
  EXPECT_EQ(1u, result.stored_value->value.size());
  EXPECT_EQ(11, result.stored_value->value.front());
  EXPECT_EQ(11, map.find(1)->value.front());
}

class InstanceCounter {
 public:
  InstanceCounter() { ++counter_; }
  InstanceCounter(const InstanceCounter& another) { ++counter_; }
  ~InstanceCounter() { --counter_; }
  static int counter_;
};
int InstanceCounter::counter_ = 0;

TEST(HashMapTest, ValueTypeDestructed) {
  InstanceCounter::counter_ = 0;
  HashMap<int, InstanceCounter> map;
  map.Set(1, InstanceCounter());
  map.clear();
  EXPECT_EQ(0, InstanceCounter::counter_);
}

TEST(HashMapTest, MoveOnlyValueType) {
  using TheMap = HashMap<int, MoveOnlyHashValue>;
  TheMap map;
  {
    TheMap::AddResult add_result = map.insert(1, MoveOnlyHashValue(10));
    EXPECT_TRUE(add_result.is_new_entry);
    EXPECT_EQ(1, add_result.stored_value->key);
    EXPECT_EQ(10, add_result.stored_value->value.Value());
  }
  auto iter = map.find(1);
  ASSERT_TRUE(iter != map.end());
  EXPECT_EQ(1, iter->key);
  EXPECT_EQ(10, iter->value.Value());

  iter = map.find(2);
  EXPECT_TRUE(iter == map.end());

  // Try to add more to trigger rehashing.
  for (int i = 2; i < 32; ++i) {
    TheMap::AddResult add_result = map.insert(i, MoveOnlyHashValue(i * 10));
    EXPECT_TRUE(add_result.is_new_entry);
    EXPECT_EQ(i, add_result.stored_value->key);
    EXPECT_EQ(i * 10, add_result.stored_value->value.Value());
  }

  iter = map.find(1);
  ASSERT_TRUE(iter != map.end());
  EXPECT_EQ(1, iter->key);
  EXPECT_EQ(10, iter->value.Value());

  iter = map.find(7);
  ASSERT_TRUE(iter != map.end());
  EXPECT_EQ(7, iter->key);
  EXPECT_EQ(70, iter->value.Value());

  {
    TheMap::AddResult add_result = map.Set(9, MoveOnlyHashValue(999));
    EXPECT_FALSE(add_result.is_new_entry);
    EXPECT_EQ(9, add_result.stored_value->key);
    EXPECT_EQ(999, add_result.stored_value->value.Value());
  }

  map.erase(11);
  iter = map.find(11);
  EXPECT_TRUE(iter == map.end());

  MoveOnlyHashValue one_thirty(map.Take(13));
  EXPECT_EQ(130, one_thirty.Value());
  iter = map.find(13);
  EXPECT_TRUE(iter == map.end());

  map.clear();
}

TEST(HashMapTest, MoveOnlyKeyType) {
  // The content of this test is similar to the test above, except that the
  // types of key and value are swapped.
  using TheMap = HashMap<MoveOnlyHashValue, int>;
  TheMap map;
  {
    TheMap::AddResult add_result = map.insert(MoveOnlyHashValue(1), 10);
    EXPECT_TRUE(add_result.is_new_entry);
    EXPECT_EQ(1, add_result.stored_value->key.Value());
    EXPECT_EQ(10, add_result.stored_value->value);
  }
  auto iter = map.find(MoveOnlyHashValue(1));
  ASSERT_TRUE(iter != map.end());
  EXPECT_EQ(1, iter->key.Value());
  EXPECT_EQ(10, iter->value);

  iter = map.find(MoveOnlyHashValue(2));
  EXPECT_TRUE(iter == map.end());

  for (int i = 2; i < 32; ++i) {
    TheMap::AddResult add_result = map.insert(MoveOnlyHashValue(i), i * 10);
    EXPECT_TRUE(add_result.is_new_entry);
    EXPECT_EQ(i, add_result.stored_value->key.Value());
    EXPECT_EQ(i * 10, add_result.stored_value->value);
  }

  iter = map.find(MoveOnlyHashValue(1));
  ASSERT_TRUE(iter != map.end());
  EXPECT_EQ(1, iter->key.Value());
  EXPECT_EQ(10, iter->value);

  iter = map.find(MoveOnlyHashValue(7));
  ASSERT_TRUE(iter != map.end());
  EXPECT_EQ(7, iter->key.Value());
  EXPECT_EQ(70, iter->value);

  {
    TheMap::AddResult add_result = map.Set(MoveOnlyHashValue(9), 999);
    EXPECT_FALSE(add_result.is_new_entry);
    EXPECT_EQ(9, add_result.stored_value->key.Value());
    EXPECT_EQ(999, add_result.stored_value->value);
  }

  map.erase(MoveOnlyHashValue(11));
  iter = map.find(MoveOnlyHashValue(11));
  EXPECT_TRUE(iter == map.end());

  int one_thirty = map.Take(MoveOnlyHashValue(13));
  EXPECT_EQ(130, one_thirty);
  iter = map.find(MoveOnlyHashValue(13));
  EXPECT_TRUE(iter == map.end());

  map.clear();
}

TEST(HashMapTest, MoveShouldNotMakeCopy) {
  HashMap<int, CountCopy> map;
  int counter = 0;
  map.insert(1, CountCopy(counter));

  HashMap<int, CountCopy> other(map);
  counter = 0;
  map = std::move(other);
  EXPECT_EQ(0, counter);

  counter = 0;
  HashMap<int, CountCopy> yet_another(std::move(map));
  EXPECT_EQ(0, counter);
}

TEST(HashMapTest, UniquePtrAsKey) {
  using Pointer = std::unique_ptr<int>;
  using Map = HashMap<Pointer, int>;
  Map map;
  int* one_pointer = new int(1);
  {
    Map::AddResult add_result = map.insert(Pointer(one_pointer), 1);
    EXPECT_TRUE(add_result.is_new_entry);
    EXPECT_EQ(one_pointer, add_result.stored_value->key.get());
    EXPECT_EQ(1, *add_result.stored_value->key);
    EXPECT_EQ(1, add_result.stored_value->value);
  }
  auto iter = map.find(one_pointer);
  ASSERT_TRUE(iter != map.end());
  EXPECT_EQ(one_pointer, iter->key.get());
  EXPECT_EQ(1, iter->value);

  Pointer nonexistent(new int(42));
  iter = map.find(nonexistent.get());
  EXPECT_TRUE(iter == map.end());

  // Insert more to cause a rehash.
  for (int i = 2; i < 32; ++i) {
    Map::AddResult add_result = map.insert(Pointer(new int(i)), i);
    EXPECT_TRUE(add_result.is_new_entry);
    EXPECT_EQ(i, *add_result.stored_value->key);
    EXPECT_EQ(i, add_result.stored_value->value);
  }

  iter = map.find(one_pointer);
  ASSERT_TRUE(iter != map.end());
  EXPECT_EQ(one_pointer, iter->key.get());
  EXPECT_EQ(1, iter->value);

  EXPECT_EQ(1, map.Take(one_pointer));
  // From now on, |onePointer| is a dangling pointer.

  iter = map.find(one_pointer);
  EXPECT_TRUE(iter == map.end());
}

TEST(HashMapTest, UniquePtrAsValue) {
  using Pointer = std::unique_ptr<int>;
  using Map = HashMap<int, Pointer>;
  Map map;
  {
    Map::AddResult add_result = map.insert(1, Pointer(new int(1)));
    EXPECT_TRUE(add_result.is_new_entry);
    EXPECT_EQ(1, add_result.stored_value->key);
    EXPECT_EQ(1, *add_result.stored_value->value);
  }
  auto iter = map.find(1);
  ASSERT_TRUE(iter != map.end());
  EXPECT_EQ(1, iter->key);
  EXPECT_EQ(1, *iter->value);

  int* one_pointer = map.at(1);
  EXPECT_TRUE(one_pointer);
  EXPECT_EQ(1, *one_pointer);

  iter = map.find(42);
  EXPECT_TRUE(iter == map.end());

  for (int i = 2; i < 32; ++i) {
    Map::AddResult add_result = map.insert(i, Pointer(new int(i)));
    EXPECT_TRUE(add_result.is_new_entry);
    EXPECT_EQ(i, add_result.stored_value->key);
    EXPECT_EQ(i, *add_result.stored_value->value);
  }

  iter = map.find(1);
  ASSERT_TRUE(iter != map.end());
  EXPECT_EQ(1, iter->key);
  EXPECT_EQ(1, *iter->value);

  Pointer one(map.Take(1));
  ASSERT_TRUE(one);
  EXPECT_EQ(1, *one);

  Pointer empty(map.Take(42));
  EXPECT_TRUE(!empty);

  iter = map.find(1);
  EXPECT_TRUE(iter == map.end());

  {
    Map::AddResult add_result = map.insert(1, std::move(one));
    EXPECT_TRUE(add_result.is_new_entry);
    EXPECT_EQ(1, add_result.stored_value->key);
    EXPECT_EQ(1, *add_result.stored_value->value);
  }
}

TEST(HashMapTest, MoveOnlyPairKeyType) {
  using Pair = std::pair<MoveOnlyHashValue, int>;
  using TheMap = HashMap<Pair, int>;
  TheMap map;
  {
    TheMap::AddResult add_result =
        map.insert(Pair(MoveOnlyHashValue(1), -1), 10);
    EXPECT_TRUE(add_result.is_new_entry);
    EXPECT_EQ(1, add_result.stored_value->key.first.Value());
    EXPECT_EQ(-1, add_result.stored_value->key.second);
    EXPECT_EQ(10, add_result.stored_value->value);
  }
  auto iter = map.find(Pair(MoveOnlyHashValue(1), -1));
  ASSERT_TRUE(iter != map.end());
  EXPECT_EQ(1, iter->key.first.Value());
  EXPECT_EQ(-1, iter->key.second);
  EXPECT_EQ(10, iter->value);

  iter = map.find(Pair(MoveOnlyHashValue(1), 0));
  EXPECT_TRUE(iter == map.end());

  for (int i = 2; i < 32; ++i) {
    TheMap::AddResult add_result =
        map.insert(Pair(MoveOnlyHashValue(i), -i), i * 10);
    EXPECT_TRUE(add_result.is_new_entry);
    EXPECT_EQ(i, add_result.stored_value->key.first.Value());
    EXPECT_EQ(-i, add_result.stored_value->key.second);
    EXPECT_EQ(i * 10, add_result.stored_value->value);
  }

  iter = map.find(Pair(MoveOnlyHashValue(1), -1));
  ASSERT_TRUE(iter != map.end());
  EXPECT_EQ(1, iter->key.first.Value());
  EXPECT_EQ(-1, iter->key.second);
  EXPECT_EQ(10, iter->value);

  iter = map.find(Pair(MoveOnlyHashValue(7), -7));
  ASSERT_TRUE(iter != map.end());
  EXPECT_EQ(7, iter->key.first.Value());
  EXPECT_EQ(-7, iter->key.second);
  EXPECT_EQ(70, iter->value);

  {
    TheMap::AddResult add_result = map.Set(Pair(MoveOnlyHashValue(9), -9), 999);
    EXPECT_FALSE(add_result.is_new_entry);
    EXPECT_EQ(9, add_result.stored_value->key.first.Value());
    EXPECT_EQ(-9, add_result.stored_value->key.second);
    EXPECT_EQ(999, add_result.stored_value->value);
  }

  map.erase(Pair(MoveOnlyHashValue(11), -11));
  iter = map.find(Pair(MoveOnlyHashValue(11), -11));
  EXPECT_TRUE(iter == map.end());

  int one_thirty = map.Take(Pair(MoveOnlyHashValue(13), -13));
  EXPECT_EQ(130, one_thirty);
  iter = map.find(Pair(MoveOnlyHashValue(13), -13));
  EXPECT_TRUE(iter == map.end());

  map.clear();
}

bool IsOneTwoThreeMap(const HashMap<int, int>& map) {
  return map.size() == 3 && map.Contains(1) && map.Contains(2) &&
         map.Contains(3) && map.at(1) == 11 && map.at(2) == 22 &&
         map.at(3) == 33;
};

HashMap<int, int> ReturnOneTwoThreeMap() {
  return {{1, 11}, {2, 22}, {3, 33}};
};

TEST(HashMapTest, InitializerList) {
  HashMap<int, int> empty({});
  EXPECT_TRUE(empty.IsEmpty());

  HashMap<int, int> one({{1, 11}});
  EXPECT_EQ(one.size(), 1u);
  EXPECT_TRUE(one.Contains(1));
  EXPECT_EQ(one.at(1), 11);

  HashMap<int, int> one_two_three({{1, 11}, {2, 22}, {3, 33}});
  EXPECT_EQ(one_two_three.size(), 3u);
  EXPECT_TRUE(one_two_three.Contains(1));
  EXPECT_TRUE(one_two_three.Contains(2));
  EXPECT_TRUE(one_two_three.Contains(3));
  EXPECT_EQ(one_two_three.at(1), 11);
  EXPECT_EQ(one_two_three.at(2), 22);
  EXPECT_EQ(one_two_three.at(3), 33);

  // Put some jank so we can check if the assignments can clear them later.
  empty.insert(9999, 99999);
  one.insert(9999, 99999);
  one_two_three.insert(9999, 99999);

  empty = {};
  EXPECT_TRUE(empty.IsEmpty());

  one = {{1, 11}};
  EXPECT_EQ(one.size(), 1u);
  EXPECT_TRUE(one.Contains(1));
  EXPECT_EQ(one.at(1), 11);

  one_two_three = {{1, 11}, {2, 22}, {3, 33}};
  EXPECT_EQ(one_two_three.size(), 3u);
  EXPECT_TRUE(one_two_three.Contains(1));
  EXPECT_TRUE(one_two_three.Contains(2));
  EXPECT_TRUE(one_two_three.Contains(3));
  EXPECT_EQ(one_two_three.at(1), 11);
  EXPECT_EQ(one_two_three.at(2), 22);
  EXPECT_EQ(one_two_three.at(3), 33);

  // Other ways of construction: as a function parameter and in a return
  // statement.
  EXPECT_TRUE(IsOneTwoThreeMap({{1, 11}, {2, 22}, {3, 33}}));
  EXPECT_TRUE(IsOneTwoThreeMap(ReturnOneTwoThreeMap()));
}

static_assert(!IsTraceable<HashMap<int, int>>::value,
              "HashMap<int, int> must not be traceable.");

}  // anonymous namespace

}  // namespace WTF
