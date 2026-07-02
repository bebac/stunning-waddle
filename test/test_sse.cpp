#include "doctest/doctest.h"
#include "http/sse.h"
#include <vector>
#include <string>
#include <cstdint>

namespace
{
  // Helper to create byte span from string
  std::vector<std::byte> make_sse_data(std::string_view str)
  {
    return std::vector<std::byte>(
        reinterpret_cast<const std::byte*>(str.data()), reinterpret_cast<const std::byte*>(str.data() + str.size()));
  }

  // Helper to create byte span from literal
  template <typename... Ts> auto make_bytes(Ts... args) {
    return std::vector<std::byte>{std::byte(static_cast<unsigned char>(args))...};
  }
} // namespace

TEST_CASE("SSE Event Structure")
{
  http::sse_event event;

  SUBCASE("Default constructed event has empty fields")
  {
    CHECK(event.id.empty());
    CHECK(event.type.empty());
    CHECK(event.data.empty());
    CHECK(event.retry.has_value() == false);
  }

  SUBCASE("Event clear resets all fields")
  {
    event.id = "test-id";
    event.type = "test-type";
    event.data = "test-data";
    event.retry = std::chrono::milliseconds(1000);

    event.clear();

    CHECK(event.id.empty());
    CHECK(event.type.empty());
    CHECK(event.data.empty());
    CHECK(event.retry.has_value() == false);
  }
}

TEST_CASE("SSE Parser - Basic Event Parsing")
{
  http::sse_parser parser;

  SUBCASE("Parse simple event with data field only")
  {
    auto events_count = 0u;
    auto input = make_sse_data("data: Hello, World!\n\n");

    parser.parse(
      std::span(input),
      [&](auto event) {
        CHECK(event.id.empty());
        CHECK(event.type.empty());
        CHECK(event.data == "Hello, World!");
        CHECK(event.retry.has_value() == false);
        events_count++;
      }
    );

    CHECK(events_count == 1u);
  }

  SUBCASE("Parse event with all fields")
  {
    auto events_count = 0u;
    auto input = make_sse_data("data: test data\nid: 123\nevent: test-event\nretry: 5000\n\n");

    parser.parse(
      std::span(input),
      [&](auto event) {
        CHECK(event.id == "123");
        CHECK(event.type == "test-event");
        CHECK(event.data == "test data");
        CHECK(event.retry.has_value());
        CHECK(event.retry->count() == 5000);
        events_count++;
      }
    );

    CHECK(events_count == 1u);
  }

  SUBCASE("Parse event with UTF-8 BOM")
  {
    auto events_count = 0u;
    // UTF-8 BOM: 0xEF 0xBB 0xBF
    auto input = make_bytes(0xEF, 0xBB, 0xBF, 'd', 'a', 't', 'a', ':', ' ', 't', 'e', 's', 't', '\n', '\n');

    parser.parse(
      std::span(input),
      [&](auto event) {
        CHECK(event.data == "test");
        events_count++;
      }
    );

    CHECK(events_count == 1u);
  }

  SUBCASE("Parse multiple data fields concatenated with newlines")
  {
    auto events_count = 0u;
    auto input = make_sse_data("data: YHOO\ndata: +2\ndata: 10\n\n");

    parser.parse(
      std::span(input),
      [&](auto event) {
        CHECK(event.data == "YHOO\n+2\n10");
        events_count++;
      }
    );

    CHECK(events_count == 1u);
  }
}

TEST_CASE("SSE Parser - Field Variations")
{
  http::sse_parser parser;

  SUBCASE("Field without value")
  {
    auto events_count = 0u;
    auto input = make_sse_data("data\n\n");

    parser.parse(
      std::span(input),
      [&](auto event) {
        CHECK(event.data.empty());
        events_count++;
      }
    );

    CHECK(events_count == 1u);
  }

  SUBCASE("Field with space after colon is stripped")
  {
    auto events_count = 0u;
    auto input = make_sse_data("data: test\n\n");

    parser.parse(
      std::span(input),
      [&](auto event) {
        CHECK(event.data == "test");
        events_count++;
      }
    );

    CHECK(events_count == 1u);
  }

  SUBCASE("Field without space after colon")
  {
    auto events_count = 0u;
    auto input = make_sse_data("data:test\n\n");

    parser.parse(
      std::span(input),
      [&](auto event) {
        CHECK(event.data == "test");
        events_count++;
      }
    );

    CHECK(events_count == 1u);
  }

  SUBCASE("Field with multiple spaces after colon")
  {
    auto events_count = 0u;
    auto input = make_sse_data("data:  test\n\n"); // Two spaces

    parser.parse(
      std::span(input),
      [&](auto event) {
        CHECK(event.data == " test"); // Leading space preserved
        events_count++;
      }
    );
    CHECK(events_count == 1u);
  }

  SUBCASE("ID field with NULL is ignored")
  {
    auto events_count = 0u;
    auto input = make_bytes(
      'i', 'd', ':', ' ', 't', 'e', 's', 't', '\0', 'i', 'd', '\n', '\n',
      'd', 'a', 't', 'a', ':', ' ', 't', 'e', 's', 't', '\n', '\n'
    );

    parser.parse(
      std::span(input),
      [&](auto event) {
        if (events_count == 0) {
          CHECK(event.id.empty());  // First event with NULL in id
        } else {
          CHECK(event.id == "test");  // ID persists from previous valid id
          CHECK(event.data == "test");
        }
        events_count++;
      }
    );

    CHECK(events_count == 1u);
  }

  SUBCASE("Retry field with non-digit is ignored")
  {
    auto events_count = 0u;
    auto input = make_sse_data("retry: abc\n\ndata: test\n\n");

    parser.parse(
      std::span(input),
      [&](auto event) {
        CHECK(event.retry.has_value() == false);
        CHECK(event.data == "test");
        events_count++;
      }
    );

    CHECK(events_count == 1u);
  }

  SUBCASE("Retry field with valid digits")
  {
    auto events_count = 0u;
    auto input = make_sse_data("retry: 10000\n\ndata: test\n\n");

    parser.parse(
      std::span(input),
      [&](auto event) {
        CHECK(event.retry.has_value());
        CHECK(event.retry->count() == 10000);
        CHECK(event.data == "test");
        events_count++;
      }
    );

    CHECK(events_count == 1u);
  }

  SUBCASE("Retry persists across events") {
    auto input = make_sse_data("retry: 5000\ndata: first\n\ndata: second\n\n");

    std::vector<http::sse_event> events;
    parser.parse(
      std::span(input),
      [&](auto event) {
        events.push_back(event);
      }
    );

    CHECK(events.size() == 2u);
    CHECK(events[0].retry.has_value());
    CHECK(events[0].retry->count() == 5000);
    CHECK(events[1].retry.has_value()); // Retry persists
    CHECK(events[1].retry->count() == 5000);
  }
}

TEST_CASE("SSE Parser - Line Endings")
{
  http::sse_parser parser;

  SUBCASE("LF line ending")
  {
    auto events_count = 0u;
    auto input = make_sse_data("data: test\n\n");

    parser.parse(
      std::span(input),
      [&](auto event) {
        CHECK(event.data == "test");
        events_count++;
      }
    );

    CHECK(events_count == 1u);
  }

  SUBCASE("CR line ending")
  {
    auto events_count = 0u;
    auto input = make_sse_data("data: test\r\r");

    parser.parse(
      std::span(input),
      [&](auto event) {
        CHECK(event.data == "test");
        events_count++;
      }
    );

    CHECK(events_count == 1u);
  }

  SUBCASE("CRLF line ending")
  {
    auto events_count = 0u;
    auto input = make_sse_data("data: test\r\n\r\n");

    parser.parse(
      std::span(input),
      [&](auto event) {
        CHECK(event.data == "test");
        events_count++;
      }
    );

    CHECK(events_count == 1u);
  }

  SUBCASE("Mixed line endings")
  {
    auto events_count = 0u;
    auto input = make_sse_data("data: test1\r\ndata: test2\n\r\ndata: test3\r\n\r\n");

    parser.parse(
      std::span(input),
      [&](auto event) {
        if ( events_count == 0 ) {
          CHECK(event.data == "test1\ntest2");
        }
        else if ( events_count == 1) {
          CHECK(event.data == "test3");
        }
        events_count++;
      }
    );

    CHECK(events_count == 2u);
  }
}

TEST_CASE("SSE Parser - Comments")
{
  http::sse_parser parser;

  SUBCASE("Comment line is ignored")
  {
    auto events_count = 0u;
    auto input = make_sse_data(": This is a comment\n\n");

    parser.parse(
      std::span(input),
      [&](auto event) {
        events_count++;
      }
    );

    CHECK(events_count == 0u);
  }

  SUBCASE("Comment with data after")
  {
    auto events_count = 0u;
    auto input = make_sse_data(": comment\ndata: test\n\n");

    parser.parse(
      std::span(input),
      [&](auto event) {
        CHECK(event.data == "test");
        events_count++;
      }
    );

    CHECK(events_count == 1u);
  }

  SUBCASE("Comment spanning multiple lines")
  {
    auto events_count = 0u;
    auto input = make_sse_data(": comment line 1\n: comment line 2\ndata: test\n\n");

    parser.parse(
      std::span(input),
      [&](auto event) {
        CHECK(event.data == "test");
        events_count++;
      }
    );

    CHECK(events_count == 1u);
  }

  SUBCASE("Comment at start of stream")
  {
    auto events_count = 0u;
    auto input = make_sse_data(": initial comment\ndata: test\n\n");

    parser.parse(
      std::span(input),
      [&](auto event) {
        CHECK(event.data == "test");
        events_count++;
      }
    );
    CHECK(events_count == 1u);
  }

  SUBCASE("Empty comment")
  {
    auto events_count = 0u;
    auto input = make_sse_data(":\ndata: test\n\n");

    parser.parse(
      std::span(input),
      [&](auto event) {
        CHECK(event.data == "test");
        events_count++;
      }
    );
    CHECK(events_count == 1u);
  }
}

TEST_CASE("SSE Parser - Event Dispatch")
{
  http::sse_parser parser;

  SUBCASE("Empty data event")
  {
    auto events_count = 0u;
    auto input = make_sse_data("data:\n\n");

    parser.parse(
      std::span(input),
      [&](auto event) {
        CHECK(event.data.empty());
        events_count++;
      }
    );

    CHECK(events_count == 1u);
  }

  SUBCASE("Event with only ID field")
  {
    auto events_count = 0u;
    auto input = make_sse_data("id: 123\n\n");

    parser.parse(
      std::span(input),
      [&](auto event) {
        CHECK(event.id == "123");
        CHECK(event.data.empty());
        events_count++;
      }
    );

    CHECK(events_count == 1u);
  }

  SUBCASE("Event with only type field")
  {
    auto events_count = 0u;
    auto input = make_sse_data("event: my-type\n\n");

    parser.parse(
      std::span(input),
      [&](auto event) {
        CHECK(event.type == "my-type");
        CHECK(event.data.empty());
        events_count++;
      }
    );

    CHECK(events_count == 1u);
  }

  SUBCASE("Multiple events in sequence")
  {
    auto events_count = 0u;
    auto input = make_sse_data("data: first\n\nid: 1\n\ndata: second\nid: 2\n\n");

    std::vector<http::sse_event> events;
    parser.parse(
      std::span(input),
      [&](auto event) {
        events.push_back(event);
        events_count++;
      }
    );

    CHECK(events_count == 3u);
    CHECK(events[0].id.empty());
    CHECK(events[0].data == "first");
    CHECK(events[1].id == "1");
    CHECK(events[1].data.empty());
    CHECK(events[2].id == "2");
    CHECK(events[2].data == "second");
  }

  SUBCASE("ID persists across events")
  {
    auto events_count = 0u;
    auto input = make_sse_data("id: persistent-id\ndata: first\n\nid: new-id\ndata: second\n\n");

    std::vector<http::sse_event> events;
    parser.parse(
      std::span(input),
      [&](auto event) {
        events.push_back(event);
        events_count++;
      }
    );

    CHECK(events_count == 2u);
    CHECK(events[0].id == "persistent-id");
    CHECK(events[0].data == "first");
    CHECK(events[1].id == "new-id");
    CHECK(events[1].data == "second");
  }

  SUBCASE("Type and data cleared after dispatch, ID persists")
  {
    auto events_count = 0u;
    auto input = make_sse_data("id: shared-id\nevent: first-type\ndata: first-data\n\nid: shared-id\nevent: second-type\ndata: second-data\n\n");

    std::vector<http::sse_event> events;
    parser.parse(
      std::span(input),
      [&](auto event) {
        events.push_back(event);
        events_count++;
      }
    );

    CHECK(events_count == 2u);
    CHECK(events[0].id == "shared-id");
    CHECK(events[0].type == "first-type");
    CHECK(events[0].data == "first-data");
    CHECK(events[1].id == "shared-id");  // ID persists
    CHECK(events[1].type == "second-type");
    CHECK(events[1].data == "second-data");
  }
}

TEST_CASE("SSE Parser - Edge Cases")
{
  http::sse_parser parser;

  SUBCASE("Incomplete event without trailing newline is discarded")
  {
    auto events_count = 0u;
    auto input = make_sse_data("data: incomplete");

    parser.parse(
      std::span(input),
      [&](auto event) {
        events_count++;
      }
    );

    CHECK(events_count == 0u);
  }

  SUBCASE("Empty stream")
  {
    auto events_count = 0u;
    auto input = make_sse_data("");

    parser.parse(
      std::span(input),
      [&](auto event) {
        events_count++;
      }
    );

    CHECK(events_count == 0u);
  }

  SUBCASE("Only newline")
  {
    auto events_count = 0u;
    auto input = make_sse_data("\n");

    parser.parse(
      std::span(input),
      [&](auto event) {
        events_count++;
      }
    );

    CHECK(events_count == 0u);
  }

  SUBCASE("Only double newline")
  {
    auto events_count = 0u;
    auto input = make_sse_data("\n\n");

    parser.parse(
      std::span(input),
      [&](auto event) {
        events_count++;
      }
    );

    CHECK(events_count == 0u);
  }

  SUBCASE("Field name with colon in value")
  {
    auto events_count = 0u;
    auto input = make_sse_data("data: value:with:colons\n\n");

    parser.parse(
      std::span(input),
      [&](auto event) {
        CHECK(event.data == "value:with:colons");
        events_count++;
      }
    );

    CHECK(events_count == 1u);
  }

  SUBCASE("Unknown field is ignored")
  {
    auto events_count = 0u;
    auto input = make_sse_data("unknown: value\ndata: test\n\n");

    parser.parse(
      std::span(input),
      [&](auto event) {
        CHECK(event.data == "test");
        events_count++;
      }
    );

    CHECK(events_count == 1u);
  }

  SUBCASE("Multiple consecutive empty lines")
  {
    auto events_count = 0u;
    auto input = make_sse_data("data: test\n\n\n\n");

    parser.parse(
      std::span(input),
      [&](auto event) {
        if (events_count == 0) {
          CHECK(event.data == "test");
        }
        events_count++;
      }
    );

    // First empty line dispatches, subsequent ones don't create events
    CHECK(events_count == 1u);
  }
}

TEST_CASE("SSE Parser - Chunked Parsing")
{
  http::sse_parser parser;

  SUBCASE("Parse event split across chunks")
  {
    auto events_count = 0u;
    auto chunk1 = make_sse_data("data: [DONE]");
    auto chunk2 = make_sse_data("\n\n");

    parser.parse(
      std::span(chunk1),
      [&](auto event) {
        events_count++;
      }
    );

    parser.parse(
      std::span(chunk2),
      [&](auto event) {
        CHECK(event.data == "[DONE]");
        events_count++;
      }
    );

    CHECK(events_count == 1u);
  }

  SUBCASE("Parse event with BOM in first chunk only")
  {
    auto events_count = 0u;
    auto chunk1 = make_bytes(0xEF, 0xBB, 0xBF, 'd', 'a', 't', 'a', ':', ' ', 'p', 'a', 'r', 't', '1', '\n');
    auto chunk2 = make_sse_data("data: part2\n\n");

    parser.parse(
      std::span(chunk1),
      [&](auto event) {
        events_count++;
      }
    );

    parser.parse(
      std::span(chunk2),
      [&](auto event) {
        CHECK(event.data == "part1\npart2");
        events_count++;
      }
    );

    CHECK(events_count == 1u);
  }

  SUBCASE("Reset parser state")
  {
    auto events_count = 0u;
    auto input = make_sse_data("data: test\n\n");

    parser.parse(
      std::span(input),
      [&](auto event) {
        events_count++;
      }
    );

    CHECK(events_count == 1u);

    parser.reset();
    events_count = 0u;

    parser.parse(
      std::span(input),
      [&](auto event) {
        CHECK(event.data == "test");
        events_count++;
      }
    );

    CHECK(events_count == 1u);
  }
}

