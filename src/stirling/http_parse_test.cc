#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <utility>

#include "src/stirling/http_parse.h"

namespace pl {
namespace stirling {

using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::Key;
using ::testing::Not;
using ::testing::Pair;

TEST(PreProcessRecordTest, GzipCompressedContentIsDecompressed) {
  HTTPTraceRecord record;
  record.message.http_headers[http_headers::kContentEncoding] = "gzip";
  const uint8_t compressed_bytes[] = {0x1f, 0x8b, 0x08, 0x00, 0x37, 0xf0, 0xbf, 0x5c, 0x00,
                                      0x03, 0x0b, 0xc9, 0xc8, 0x2c, 0x56, 0x00, 0xa2, 0x44,
                                      0x85, 0x92, 0xd4, 0xe2, 0x12, 0x2e, 0x00, 0x8c, 0x2d,
                                      0xc0, 0xfa, 0x0f, 0x00, 0x00, 0x00};
  record.message.http_resp_body.assign(reinterpret_cast<const char*>(compressed_bytes),
                                       sizeof(compressed_bytes));
  PreProcessHTTPRecord(&record);
  EXPECT_EQ("This is a test\n", record.message.http_resp_body);
}

TEST(PreProcessRecordTest, ContentHeaderIsNotAdded) {
  HTTPTraceRecord record;
  record.message.http_resp_body = "test";
  PreProcessHTTPRecord(&record);
  EXPECT_EQ("test", record.message.http_resp_body);
  EXPECT_THAT(record.message.http_headers, Not(Contains(Key(http_headers::kContentEncoding))));
}

TEST(ParseEventAttrTest, DataIsCopied) {
  socket_data_event_t event;
  event.attr.timestamp_ns = 100;
  event.attr.tgid = 1;
  event.attr.fd = 3;

  HTTPTraceRecord record;

  ParseEventAttr(event, &record);

  EXPECT_EQ(100, record.message.timestamp_ns);
  EXPECT_EQ(1, record.conn.tgid);
  EXPECT_EQ(3, record.conn.fd);
}

TEST(ParseRawTest, ContentIsCopied) {
  const std::string data = "test";
  socket_data_event_t event;
  event.attr.timestamp_ns = 100;
  event.attr.tgid = 1;
  event.attr.fd = 3;
  event.attr.msg_size = data.size();
  data.copy(event.msg, data.size());

  HTTPTraceRecord record;

  EXPECT_TRUE(ParseRaw(event, &record));
  EXPECT_EQ(100, record.message.timestamp_ns);
  EXPECT_EQ(1, record.conn.tgid);
  EXPECT_EQ(3, record.conn.fd);
  EXPECT_EQ(SocketTraceEventType::kUnknown, record.message.type);
  EXPECT_EQ("test", record.message.http_resp_body);
}

TEST(ParseHTTPHeaderFiltersAndMatchTest, FiltersAreAsExpectedAndMatchesWork) {
  const std::string filters_string =
      "Content-Type:json,,,,Content-Type:plain,Transfer-Encoding:chunked,,,Transfer-Encoding:"
      "chunked,-Content-Encoding:gzip,-Content-Encoding:binary";
  HTTPHeaderFilter filter = ParseHTTPHeaderFilters(filters_string);
  EXPECT_THAT(filter.inclusions,
              ElementsAre(Pair("Content-Type", "json"), Pair("Content-Type", "plain"),
                          Pair("Transfer-Encoding", "chunked"),
                          // Note that multimap does not remove duplicates.
                          Pair("Transfer-Encoding", "chunked")));
  EXPECT_THAT(filter.exclusions,
              ElementsAre(Pair("Content-Encoding", "gzip"), Pair("Content-Encoding", "binary")));
  {
    std::map<std::string, std::string> http_headers = {
        {"Content-Type", "application/json; charset=utf-8"},
    };
    EXPECT_TRUE(MatchesHTTPTHeaders(http_headers, filter));
    http_headers.insert({"Content-Encoding", "gzip"});
    EXPECT_FALSE(MatchesHTTPTHeaders(http_headers, filter)) << "gzip should be filtered out";
  }
  {
    std::map<std::string, std::string> http_headers = {
        {"Transfer-Encoding", "chunked"},
    };
    EXPECT_TRUE(MatchesHTTPTHeaders(http_headers, filter));
    http_headers.insert({"Content-Encoding", "binary"});
    EXPECT_FALSE(MatchesHTTPTHeaders(http_headers, filter)) << "binary should be filtered out";
  }
  {
    std::map<std::string, std::string> http_headers;
    EXPECT_FALSE(MatchesHTTPTHeaders(http_headers, filter));

    const HTTPHeaderFilter empty_filter;
    EXPECT_TRUE(MatchesHTTPTHeaders(http_headers, empty_filter))
        << "Empty filter matches any HTTP headers";
    http_headers.insert({"Content-Type", "non-matching-type"});
    EXPECT_TRUE(MatchesHTTPTHeaders(http_headers, empty_filter))
        << "Empty filter matches any HTTP headers";
  }
  {
    const std::map<std::string, std::string> http_headers = {
        {"Content-Type", "non-matching-type"},
    };
    EXPECT_FALSE(MatchesHTTPTHeaders(http_headers, filter));
  }
}

class HTTPParserTest : public ::testing::Test {
 protected:
  HTTPParser parser_;
};

bool operator==(const HTTPMessage& lhs, const HTTPMessage& rhs) {
#define CMP(field)                                                 \
  if (lhs.field != rhs.field) {                                    \
    LOG(INFO) << #field ": " << lhs.field << " vs. " << rhs.field; \
    return false;                                                  \
  }
  CMP(is_complete);
  CMP(http_minor_version);
  CMP(http_resp_status);
  CMP(http_resp_message);
  if (lhs.http_headers != rhs.http_headers) {
    LOG(INFO) << absl::StrJoin(std::begin(lhs.http_headers), std::end(lhs.http_headers), " ",
                               absl::PairFormatter(":"))
              << " vs. "
              << absl::StrJoin(std::begin(rhs.http_headers), std::end(rhs.http_headers), " ",
                               absl::PairFormatter(":"));
    return false;
  }
  if (lhs.type != rhs.type) {
    LOG(INFO) << static_cast<int>(lhs.type) << " vs. " << static_cast<int>(rhs.type);
  }
  return true;
}

socket_data_event_t Event(uint64_t seq_num, std::string_view msg) {
  socket_data_event_t event;
  event.attr.seq_num = seq_num;
  event.attr.msg_size = msg.size();
  msg.copy(event.msg, msg.size());
  return event;
}

TEST_F(HTTPParserTest, ParseCompleteHTTPResponseWithContentLengthHeader) {
  std::string_view msg1 = R"(HTTP/1.1 200 OK
Content-Type: foo
Content-Length: 9

pixielabs)";
  socket_data_event_t event1 = Event(0, msg1);

  std::string_view msg2 = R"(HTTP/1.1 200 OK
Content-Type: bar
Content-Length: 10

pixielabs!)";
  socket_data_event_t event2 = Event(1, msg2);

  EXPECT_EQ(ParseState::kSuccess, parser_.ParseResponse(event1));
  EXPECT_EQ(ParseState::kSuccess, parser_.ParseResponse(event2));

  HTTPMessage expected_message1;
  expected_message1.is_complete = true;
  expected_message1.type = SocketTraceEventType::kHTTPResponse;
  expected_message1.http_minor_version = 1;
  expected_message1.http_headers = {{"Content-Type", "foo"}, {"Content-Length", "9"}};
  expected_message1.http_resp_status = 200;
  expected_message1.http_resp_message = "OK";
  expected_message1.http_resp_body = "pixielabs";

  HTTPMessage expected_message2;
  expected_message2.is_complete = true;
  expected_message2.type = SocketTraceEventType::kHTTPResponse;
  expected_message2.http_minor_version = 1;
  expected_message2.http_headers = {{"Content-Type", "bar"}, {"Content-Length", "10"}};
  expected_message2.http_resp_status = 200;
  expected_message2.http_resp_message = "OK";
  expected_message2.http_resp_body = "pixielabs!";

  EXPECT_THAT(parser_.ExtractHTTPMessages(), ElementsAre(expected_message1, expected_message2));
  EXPECT_THAT(parser_.ExtractHTTPMessages(), IsEmpty());

  parser_.Append(0, 0, msg1);
  parser_.Append(1, 1, msg2);
  parser_.ParseResponses();

  EXPECT_EQ(ParseState::kSuccess, parser_.parse_state());
  EXPECT_THAT(parser_.ExtractHTTPMessages(), ElementsAre(expected_message1, expected_message2));
  EXPECT_THAT(parser_.ExtractHTTPMessages(), IsEmpty());
}

TEST_F(HTTPParserTest, ParseIncompleteHTTPResponseWithContentLengthHeader) {
  const std::string_view msg1 = R"(HTTP/1.1 200 OK
Content-Type: foo
Content-Length: 21

pixielabs)";
  socket_data_event_t event1 = Event(0, msg1);

  const std::string_view msg2 = " is awesome";
  socket_data_event_t event2 = Event(1, msg2);

  const std::string_view msg3 = "!";
  socket_data_event_t event3 = Event(2, msg3);

  EXPECT_EQ(ParseState::kNeedsMoreData, parser_.ParseResponse(event1));
  EXPECT_THAT(parser_.ExtractHTTPMessages(), IsEmpty());
  EXPECT_EQ(ParseState::kNeedsMoreData, parser_.ParseResponse(event2));
  EXPECT_THAT(parser_.ExtractHTTPMessages(), IsEmpty());
  EXPECT_EQ(ParseState::kSuccess, parser_.ParseResponse(event3));

  HTTPMessage expected_message1;
  expected_message1.is_complete = true;
  expected_message1.type = SocketTraceEventType::kHTTPResponse;
  expected_message1.http_minor_version = 1;
  expected_message1.http_headers = {{"Content-Type", "foo"}, {"Content-Length", "21"}};
  expected_message1.http_resp_status = 200;
  expected_message1.http_resp_message = "OK";
  expected_message1.http_resp_body = "pixielabs is awesome!";

  EXPECT_THAT(parser_.ExtractHTTPMessages(), ElementsAre(expected_message1));
  EXPECT_THAT(parser_.ExtractHTTPMessages(), IsEmpty());

  parser_.Append(0, 0, msg1);
  parser_.Append(1, 1, msg2);
  parser_.Append(2, 2, msg3);
  parser_.ParseResponses();

  EXPECT_EQ(ParseState::kSuccess, parser_.parse_state());
  EXPECT_THAT(parser_.ExtractHTTPMessages(), ElementsAre(expected_message1));
  EXPECT_THAT(parser_.ExtractHTTPMessages(), IsEmpty());
}

TEST_F(HTTPParserTest, InvalidInput) {
  {
    socket_data_event_t event;
    event.attr.seq_num = 0;
    const std::string_view msg = " is awesome";
    msg.copy(event.msg, msg.size());
    EXPECT_EQ(ParseState::kInvalid, parser_.ParseResponse(event));

    parser_.Append(0, 0, msg);
    parser_.ParseResponses();

    EXPECT_EQ(ParseState::kInvalid, parser_.parse_state());
  }
  {
    socket_data_event_t event;
    event.attr.seq_num = 2;
    const std::string_view msg = " is awesome";
    msg.copy(event.msg, msg.size());
    EXPECT_EQ(ParseState::kUnknown, parser_.ParseResponse(event));
  }
}

// Leave http_resp_body set by caller.
HTTPMessage ExpectMessage() {
  HTTPMessage result;
  result.is_complete = true;
  result.type = SocketTraceEventType::kHTTPResponse;
  result.http_minor_version = 1;
  result.http_headers = {{"Transfer-Encoding", "chunked"}};
  result.http_resp_status = 200;
  result.http_resp_message = "OK";
  return result;
}

TEST_F(HTTPParserTest, ParseComplteChunkEncodedMessage) {
  std::string msg = R"(HTTP/1.1 200 OK
Transfer-Encoding: chunked

9
pixielabs
C
 is awesome!
0

)";
  socket_data_event_t event = Event(0, msg);

  HTTPMessage expected_message = ExpectMessage();
  expected_message.http_resp_body = "pixielabs is awesome!";

  EXPECT_EQ(ParseState::kSuccess, parser_.ParseResponse(event));
  EXPECT_THAT(parser_.ExtractHTTPMessages(), ElementsAre(expected_message));
  EXPECT_THAT(parser_.ExtractHTTPMessages(), IsEmpty());

  parser_.Append(0, 0, msg);
  parser_.ParseResponses();

  EXPECT_EQ(ParseState::kSuccess, parser_.parse_state());
  EXPECT_THAT(parser_.ExtractHTTPMessages(), ElementsAre(expected_message));
  EXPECT_THAT(parser_.ExtractHTTPMessages(), IsEmpty());
}

TEST_F(HTTPParserTest, ParseMultipleChunks) {
  std::string msg1 = R"(HTTP/1.1 200 OK
Transfer-Encoding: chunked

9
pixielabs
)";
  socket_data_event_t event1 = Event(0, msg1);

  std::string msg2 = "C\r\n is awesome!\r\n";
  socket_data_event_t event2 = Event(1, msg2);

  std::string msg3 = "0\r\n\r\n";
  socket_data_event_t event3 = Event(2, msg3);

  HTTPMessage expected_message = ExpectMessage();
  expected_message.http_resp_body = "pixielabs is awesome!";

  EXPECT_EQ(ParseState::kNeedsMoreData, parser_.ParseResponse(event1));
  EXPECT_THAT(parser_.ExtractHTTPMessages(), IsEmpty());
  EXPECT_EQ(ParseState::kNeedsMoreData, parser_.ParseResponse(event2));
  EXPECT_THAT(parser_.ExtractHTTPMessages(), IsEmpty());
  EXPECT_EQ(ParseState::kSuccess, parser_.ParseResponse(event3));
  EXPECT_THAT(parser_.ExtractHTTPMessages(), ElementsAre(expected_message));
  EXPECT_THAT(parser_.ExtractHTTPMessages(), IsEmpty()) << "Data should be empty after extraction";

  parser_.Append(0, 0, msg1);
  parser_.Append(1, 1, msg2);
  parser_.Append(2, 2, msg3);
  parser_.ParseResponses();

  EXPECT_EQ(ParseState::kSuccess, parser_.parse_state());
  EXPECT_THAT(parser_.ExtractHTTPMessages(), ElementsAre(expected_message));
  EXPECT_THAT(parser_.ExtractHTTPMessages(), IsEmpty()) << "Data should be empty after extraction";
}

TEST_F(HTTPParserTest, ParseIncompleteChunks) {
  std::string msg1 = R"(HTTP/1.1 200 OK
Transfer-Encoding: chunked

9
pixie)";
  socket_data_event_t event1 = Event(0, msg1);

  std::string msg2 = "labs\r\n";
  socket_data_event_t event2 = Event(1, msg2);

  std::string msg3 = "0\r\n\r\n";
  socket_data_event_t event3 = Event(2, msg3);

  HTTPMessage expected_message = ExpectMessage();
  expected_message.http_resp_body = "pixielabs";

  EXPECT_EQ(ParseState::kNeedsMoreData, parser_.ParseResponse(event1));
  EXPECT_THAT(parser_.ExtractHTTPMessages(), IsEmpty());
  EXPECT_EQ(ParseState::kNeedsMoreData, parser_.ParseResponse(event2));
  EXPECT_THAT(parser_.ExtractHTTPMessages(), IsEmpty());
  EXPECT_EQ(ParseState::kSuccess, parser_.ParseResponse(event3));
  EXPECT_THAT(parser_.ExtractHTTPMessages(), ElementsAre(expected_message));
  EXPECT_THAT(parser_.ExtractHTTPMessages(), IsEmpty()) << "Data should be empty after extraction";

  parser_.Append(0, 0, msg1);
  parser_.Append(1, 1, msg2);
  parser_.Append(2, 2, msg3);
  parser_.ParseResponses();

  EXPECT_EQ(ParseState::kSuccess, parser_.parse_state());
  EXPECT_THAT(parser_.ExtractHTTPMessages(), ElementsAre(expected_message));
  EXPECT_THAT(parser_.ExtractHTTPMessages(), IsEmpty()) << "Data should be empty after extraction";
}

TEST_F(HTTPParserTest, ParseMessagesWithoutLengthOrChunking) {
  std::string msg1 = R"(HTTP/1.1 200 OK

pixielabs )";
  socket_data_event_t event1 = Event(0, msg1);

  std::string msg2 = "is ";
  socket_data_event_t event2 = Event(1, msg2);

  std::string msg3 = "awesome!";
  socket_data_event_t event3 = Event(2, msg3);

  HTTPMessage expected_message = ExpectMessage();
  expected_message.http_headers.clear();
  expected_message.http_resp_body = "pixielabs is awesome!";

  EXPECT_EQ(ParseState::kNeedsMoreData, parser_.ParseResponse(event1));
  EXPECT_THAT(parser_.ExtractHTTPMessages(), IsEmpty());
  EXPECT_EQ(ParseState::kNeedsMoreData, parser_.ParseResponse(event2));
  EXPECT_THAT(parser_.ExtractHTTPMessages(), IsEmpty());
  EXPECT_EQ(ParseState::kNeedsMoreData, parser_.ParseResponse(event3));
  parser_.Close();
  EXPECT_THAT(parser_.ExtractHTTPMessages(), ElementsAre(expected_message));
  EXPECT_THAT(parser_.ExtractHTTPMessages(), IsEmpty()) << "Data should be empty after extraction";

  parser_.Append(0, 0, msg1);
  parser_.Append(1, 1, msg2);
  parser_.Append(2, 2, msg3);
  parser_.ParseResponses();

  EXPECT_EQ(ParseState::kSuccess, parser_.parse_state());
  parser_.Close();
  EXPECT_THAT(parser_.ExtractHTTPMessages(), ElementsAre(expected_message));
  EXPECT_THAT(parser_.ExtractHTTPMessages(), IsEmpty()) << "Data should be empty after extraction";
}

std::string HTTPRespWithSizedBody(std::string_view body) {
  return absl::Substitute(
      "HTTP/1.1 200 OK\r\n"
      "Content-Length: $0\r\n"
      "\r\n"
      "$1",
      body.size(), body);
}

std::string HTTPChunk(std::string_view chunk_body) {
  const char size = chunk_body.size() < 10 ? '0' + chunk_body.size() : 'A' + chunk_body.size() - 10;
  std::string s(1, size);
  return absl::StrCat(s, "\r\n", chunk_body, "\r\n");
}

std::string HTTPRespWithChunkedBody(std::vector<std::string_view> chunk_bodys) {
  std::string result =
      "HTTP/1.1 200 OK\r\n"
      "Transfer-Encoding: chunked\r\n"
      "\r\n";
  for (auto c : chunk_bodys) {
    absl::StrAppend(&result, HTTPChunk(c));
  }
  // Lastly, append a 0-length chunk.
  absl::StrAppend(&result, HTTPChunk(""));
  return result;
}

TEST_F(HTTPParserTest, MessagePartialHeaders) {
  std::string msg1 = R"(HTTP/1.1 200 OK
Content-Type: text/plain)";
  socket_data_event_t event1 = Event(0, msg1);
  EXPECT_EQ(ParseState::kNeedsMoreData, parser_.ParseResponse(event1));
  parser_.Close();
  EXPECT_THAT(parser_.ExtractHTTPMessages(), IsEmpty());

  parser_.Append(0, 0, msg1);
  parser_.ParseResponses();

  EXPECT_EQ(ParseState::kNeedsMoreData, parser_.parse_state());
  EXPECT_THAT(parser_.ExtractHTTPMessages(), IsEmpty());
}

MATCHER_P(HasBody, body, "") {
  LOG(INFO) << "Got: " << arg.http_resp_body;
  return arg.http_resp_body == body;
}

TEST_F(HTTPParserTest, PartialMessageInTheMiddleOfStream) {
  std::string msg0 = HTTPRespWithSizedBody("foobar") + "HTTP/1.1 200 OK\r\n";
  std::string msg1 = "Transfer-Encoding: chunked\r\n\r\n";
  std::string msg2 = HTTPChunk("pixielabs ");
  std::string msg3 = HTTPChunk("rocks!");
  std::string msg4 = HTTPChunk("");

  parser_.Append(0, 0, msg0);
  parser_.Append(1, 1, msg1);
  parser_.Append(2, 2, msg2);
  parser_.Append(3, 3, msg3);
  parser_.Append(4, 4, msg4);
  parser_.ParseResponses();

  EXPECT_EQ(ParseState::kSuccess, parser_.parse_state());
  EXPECT_THAT(parser_.ExtractHTTPMessages(),
              ElementsAre(HasBody("foobar"), HasBody("pixielabs rocks!")));
}

TEST_F(HTTPParserTest, AppendNonContiguousMessage) {
  EXPECT_TRUE(parser_.Append(1, 0, ""));
  EXPECT_FALSE(parser_.Append(3, 0, ""));
}

MATCHER_P2(HasBytesRange, begin, end, "") {
  LOG(INFO) << "Got: " << arg.bytes_begin << ", " << arg.bytes_end;
  return arg.bytes_begin == static_cast<size_t>(begin) && arg.bytes_end == static_cast<size_t>(end);
}

TEST(ParseTest, CompleteMessages) {
  std::string msg_a = HTTPRespWithSizedBody("a");
  std::string msg_b = HTTPRespWithChunkedBody({"b"});
  std::string msg_c = HTTPRespWithSizedBody("c");
  std::string buf = absl::StrCat(msg_a, msg_b, msg_c);

  std::vector<SeqHTTPMessage> messages;
  EXPECT_EQ(std::make_pair(ParseState::kSuccess, msg_a.size() + msg_b.size() + msg_c.size()),
            Parse(buf, &messages));
  EXPECT_THAT(messages, ElementsAre(HasBody("a"), HasBody("b"), HasBody("c")));
  EXPECT_THAT(messages, ElementsAre(HasBytesRange(0, msg_a.size()),
                                    HasBytesRange(msg_a.size(), msg_a.size() + msg_b.size()),
                                    HasBytesRange(msg_a.size() + msg_b.size(),
                                                  msg_a.size() + msg_b.size() + msg_c.size())));
}

TEST(ParseTest, PartialMessages) {
  std::string msg =
      "HTTP/1.1 200 OK\r\n"
      "Content-Length: $0\r\n";

  std::vector<SeqHTTPMessage> messages;
  constexpr size_t kZero = 0;
  EXPECT_EQ(std::make_pair(ParseState::kNeedsMoreData, kZero), Parse(msg, &messages));
  EXPECT_THAT(messages, IsEmpty());
}

}  // namespace stirling
}  // namespace pl
