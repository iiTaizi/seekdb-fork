/*
 * Copyright (c) 2026 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gtest/gtest.h>
#include <limits>
#include <string>
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"

using namespace oceanbase::common;

namespace {

class AIFunctionValidation : public ::testing::Test
{
protected:
  ObIJsonBase *parse(const char *text)
  {
    ObIJsonBase *node = nullptr;
    const int ret = ObJsonBaseFactory::get_json_base(&allocator_, ObString(text),
        ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, node);
    EXPECT_EQ(OB_SUCCESS, ret);
    return node;
  }

  ObJsonObject *object(const char *text)
  {
    ObIJsonBase *node = parse(text);
    if (nullptr == node || node->json_type() != ObJsonNodeType::J_OBJECT) {
      ADD_FAILURE() << "expected object";
      return nullptr;
    }
    return static_cast<ObJsonObject *>(node);
  }

  ObJsonArray *array(const char *text)
  {
    ObIJsonBase *node = parse(text);
    if (nullptr == node || node->json_type() != ObJsonNodeType::J_ARRAY) {
      ADD_FAILURE() << "expected array";
      return nullptr;
    }
    return static_cast<ObJsonArray *>(node);
  }

  std::string string(const ObString &text)
  {
    return std::string(text.ptr(), text.length());
  }

  ObArenaAllocator allocator_;
};

TEST_F(AIFunctionValidation, repeated_long_argument_and_literal_braces)
{
  ObJsonObject *prompt = object("{\"template\":\"{0}/{0}/{0}\",\"args\":[\"x\"]}");
  ASSERT_NE(nullptr, prompt);
  ObJsonArray *args = static_cast<ObJsonArray *>(prompt->get_value("args"));
  std::string long_text(8192, 'x');
  ObString text(long_text.size(), long_text.data());
  ObJsonString *value = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_string(allocator_, text, value));
  ASSERT_EQ(OB_SUCCESS, args->replace(args->get_value(0), value));
  ObString result;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(
      allocator_, prompt, result));
  EXPECT_EQ(long_text + "/" + long_text + "/" + long_text, string(result));
  prompt = object("{\"template\":\"{0}/{0}\",\"args\":[\"{1}\"]}");
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(
      allocator_, prompt, result));
  EXPECT_EQ("{1}/{1}", string(result));
}

TEST_F(AIFunctionValidation, structured_and_nested_prompt)
{
  ObJsonObject *prompt = object(
      "{\"template\":\"{0}/{1}/{2}\",\"args\":[[1,true],{\"k\":2},"
      "{\"template\":\"inner:{0}\",\"args\":[\"world\"]}]}");
  ASSERT_NE(nullptr, prompt);
  ObString result;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(
      allocator_, prompt, result));
  EXPECT_EQ("[1, true]/{\"k\": 2}/inner:world", string(result));
}

TEST_F(AIFunctionValidation, invalid_placeholder_indices)
{
  for (const char *text : {
      "{\"template\":\"{1}\",\"args\":[\"x\"]}",
      "{\"template\":\"{999999999999999999999999999999}\",\"args\":[\"x\"]}"}) {
    ObJsonObject *prompt = object(text);
    ASSERT_NE(nullptr, prompt);
    ObString result;
    EXPECT_EQ(OB_INVALID_ARGUMENT,
        ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator_, prompt, result));
  }
}

TEST_F(AIFunctionValidation, prompt_preserves_unicode_nul_and_json_null)
{
  ObJsonObject *prompt = object(
      "{\"template\":\"{0}/{1}/{2}\",\"args\":[\"\\u4e16\\u754c\",\"\\u0000x\",null]}");
  ASSERT_NE(nullptr, prompt);
  ObString result;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(
      allocator_, prompt, result));
  std::string expected = "\xe4\xb8\x96\xe7\x95\x8c/";
  expected.append("\0x", 2);
  expected += "/null";
  EXPECT_EQ(expected, string(result));
}

TEST_F(AIFunctionValidation, embedding_restores_index_order)
{
  ObJsonObject *response = object("{\"data\":[{\"index\":1,\"embedding\":[20,21]},"
      "{\"index\":0,\"embedding\":[10,11]}]}");
  ASSERT_NE(nullptr, response);
  ObOpenAIUtils::ObOpenAIEmbed provider;
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, provider.parse_output(allocator_, response, result));
  ASSERT_NE(nullptr, result);
  ASSERT_EQ(ObJsonNodeType::J_ARRAY, result->json_type());
  ObJsonArray *vectors = static_cast<ObJsonArray *>(result);
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::validate_embeddings(vectors, 2, 2));
  EXPECT_EQ(10, static_cast<ObJsonArray *>(vectors->get_value(0))->get_value(0)->get_int());
  EXPECT_EQ(20, static_cast<ObJsonArray *>(vectors->get_value(1))->get_value(0)->get_int());
  EXPECT_NE(OB_SUCCESS, ObAIFuncUtils::validate_embeddings(vectors, 1, 2));
  EXPECT_NE(OB_SUCCESS, ObAIFuncUtils::validate_embeddings(vectors, 2, 3));
}

TEST_F(AIFunctionValidation, embedding_rejects_malformed_responses)
{
  ObOpenAIUtils::ObOpenAIEmbed provider;
  for (const char *text : {
      "{\"data\":{}}", "{\"data\":[]}",
      "{\"data\":[{\"embedding\":[1]}]}",
      "{\"data\":[{\"index\":-1,\"embedding\":[1]}]}",
      "{\"data\":[{\"index\":0,\"embedding\":[1]},{\"index\":0,\"embedding\":[2]}]}",
      "{\"data\":[{\"index\":0,\"embedding\":[true]}]}",
      "{\"data\":[{\"index\":0,\"embedding\":[\"1\"]}]}",
      "{\"data\":[{\"index\":0,\"embedding\":[1]},{\"index\":1,\"embedding\":[2,3]}]}"}) {
    ObJsonObject *response = object(text);
    ASSERT_NE(nullptr, response);
    ObIJsonBase *result = nullptr;
    EXPECT_NE(OB_SUCCESS, provider.parse_output(allocator_, response, result)) << text;
  }
  ObJsonDouble infinity(std::numeric_limits<double>::infinity());
  double value = 0;
  EXPECT_NE(OB_SUCCESS, ObAIFuncUtils::get_finite_number(&infinity, value));
}

TEST_F(AIFunctionValidation, completion_rejects_empty_or_nonstring_content)
{
  ObOpenAIUtils::ObOpenAIComplete provider;
  for (const char *text : {"{}", "{\"choices\":[]}",
      "{\"choices\":[{\"message\":{\"content\":\"\"}}]}",
      "{\"choices\":[{\"message\":{\"content\":null}}]}",
      "{\"choices\":[{\"message\":{\"content\":42}}]}"}) {
    ObIJsonBase *result = nullptr;
    EXPECT_NE(OB_SUCCESS, provider.parse_output(allocator_, object(text), result));
  }
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, provider.parse_output(allocator_,
      object("{\"choices\":[{\"message\":{\"content\":\"ok\"}}]}"), result));
  ASSERT_NE(nullptr, result);
  EXPECT_EQ(2, result->get_data_length());
}

TEST_F(AIFunctionValidation, ollama_native_protocol)
{
  ObOllamaUtils::ObOllamaComplete complete;
  ObString model("arbitrary-model");
  ObString system;
  ObString prompt("hello");
  ObJsonObject *body = nullptr;
  ASSERT_EQ(OB_SUCCESS, complete.get_body(allocator_, model, system, prompt,
      object("{\"temperature\":0.2,\"max_tokens\":16}"), body));
  ASSERT_NE(nullptr, body);
  ASSERT_NE(nullptr, body->get_value("stream"));
  ASSERT_EQ(ObJsonNodeType::J_BOOLEAN, body->get_value("stream")->json_type());
  EXPECT_FALSE(body->get_value("stream")->get_boolean());
  EXPECT_EQ(nullptr, body->get_value("messages"));
  ObJsonNode *options = body->get_value("options");
  ASSERT_NE(nullptr, options);
  ASSERT_EQ(ObJsonNodeType::J_OBJECT, options->json_type());
  EXPECT_EQ(16, static_cast<ObJsonObject *>(options)->get_value("num_predict")->get_int());
  ObOllamaUtils::ObOllamaEmbed embed;
  ObArray<ObString> inputs;
  ASSERT_EQ(OB_SUCCESS, inputs.push_back(ObString("first")));
  ASSERT_EQ(OB_SUCCESS, inputs.push_back(ObString("second")));
  ASSERT_EQ(OB_SUCCESS, embed.get_body(allocator_, model, inputs, nullptr, body));
  ASSERT_NE(nullptr, body->get_value("input"));
  EXPECT_EQ(2, body->get_value("input")->element_count());
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, embed.parse_output(allocator_,
      object("{\"embeddings\":[[1,2],[3,4]]}"), result));
  ASSERT_NE(nullptr, result);
  EXPECT_EQ(OB_SUCCESS, ObAIFuncUtils::validate_embeddings(static_cast<ObJsonArray *>(result), 2, 2));
}

TEST_F(AIFunctionValidation, completion_config_validation)
{
  EXPECT_EQ(OB_SUCCESS, ObAIFuncUtils::validate_complete_config(nullptr));
  EXPECT_EQ(OB_SUCCESS, ObAIFuncUtils::validate_complete_config(object("{}")));
  EXPECT_EQ(OB_SUCCESS, ObAIFuncUtils::validate_complete_config(
      object("{\"temperature\":0,\"max_tokens\":16}")));
  for (const char *text : {"{\"temperature\":true}", "{\"temperature\":\"0.2\"}",
      "{\"max_tokens\":0}", "{\"max_tokens\":1.5}", "{\"max_tokens\":null}",
      "{\"model\":\"override\"}", "{\"messages\":[]}", "{\"stream\":true}"}) {
    EXPECT_NE(OB_SUCCESS, ObAIFuncUtils::validate_complete_config(object(text))) << text;
  }
}

TEST_F(AIFunctionValidation, rerank_index_and_score_validation)
{
  EXPECT_EQ(OB_SUCCESS, ObAIFuncUtils::validate_rerank(array(
      "[{\"index\":1,\"relevance_score\":1},{\"index\":0,\"relevance_score\":0.25}]"), 2));
  for (const char *text : {"[]",
      "[{\"index\":0,\"relevance_score\":1},{\"index\":0,\"relevance_score\":2}]",
      "[{\"index\":-1,\"relevance_score\":1},{\"index\":1,\"relevance_score\":2}]",
      "[{\"index\":0,\"relevance_score\":null},{\"index\":1,\"relevance_score\":2}]"}) {
    EXPECT_NE(OB_SUCCESS, ObAIFuncUtils::validate_rerank(array(text), 2));
  }
}

} // namespace

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
