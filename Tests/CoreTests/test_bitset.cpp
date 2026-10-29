#include <NF/Test/TestFramework.hpp>
#include <NF/Core/BitSet.hpp>

using namespace nf;

NF_TEST(bitset_fixed_basic_ops) {
    BitSet<128> bs;
    NF_CHECK_EQ(bs.size(), 128u);
    NF_CHECK(bs.none());
    NF_CHECK(!bs.any());
    NF_CHECK_EQ(bs.count(), 0u);

    bs.set(10);
    bs.set(70); // in second 64-bit word
    NF_CHECK(bs.test(10));
    NF_CHECK(bs.test(70));
    NF_CHECK(!bs.test(11));
    NF_CHECK_EQ(bs.count(), 2u);
    NF_CHECK(bs.any());

    bs.reset(10);
    NF_CHECK(!bs.test(10));
    NF_CHECK_EQ(bs.count(), 1u);

    bs.flip(70);
    NF_CHECK(!bs.test(70));
    NF_CHECK(bs.none());
}

NF_TEST(bitset_fixed_find_first_and_next) {
    BitSet<128> bs;
    bs.set(5);
    bs.set(42);
    bs.set(65); // crosses 64-bit boundary
    bs.set(120);

    size_t first = bs.find_first();
    NF_CHECK_EQ(first, 5u);

    size_t next1 = bs.find_next(first);
    NF_CHECK_EQ(next1, 42u);

    size_t next2 = bs.find_next(next1);
    NF_CHECK_EQ(next2, 65u);

    size_t next3 = bs.find_next(next2);
    NF_CHECK_EQ(next3, 120u);

    size_t next4 = bs.find_next(next3);
    NF_CHECK_EQ(next4, BitSet<128>::npos);
}

NF_TEST(bitset_fixed_bitwise_logic) {
    BitSet<64> a;
    BitSet<64> b;
    a.set(0);
    a.set(1);
    b.set(1);
    b.set(2);

    auto and_res = a & b;
    NF_CHECK_EQ(and_res.count(), 1u);
    NF_CHECK(and_res.test(1));

    auto or_res = a | b;
    NF_CHECK_EQ(or_res.count(), 3u);
    NF_CHECK(or_res.test(0) && or_res.test(1) && or_res.test(2));

    auto xor_res = a ^ b;
    NF_CHECK_EQ(xor_res.count(), 2u);
    NF_CHECK(xor_res.test(0) && xor_res.test(2) && !xor_res.test(1));
}

NF_TEST(dynamic_bitset_operations) {
    DynamicBitSet dbs(100);
    NF_CHECK_EQ(dbs.size(), 100u);
    NF_CHECK(dbs.none());

    dbs.set(3);
    dbs.set(67);
    dbs.set(99);
    NF_CHECK_EQ(dbs.count(), 3u);
    NF_CHECK(dbs.test(3));
    NF_CHECK(dbs.test(67));
    NF_CHECK(dbs.test(99));

    size_t f = dbs.find_first();
    NF_CHECK_EQ(f, 3u);
    size_t n1 = dbs.find_next(f);
    NF_CHECK_EQ(n1, 67u);
    size_t n2 = dbs.find_next(n1);
    NF_CHECK_EQ(n2, 99u);
    size_t n3 = dbs.find_next(n2);
    NF_CHECK_EQ(n3, DynamicBitSet::npos);

    // Resize expanding
    dbs.resize(150, false);
    NF_CHECK_EQ(dbs.size(), 150u);
    NF_CHECK_EQ(dbs.count(), 3u);
    dbs.set(149);
    NF_CHECK(dbs.test(149));
    NF_CHECK_EQ(dbs.count(), 4u);
}
