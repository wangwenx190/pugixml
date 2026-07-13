# pugixml

A fork of [pugixml](https://github.com/zeux/pugixml) with personal modifications.

Please be aware that this repository is for my own personal usages only and thus will never accept any kinds of feature requests and bug reports.

Build with a C++23 compiler and standard library.

## Changes compared to the original project

- Removed all unused files.
- Added streaming SAX parsing and serialization.
- Added indexed DOM views, typed SAX callbacks, and byte-span input.

## Indexed DOM lookup

Include `pugixml_dom.hpp` to use the header-only DOM view.

```cpp
#include "pugixml_dom.hpp"

pugi::xml_node_view root{doc.document_element()};
auto server = root["server"];
int port = server["network"]["port"].text().as_int(8080);

if (auto enabled = server[pugi::attribute_name{"enabled"}])
    enabled = true;
```

`view[name]` looks up the first child with that name. `view[pugi::attribute_name{name}]` looks up the first matching attribute. Both return empty handles on a miss. Use `node()` to retrieve the native handle for operations such as `append_child` and `append_attribute`.

The view holds one `xml_node` handle and follows the same lifetime and constness rules. Lookups use the existing DOM methods directly and allocate no memory. Returned `xml_text` and `xml_attribute` handles retain their usual mutation behavior, including allocation when the underlying tree needs storage. Names use `pugi::char_t`. Builds with `PUGIXML_HAS_STRING_VIEW` also support length-delimited names.

The view can also traverse all children or just children with a given name. Both forms return a lazy range of `xml_node_view` handles. Keep the document and any lookup name alive through iteration.

```cpp
for (auto server : root.children("server"))
{
    auto port = pugi::try_value<int>(server["network"]["port"].text());
    if (port)
        connect_to_port(*port);
}
```

`try_value<T>` also accepts `xml_attribute`. It returns `std::expected<T, pugi::xml_value_error>` with errors for missing values, invalid syntax, and out-of-range numbers. It accepts decimal integers, finite floating-point values, and the boolean spellings `true`, `false`, `1`, and `0`. Leading and trailing XML whitespace is accepted. Numeric parsing is locale-independent and consumes the full value. For numeric values longer than 128 characters in wide-character builds, conversion uses a temporary narrow string.

## Byte-span input

```cpp
const char xml[] = "<server/>";
auto bytes = std::as_bytes(std::span{xml}.first<sizeof(xml) - 1>());
pugi::xml_parse_result result = doc.load_buffer(bytes);
```

`xml_node::append_buffer` and `xml_sax_parse_buffer` also accept byte spans. For in-place loading, pass a writable byte span to `load_buffer_inplace` and keep its storage alive for the document's lifetime. In wide-character builds, native-width input at an unaligned address uses an aligned copy. Ownership transfer uses the existing `load_buffer_inplace_own` pointer interface with pugixml-allocated storage. The standard-library conveniences are available in builds with STL support.

## Streaming XML with SAX

`xml_sax_parse_file`, `xml_sax_parse_buffer`, and `xml_sax_parse` deliver events through an `xml_sax_handler`. The `xml_sax_parse` overloads accept input streams and custom byte sources derived from `xml_reader`. Each source is read sequentially.

```cpp
#include "pugixml.hpp"

struct element_counter : pugi::xml_sax_handler
{
    size_t count = 0;

    bool start_element(const pugi::char_t*, const pugi::xml_sax_attribute*, size_t) override
    {
        ++count;
        return true;
    }
};

element_counter handler;
pugi::xml_parse_result result = pugi::xml_sax_parse_file("large.xml", handler);
```

Callback strings and attribute arrays are valid for the duration of the callback. Copy values you need to retain. Empty elements produce both start and end events. PCDATA and CDATA can arrive in multiple calls. Returning `false` stops parsing with `status_sax_aborted`. A parse or I/O error can occur after events have already been delivered.

Include `pugixml_sax.hpp` to build a handler from typed lambdas.

```cpp
#include "pugixml_sax.hpp"

size_t elements = 0;
auto handler = pugi::make_sax_handler(
    [&](const pugi::sax::start_element&) { ++elements; });
pugi::xml_parse_result result = pugi::xml_sax_parse_file("large.xml", handler);
```

Register one or more callables for events in `pugi::sax`. Matching callables run in argument order. A `void` callback continues parsing and a `bool` callback controls whether parsing continues. The adapter stores callables by value and accepts `std::ref` for borrowed callables. Event strings and attribute spans borrow the parser's storage for the duration of each callback.

The parser uses the existing `parse_*` options and `xml_encoding` values. `parse_embed_pcdata` and `parse_merge_pcdata` apply only to DOM tree layout. `parse_trim_pcdata` streams the text and defers only whitespace whose position is still ambiguous. The parser keeps uniform deferred whitespace as a count and stores mixed whitespace in compact pages.

Use these SAX options when an individual attribute or comment can be large.

- `parse_sax_stream_attributes` calls `start_element` with an empty attribute array, then calls `start_attribute`, zero or more `attribute_value` callbacks, and `end_attribute` for each attribute. `end_start_element` confirms that the entire start tag parsed successfully. Attribute values use the selected normalization and escape options.
- `parse_comments | parse_sax_stream_comments` sends comments to `comment_chunk(value, last)`, with `last` set on the final chunk of each comment.

The regular callbacks can still deliver a whole start tag's attributes or a complete comment. They need contiguous storage for the data they deliver. Streamed parsing also retains open element names and any individual data that its callback requires as a single string, such as an attribute name or a reported DOCTYPE value. Very long unresolved numeric references and mixed deferred whitespace can also use additional memory.

`xml_sax_writer` sends raw XML to an existing `xml_writer` sink such as `xml_writer_file` or `xml_writer_stream`. Keep the sink alive until the SAX writer has finished.

```cpp
bool write_items(pugi::xml_writer& sink)
{
    pugi::xml_sax_writer writer(sink);
    const pugi::xml_sax_attribute attributes[] = {
        {PUGIXML_TEXT("id"), PUGIXML_TEXT("42")}
    };
    return writer.start_element(PUGIXML_TEXT("items")) &&
           writer.start_element(PUGIXML_TEXT("item"), attributes, 1) &&
           writer.text(pugi::node_pcdata, PUGIXML_TEXT("A & B")) &&
           writer.end_element() &&
           writer.end_element() &&
           writer.finish();
}
```

Call `declaration()` to emit an XML declaration. The writer accepts the encoding and the BOM, escaping, control-character, attribute-quote, and empty-tag formatting flags. `finish()` flushes the writer's buffer after all open elements close. Check the sink for I/O errors.

## Length-aware SAX

`xml_sax_view_handler` receives `pugi::string_view_t` for names and text, and a span of `xml_sax_attribute_view` for attributes. Use the same parse functions and options as for `xml_sax_handler`. Views are valid during the callback. Copy the parts you need to retain. Text and CDATA can arrive in multiple calls, with chunk boundaries that depend on the source and the parser path.

```cpp
struct text_counter : pugi::xml_sax_view_handler
{
    size_t units = 0;

    bool text(pugi::xml_node_type, pugi::string_view_t value) override
    {
        units += value.size();
        return true;
    }
};
```

The view-based buffer parser can borrow complete tokens from a read-only UTF-8 input in a narrow-character build with `parse_default`. Tokens that require conversion or the full grammar use the streaming window. File and reader inputs continue to use a sliding window. Use the streaming attribute and comment options for large individual values.

The SAX writer also accepts views for element names, attributes, text, declarations, and processing instructions. Pass the known length when the source already has one. Views need no terminating zero. The writer copies open element names so they survive until `end_element()`.

## Reusing storage across documents

Use `xml_sax_parser` for sequential SAX parses with a view handler. The constructor's byte limit caps dynamically allocated storage retained between parses. The parser retains its inline workspace and can be used again after an abort or a handler exception.

Call `xml_sax_writer::reset()` with a sink after a successful `finish()` to write the next document using the same output buffer. Its retention limit applies to extra storage for open element names and nesting. A sink exception still invalidates the writer.

Use `xml_document_parser::load_buffer()` for repeated DOM loads. `document()` returns the parsed document. The parser can retain the previous native input buffer and one standard allocation page up to its byte limit. Each load invalidates handles from the previous document. Call `reset()` to release retained storage.

## Tests and benchmarks

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPUGIXML_BUILD_TESTS=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure

build/pugixml_dom_view_benchmark
build/pugixml_sax_benchmark generate sample.xml 500000
build/pugixml_sax_benchmark dom-read sample.xml 500000
build/pugixml_sax_benchmark dom-visit sample.xml 500000
build/pugixml_sax_benchmark sax-read sample.xml 500000
build/pugixml_sax_benchmark dom-write sample.xml 500000
build/pugixml_sax_benchmark sax-write sample.xml 500000
```

The DOM view benchmark compares named and indexed lookups on the same tree and checks that the results match.

The SAX benchmark read cases use the same file. `dom-visit` and `sax-read` count elements, attributes, and text. The write cases serialize the same XML to a byte-counting sink. DOM setup happens before the `dom-write` timer starts. On macOS and Linux the benchmark also reports whole-process peak RSS. Use `generate-text` for a file with one large text node.

## License

```text
MIT License

Copyright (C) 2026 wangwenx190 (Yuhang ZHAO)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

The MIT license above covers this fork's changes. The original pugixml code remains under its [MIT license](LICENSE).

Please credit pugixml in user-facing product documentation, credits, or both. For example:

> Uses pugixml. Copyright (c) 2006-2026, Arseny Kapoulkine

<details>
<summary>This work is based on the pugxml parser, which carried the following Public Domain dedication</summary>

    // Pug XML Parser - Version 1.0002
    // --------------------------------------------------------
    // Copyright (C) 2003, by Kristen Wegner (kristen@tima.net)
    // Released into the Public Domain. Use at your own risk.
    // See pugxml.xml for further information, history, etc.
    // Contributions by Neville Franks (readonly@getsoft.com).
</details>

For contributions to the original project, see the [upstream AI policy](https://github.com/zeux/pugixml/blob/master/README.md#ai-policy).
