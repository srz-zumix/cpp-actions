#include "iutest.hpp"

#include "util/sha256.hpp"

using namespace cpa::util;

IUTEST(Sha256Test, KnownVectors) {
  IUTEST_EXPECT_EQ("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", sha256_hex(""));
  IUTEST_EXPECT_EQ("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", sha256_hex("abc"));
  IUTEST_EXPECT_EQ("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
                   sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"));
  std::string million(1000000, 'a');
  IUTEST_EXPECT_EQ("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", sha256_hex(million));
}
