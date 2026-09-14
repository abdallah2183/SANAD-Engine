// CoreTests/test_uuid.cpp

#include <NF/Test/TestFramework.hpp>
#include <NF/Core/UUID.hpp>

using namespace nf;

NF_TEST(test_uuid_generate) {
    UUID a = UUID::generate();
    UUID b = UUID::generate();
    NF_CHECK(a.is_valid());
    NF_CHECK(b.is_valid());
    NF_CHECK(a != b);
}

NF_TEST(test_uuid_to_string) {
    UUID a = UUID::generate();
    std::string s = a.to_string();
    NF_CHECK_EQ(s.length(), usize(36)); // 8-4-4-4-12 with dashes

    // Version 4
    NF_CHECK(s[14] == '4');
    // Variant: 8, 9, a, or b
    char variant = s[19];
    NF_CHECK(variant == '8' || variant == '9' || variant == 'a' || variant == 'b');
}

NF_TEST(test_uuid_from_string) {
    std::string str = "550e8400-e29b-41d4-a716-446655440000";
    UUID a = UUID::from_string(str);
    NF_CHECK(a.is_valid());

    std::string back = a.to_string();
    NF_CHECK_EQ(str, back);
}
