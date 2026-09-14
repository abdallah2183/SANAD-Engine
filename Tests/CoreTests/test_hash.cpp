// CoreTests/test_hash.cpp

#include <NF/Test/TestFramework.hpp>
#include <NF/Core/Hash.hpp>

using namespace nf;

NF_TEST(test_fnv1a_string) {
    u64 h1 = fnv1a_64("hello");
    u64 h2 = fnv1a_64("hello");
    u64 h3 = fnv1a_64("world");

    NF_CHECK_EQ(h1, h2);
    NF_CHECK(h1 != h3);
}

NF_TEST(test_fnv1a_data) {
    u8 data[] = {1, 2, 3, 4, 5};
    u64 h = fnv1a_64(data, 5);
    NF_CHECK(h != 0);
}

NF_TEST(test_murmur3) {
    u32 h1 = murmur3_32("test", 4, 0);
    u32 h2 = murmur3_32("test", 4, 0);
    u32 h3 = murmur3_32("Test", 4, 0);
    NF_CHECK_EQ(h1, h2);
    NF_CHECK(h1 != h3);
}

NF_TEST(test_hash_combine) {
    u64 seed = 0;
    hash_combine(seed, 1);
    hash_combine(seed, 2);
    NF_CHECK(seed != 0);
}
