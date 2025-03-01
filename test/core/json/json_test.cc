//
// Copyright 2015-2016 gRPC authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "src/core/lib/json/json.h"

#include <string.h>

#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include <grpc/support/log.h>

#include "test/core/util/test_config.h"

namespace grpc_core {

void ValidateValue(const Json& actual, const Json& expected);

void ValidateObject(const Json::Object& actual, const Json::Object& expected) {
  ASSERT_EQ(actual.size(), expected.size());
  auto actual_it = actual.begin();
  for (const auto& p : expected) {
    EXPECT_EQ(actual_it->first, p.first);
    ValidateValue(actual_it->second, p.second);
    ++actual_it;
  }
}

void ValidateArray(const Json::Array& actual, const Json::Array& expected) {
  ASSERT_EQ(actual.size(), expected.size());
  for (size_t i = 0; i < expected.size(); ++i) {
    ValidateValue(actual[i], expected[i]);
  }
}

void ValidateValue(const Json& actual, const Json& expected) {
  ASSERT_EQ(actual.type(), expected.type());
  switch (expected.type()) {
    case Json::Type::JSON_NULL:
    case Json::Type::JSON_TRUE:
    case Json::Type::JSON_FALSE:
      break;
    case Json::Type::STRING:
    case Json::Type::NUMBER:
      EXPECT_EQ(actual.string(), expected.string());
      break;
    case Json::Type::OBJECT:
      ValidateObject(actual.object(), expected.object());
      break;
    case Json::Type::ARRAY:
      ValidateArray(actual.array(), expected.array());
      break;
  }
}

void RunSuccessTest(const char* input, const Json& expected,
                    const char* expected_output) {
  gpr_log(GPR_INFO, "parsing string \"%s\" - should succeed", input);
  auto json = Json::Parse(input);
  ASSERT_TRUE(json.ok()) << json.status();
  ValidateValue(*json, expected);
  std::string output = json->Dump();
  EXPECT_EQ(output, expected_output);
}

TEST(Json, Whitespace) {
  RunSuccessTest(" 0 ", 0, "0");
  RunSuccessTest(" 1 ", 1, "1");
  RunSuccessTest(" \"    \" ", "    ", "\"    \"");
  RunSuccessTest(" \"a\" ", "a", "\"a\"");
  RunSuccessTest(" true ", true, "true");
}

TEST(Json, Utf16) {
  RunSuccessTest("\"\\u0020\\\\\\u0010\\u000a\\u000D\"", " \\\u0010\n\r",
                 "\" \\\\\\u0010\\n\\r\"");
}

MATCHER(ContainsInvalidUtf8,
        absl::StrCat(negation ? "Contains" : "Does not contain",
                     " invalid UTF-8 characters.")) {
  auto json = Json::Parse(arg);
  return json.status().code() == absl::StatusCode::kInvalidArgument &&
         absl::StrContains(json.status().message(), "JSON parsing failed");
}

TEST(Json, Utf8) {
  RunSuccessTest("\"ßâñć௵⇒\"", "ßâñć௵⇒",
                 "\"\\u00df\\u00e2\\u00f1\\u0107\\u0bf5\\u21d2\"");
  RunSuccessTest("\"\\u00df\\u00e2\\u00f1\\u0107\\u0bf5\\u21d2\"", "ßâñć௵⇒",
                 "\"\\u00df\\u00e2\\u00f1\\u0107\\u0bf5\\u21d2\"");
  // Testing UTF-8 character "𝄞", U+11D1E.
  RunSuccessTest("\"\xf0\x9d\x84\x9e\"", "\xf0\x9d\x84\x9e",
                 "\"\\ud834\\udd1e\"");
  RunSuccessTest("\"\\ud834\\udd1e\"", "\xf0\x9d\x84\x9e",
                 "\"\\ud834\\udd1e\"");
  RunSuccessTest("{\"\\ud834\\udd1e\":0}",
                 Json::Object{{"\xf0\x9d\x84\x9e", 0}},
                 "{\"\\ud834\\udd1e\":0}");

  /// For UTF-8 characters with length of 1 byte, the range of it is [0x00,
  /// 0x7f].
  EXPECT_THAT("\"\xa0\"", ContainsInvalidUtf8());

  /// For UTF-8 characters with length of 2 bytes, the range of the first byte
  /// is [0xc2, 0xdf], and the range of the second byte is [0x80, 0xbf].
  EXPECT_THAT("\"\xc0\xbc\"", ContainsInvalidUtf8());
  EXPECT_THAT("\"\xbc\xc0\"", ContainsInvalidUtf8());

  /// Corner cases for UTF-8 characters with length of 3 bytes.
  /// If the first byte is 0xe0, the range of second byte is [0xa0, 0xbf].
  EXPECT_THAT("\"\xe0\x80\x80\"", ContainsInvalidUtf8());
  /// If the first byte is 0xed, the range of second byte is [0x80, 0x9f].
  EXPECT_THAT("\"\xed\xa0\x80\"", ContainsInvalidUtf8());

  /// Corner cases for UTF-8 characters with length of 4 bytes.
  /// If the first byte is 0xf0, the range of second byte is [0x90, 0xbf].
  EXPECT_THAT("\"\xf0\x80\x80\x80\"", ContainsInvalidUtf8());
  /// If the first byte is 0xf4, the range of second byte is [0x80, 0x8f].
  EXPECT_THAT("\"\xf4\x90\x80\x80\"", ContainsInvalidUtf8());
  /// The range of the first bytes is [0xf0, 0xf4].
  EXPECT_THAT("\"\xf5\x80\x80\x80\"", ContainsInvalidUtf8());
}

TEST(Json, NestedEmptyContainers) {
  RunSuccessTest(" [ [ ] , { } , [ ] ] ",
                 Json::Array{
                     Json::Array(),
                     Json::Object(),
                     Json::Array(),
                 },
                 "[[],{},[]]");
}

TEST(Json, EscapesAndControlCharactersInKeyStrings) {
  RunSuccessTest(" { \"\\u007f\x7f\\n\\r\\\"\\f\\b\\\\a , b\": 1, \"\": 0 } ",
                 Json::Object{
                     {"\u007f\u007f\n\r\"\f\b\\a , b", 1},
                     {"", 0},
                 },
                 "{\"\":0,\"\\u007f\\u007f\\n\\r\\\"\\f\\b\\\\a , b\":1}");
}

TEST(Json, WriterCutsOffInvalidUtf8) {
  EXPECT_EQ(Json("abc\xf0\x9d\x24").Dump(), "\"abc\"");
  EXPECT_EQ(Json("\xff").Dump(), "\"\"");
}

TEST(Json, ValidNumbers) {
  RunSuccessTest("[0, 42 , 0.0123, 123.456]",
                 Json::Array{
                     0,
                     42,
                     Json("0.0123", /*is_number=*/true),
                     Json("123.456", /*is_number=*/true),
                 },
                 "[0,42,0.0123,123.456]");
  RunSuccessTest("[1e4,-53.235e-31, 0.3e+3]",
                 Json::Array{
                     Json("1e4", /*is_number=*/true),
                     Json("-53.235e-31", /*is_number=*/true),
                     Json("0.3e+3", /*is_number=*/true),
                 },
                 "[1e4,-53.235e-31,0.3e+3]");
}

TEST(Json, Keywords) {
  RunSuccessTest("[true, false, null]",
                 Json::Array{
                     Json(true),
                     Json(false),
                     Json(),
                 },
                 "[true,false,null]");
}

void RunParseFailureTest(const char* input) {
  gpr_log(GPR_INFO, "parsing string \"%s\" - should fail", input);
  auto json = Json::Parse(input);
  EXPECT_FALSE(json.ok());
}

TEST(Json, InvalidInput) {
  RunParseFailureTest("\\");
  RunParseFailureTest("nu ll");
  RunParseFailureTest("{\"foo\": bar}");
  RunParseFailureTest("{\"foo\": bar\"x\"}");
  RunParseFailureTest("fals");
  RunParseFailureTest("0,0 ");
  RunParseFailureTest("\"foo\",[]");
}

TEST(Json, UnterminatedString) { RunParseFailureTest("\"\\x"); }

TEST(Json, InvalidUtf16) {
  RunParseFailureTest("\"\\u123x");
  RunParseFailureTest("{\"\\u123x");
}

TEST(Json, ImbalancedSurrogatePairs) {
  RunParseFailureTest("\"\\ud834f");
  RunParseFailureTest("{\"\\ud834f\":0}");
  RunParseFailureTest("\"\\ud834\\n");
  RunParseFailureTest("{\"\\ud834\\n\":0}");
  RunParseFailureTest("\"\\udd1ef");
  RunParseFailureTest("{\"\\udd1ef\":0}");
  RunParseFailureTest("\"\\ud834\\ud834\"");
  RunParseFailureTest("{\"\\ud834\\ud834\"\":0}");
  RunParseFailureTest("\"\\ud834\\u1234\"");
  RunParseFailureTest("{\"\\ud834\\u1234\"\":0}");
  RunParseFailureTest("\"\\ud834]\"");
  RunParseFailureTest("{\"\\ud834]\"\":0}");
  RunParseFailureTest("\"\\ud834 \"");
  RunParseFailureTest("{\"\\ud834 \"\":0}");
  RunParseFailureTest("\"\\ud834\\\\\"");
  RunParseFailureTest("{\"\\ud834\\\\\"\":0}");
}

TEST(Json, EmbeddedInvalidWhitechars) {
  RunParseFailureTest("\"\n\"");
  RunParseFailureTest("\"\t\"");
}

TEST(Json, EmptyString) { RunParseFailureTest(""); }

TEST(Json, ExtraCharsAtEndOfParsing) {
  RunParseFailureTest("{},");
  RunParseFailureTest("{}x");
}

TEST(Json, ImbalancedContainers) {
  RunParseFailureTest("{}}");
  RunParseFailureTest("[]]");
  RunParseFailureTest("{{}");
  RunParseFailureTest("[[]");
  RunParseFailureTest("[}");
  RunParseFailureTest("{]");
}

TEST(Json, BadContainers) {
  RunParseFailureTest("{x}");
  RunParseFailureTest("{x=0,y}");
}

TEST(Json, DuplicateObjectKeys) { RunParseFailureTest("{\"x\": 1, \"x\": 1}"); }

TEST(Json, TrailingComma) {
  RunParseFailureTest("{,}");
  RunParseFailureTest("[1,2,3,4,]");
  RunParseFailureTest("{\"a\": 1, }");
}

TEST(Json, KeySyntaxInArray) { RunParseFailureTest("[\"x\":0]"); }

TEST(Json, InvalidNumbers) {
  RunParseFailureTest("1.");
  RunParseFailureTest("1e");
  RunParseFailureTest(".12");
  RunParseFailureTest("1.x");
  RunParseFailureTest("1.12x");
  RunParseFailureTest("1ex");
  RunParseFailureTest("1e12x");
  RunParseFailureTest(".12x");
  RunParseFailureTest("000");
};

TEST(Json, Equality) {
  // Null.
  EXPECT_EQ(Json(), Json());
  // Numbers.
  EXPECT_EQ(Json(1), Json(1));
  EXPECT_NE(Json(1), Json(2));
  EXPECT_EQ(Json(1), Json("1", /*is_number=*/true));
  EXPECT_EQ(Json("-5e5", /*is_number=*/true), Json("-5e5", /*is_number=*/true));
  // Booleans.
  EXPECT_EQ(Json(true), Json(true));
  EXPECT_EQ(Json(false), Json(false));
  EXPECT_NE(Json(true), Json(false));
  // Strings.
  EXPECT_EQ(Json("foo"), Json("foo"));
  EXPECT_NE(Json("foo"), Json("bar"));
  // Arrays.
  EXPECT_EQ(Json(Json::Array{"foo"}), Json(Json::Array{"foo"}));
  EXPECT_NE(Json(Json::Array{"foo"}), Json(Json::Array{"bar"}));
  // Objects.
  EXPECT_EQ(Json(Json::Object{{"foo", 1}}), Json(Json::Object{{"foo", 1}}));
  EXPECT_NE(Json(Json::Object{{"foo", 1}}), Json(Json::Object{{"foo", 2}}));
  EXPECT_NE(Json(Json::Object{{"foo", 1}}), Json(Json::Object{{"bar", 1}}));
  // Differing types.
  EXPECT_NE(Json(1), Json("foo"));
  EXPECT_NE(Json(1), Json(true));
  EXPECT_NE(Json(1), Json(Json::Array{}));
  EXPECT_NE(Json(1), Json(Json::Object{}));
  EXPECT_NE(Json(1), Json());
}

}  // namespace grpc_core

int main(int argc, char** argv) {
  grpc::testing::TestEnvironment env(&argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
