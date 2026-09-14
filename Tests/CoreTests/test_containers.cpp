// CoreTests/test_containers.cpp

#include <NF/Test/TestFramework.hpp>
#include <NF/Core/Containers.hpp>

using namespace nf;

NF_TEST(test_dynamic_array_push) {
    DynamicArray<i32> arr;
    arr.push_back(1);
    arr.push_back(2);
    arr.push_back(3);

    NF_CHECK_EQ(arr.size(), usize(3));
    NF_CHECK_EQ(arr[0], 1);
    NF_CHECK_EQ(arr[1], 2);
    NF_CHECK_EQ(arr[2], 3);
}

NF_TEST(test_dynamic_array_emplace) {
    DynamicArray<i32> arr;
    arr.emplace_back(42);
    NF_CHECK_EQ(arr[0], 42);
}

NF_TEST(test_dynamic_array_pop) {
    DynamicArray<i32> arr;
    arr.push_back(1);
    arr.push_back(2);
    arr.pop_back();
    NF_CHECK_EQ(arr.size(), usize(1));
}

NF_TEST(test_dynamic_array_resize) {
    DynamicArray<i32> arr;
    arr.resize(10, 5);
    NF_CHECK_EQ(arr.size(), usize(10));
    for (usize i = 0; i < 10; ++i) {
        NF_CHECK_EQ(arr[i], 5);
    }
}

NF_TEST(test_dynamic_array_iterate) {
    DynamicArray<i32> arr;
    for (i32 i = 0; i < 10; ++i) arr.push_back(i);

    i32 sum = 0;
    for (i32 v : arr) sum += v;
    NF_CHECK_EQ(sum, 45);
}

NF_TEST(test_hashmap_basic) {
    HashMap<std::string, i32> map;
    map.put("one", 1);
    map.put("two", 2);
    map.put("three", 3);

    NF_CHECK_EQ(map.size(), usize(3));
    NF_CHECK_EQ(*map.get("one"), 1);
    NF_CHECK_EQ(*map.get("two"), 2);
    NF_CHECK_EQ(*map.get("three"), 3);
}

NF_TEST(test_hashmap_missing) {
    HashMap<std::string, i32> map;
    map.put("a", 1);
    auto* val = map.get("nonexistent");
    NF_CHECK(val == nullptr);
}

NF_TEST(test_hashmap_remove) {
    HashMap<std::string, i32> map;
    map.put("a", 1);
    map.put("b", 2);
    map.remove("a");
    NF_CHECK(!map.contains("a"));
    NF_CHECK(map.contains("b"));
}
