#include "pugixml_sax.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#define CHECK(expression) do { if (!(expression)) { std::cerr << __LINE__ << ": " << #expression << '\n'; std::abort(); } } while (false)
#define T(value) PUGIXML_TEXT(value)

namespace
{
	using namespace pugi;
	using text_string = std::basic_string<char_t>;

	struct event
	{
		xml_node_type type;
		text_string name, value;
		std::vector<text_string> attributes;

		bool operator==(const event& rhs) const
		{
			return type == rhs.type && name == rhs.name && value == rhs.value && attributes == rhs.attributes;
		}
	};

	struct events: xml_sax_handler
	{
		std::vector<event> items;

		bool tag(xml_node_type type, const char_t* name, const xml_sax_attribute* attributes, size_t count)
		{
			items.push_back({type, name, {}, {}});
			for (size_t i = 0; i < count; ++i)
			{
				items.back().attributes.emplace_back(attributes[i].name);
				items.back().attributes.emplace_back(attributes[i].value);
			}
			return true;
		}

		bool start_element(const char_t* name, const xml_sax_attribute* attributes, size_t count) override
		{
			return tag(node_element, name, attributes, count);
		}

		bool end_element(const char_t* name) override
		{
			return tag(node_null, name, nullptr, 0);
		}

		bool declaration(const char_t* name, const xml_sax_attribute* attributes, size_t count) override
		{
			return tag(node_declaration, name, attributes, count);
		}

		bool processing_instruction(const char_t* name, const char_t* value) override
		{
			items.push_back({node_pi, name, value, {}});
			return true;
		}

		bool text(xml_node_type type, const char_t* value) override
		{
			if ((type == node_cdata || type == node_pcdata) && !items.empty() && items.back().type == type)
				items.back().value += value;
			else
				items.push_back({type, {}, value, {}});
			return true;
		}
	};

	struct streaming_events: events
	{
		text_string comment;
		bool in_start_tag = false;
		bool in_attribute = false;
		const bool stream_attributes;

		explicit streaming_events(unsigned int flags): stream_attributes((flags & parse_sax_stream_attributes) != 0) {}

		bool start_element(const char_t* name, const xml_sax_attribute* attributes, size_t count) override
		{
			CHECK(!in_start_tag && !in_attribute);
			in_start_tag = stream_attributes;
			if (in_start_tag) CHECK(count == 0);
			return events::start_element(name, attributes, count);
		}

		bool start_attribute(const char_t* name) override
		{
			CHECK(in_start_tag && !in_attribute);
			in_attribute = true;
			items.back().attributes.emplace_back(name);
			items.back().attributes.emplace_back();
			return true;
		}

		bool attribute_value(const char_t* value) override
		{
			CHECK(in_attribute);
			items.back().attributes.back() += value;
			return true;
		}

		bool end_attribute() override
		{
			CHECK(in_attribute);
			in_attribute = false;
			return true;
		}

		bool end_start_element() override
		{
			CHECK(in_start_tag && !in_attribute);
			in_start_tag = false;
			return true;
		}

		bool comment_chunk(const char_t* value, bool last) override
		{
			comment += value;
			if (last)
			{
				items.push_back({node_comment, {}, comment, {}});
				comment.clear();
			}
			return true;
		}
	};

	struct view_events: xml_sax_view_handler
	{
		std::vector<event> items;
		text_string comment;
		bool streaming_attributes = false;

		bool tag(xml_node_type type, string_view_t name, std::span<const xml_sax_attribute_view> attributes)
		{
			items.push_back({type, text_string(name), {}, {}});
			for (const auto& attribute: attributes)
			{
				items.back().attributes.emplace_back(attribute.name);
				items.back().attributes.emplace_back(attribute.value);
			}
			return true;
		}

		bool start_element(string_view_t name, std::span<const xml_sax_attribute_view> attributes) override { return tag(node_element, name, attributes); }
		bool end_element(string_view_t name) override { return tag(node_null, name, {}); }
		bool declaration(string_view_t name, std::span<const xml_sax_attribute_view> attributes) override { return tag(node_declaration, name, attributes); }

		bool processing_instruction(string_view_t name, string_view_t value) override
		{
			items.push_back({node_pi, text_string(name), text_string(value), {}});
			return true;
		}

		bool text(xml_node_type type, string_view_t value) override
		{
			if ((type == node_cdata || type == node_pcdata) && !items.empty() && items.back().type == type)
				items.back().value += value;
			else
				items.push_back({type, {}, text_string(value), {}});
			return true;
		}

		bool start_attribute(string_view_t name) override
		{
			CHECK(streaming_attributes);
			items.back().attributes.emplace_back(name);
			items.back().attributes.emplace_back();
			return true;
		}

		bool attribute_value(string_view_t value) override
		{
			items.back().attributes.back() += value;
			return true;
		}

		bool comment_chunk(string_view_t value, bool last) override
		{
			comment += value;
			if (last)
			{
				items.push_back({node_comment, {}, comment, {}});
				comment.clear();
			}
			return true;
		}
	};

	void collect(events& output, xml_node parent)
	{
		for (xml_node node: parent.children())
		{
			switch (node.type())
			{
			case node_element:
			case node_declaration:
			{
				std::vector<xml_sax_attribute> attributes;
				for (xml_attribute attr: node.attributes())
					attributes.push_back({attr.name(), attr.value()});

				output.tag(node.type(), node.name(), attributes.data(), attributes.size());
				if (node.type() == node_element)
				{
					collect(output, node);
					output.end_element(node.name());
				}
				break;
			}
			case node_pi:
				output.processing_instruction(node.name(), node.value());
				break;
			default:
				output.text(node.type(), node.value());
				break;
			}
		}
	}

	struct short_reader: xml_reader
	{
		const std::string& bytes;
		size_t offset = 0;
		size_t limit;
		bool fail_at_end;

		short_reader(const std::string& bytes_, size_t limit_, bool fail_ = false):
			bytes(bytes_), limit(limit_), fail_at_end(fail_) {}

		size_t read(void* data, size_t size) override
		{
			size = std::min({size, limit, bytes.size() - offset});
			if (!size) return fail_at_end ? read_error : 0;
			std::memcpy(data, bytes.data() + offset, size);
			offset += size;
			return size;
		}
	};

	struct string_writer: xml_writer
	{
		std::string bytes;
		void write(const void* data, size_t size) override { bytes.append(static_cast<const char*>(data), size); }
	};

	void compare(const std::string& bytes, xml_encoding encoding, size_t step, unsigned int flags)
	{
		xml_document doc;
		xml_parse_result dom = doc.load_buffer(bytes.data(), bytes.size(), flags, encoding);
		CHECK(dom);
		events expected;
		collect(expected, doc);

		streaming_events actual(flags);
		short_reader reader(bytes, step);
		xml_parse_result result = xml_sax_parse(reader, actual, flags, encoding);
		if (!result) std::cerr << result.description() << " at " << result.offset << '\n';
		CHECK(result);
		CHECK(result.encoding == dom.encoding);
		CHECK(actual.items == expected.items);
		CHECK(!actual.in_start_tag && !actual.in_attribute && actual.comment.empty());
	}

	void compare_views(const std::string& bytes, unsigned int flags = parse_default, xml_encoding encoding = encoding_auto)
	{
		streaming_events legacy(flags);
		xml_parse_result expected = xml_sax_parse_buffer(bytes.data(), bytes.size(), legacy, flags, encoding);

		std::vector<char> bounded(bytes.begin(), bytes.end());
		view_events direct;
		direct.streaming_attributes = (flags & parse_sax_stream_attributes) != 0;
		xml_parse_result result = xml_sax_parse_buffer(std::as_bytes(std::span{bounded}), direct, flags, encoding);
		CHECK(result.status == expected.status && result.offset == expected.offset && result.encoding == expected.encoding);
		if (expected) CHECK(direct.items == legacy.items);

		for (size_t step: {1, 7, 65536})
		{
			short_reader old_reader(bytes, step);
			streaming_events old(flags);
			expected = xml_sax_parse(old_reader, old, flags, encoding);
			view_events view;
			view.streaming_attributes = direct.streaming_attributes;
			short_reader reader(bytes, step);
			result = xml_sax_parse(reader, view, flags, encoding);
			CHECK(result.status == expected.status && result.offset == expected.offset && result.encoding == expected.encoding);
			CHECK(view.items == old.items);
		}
	}

	void length_aware_reading()
	{
		const std::string mixed = "<?xml version='1.0'?><!DOCTYPE root><root user='alice' other='1\t2'>"
			"  <node><one a = ' &amp; ' b = \"x'y\"/> ordinary <![CDATA[one\r\ntwo]]>"
			"<!--hello\r\nworld--><?pi value?> &#0;x</node><last/></root><second/>";
		for (unsigned int flags: {parse_default, parse_full, parse_minimal,
			parse_full | parse_sax_stream_attributes | parse_sax_stream_comments,
			parse_full | parse_trim_pcdata | parse_wnorm_attribute})
			compare_views(mixed, flags);

		for (const char* bytes: {"", "<", "<a", "<a/>", "<a>text<", "<a>x</b>", "<r b='v'c='w'/>", "<a><b/></a >",
			"<a><b a=\"x\" c='y'/><c>&amp;&#65;</c></a>", "\xef\xbb\xbf<ok> plain </ok>",
			"<r> \r\ntext</r>", "<r>\t\rtext</r>", "<r>\r\n <e/></r>"})
			compare_views(bytes);

		compare_views(std::string("<a>text\0discarded", 17));
		compare_views("<?xml version='1.0' encoding='ISO-8859-1'?><r>caf\xe9</r>");

		// A later converted token must still close ancestors borrowed from the original input.
		const std::string large(70000, 'x');
		compare_views("<root a='x'><r>" + large + "</r><p>hi &amp; " + large + "</p></root>");

		xml_document source;
		CHECK(source.load_buffer(mixed.data(), mixed.size(), parse_full));
		for (xml_encoding encoding: {encoding_utf16_be, encoding_utf32_le})
		{
			string_writer sink;
			source.save(sink, T(""), format_raw | format_write_bom, encoding);
			compare_views(sink.bytes, parse_full, encoding_auto);
		}

		struct borrow_probe: xml_sax_view_handler
		{
			const char* input;
			size_t texts = 0;
			explicit borrow_probe(const char* input_): input(input_) {}
			bool start_element(string_view_t name, std::span<const xml_sax_attribute_view> attributes) override
			{
				CHECK(name == T("root") && attributes.size() == 1);
				CHECK(attributes[0].name == T("a") && attributes[0].value == T("xyz"));
				if constexpr (sizeof(char_t) == 1)
				{
					CHECK(static_cast<const void*>(name.data()) == input + 1);
					CHECK(static_cast<const void*>(attributes[0].value.data()) == input + 9);
				}
				return true;
			}
			bool text(xml_node_type type, string_view_t value) override
			{
				CHECK(type == node_pcdata && value == T("payload"));
				if constexpr (sizeof(char_t) == 1)
					CHECK(static_cast<const void*>(value.data()) == input + 14);
				++texts;
				return true;
			}
		};
		const std::string plain = "<root a='xyz'>payload</root>";
		std::vector<char> bytes(plain.begin(), plain.end());
		borrow_probe view(bytes.data());
		CHECK(xml_sax_parse_buffer(std::as_bytes(std::span{bytes}), view));
		CHECK(view.texts == 1);

		xml_sax_parser parser;
		struct recursive_handler: xml_sax_view_handler
		{
			xml_sax_parser& parser;
			explicit recursive_handler(xml_sax_parser& parser_): parser(parser_) {}
			bool start_element(string_view_t, std::span<const xml_sax_attribute_view>) override
			{
				CHECK(parser.parse_buffer("<x/>", 4, *this).status == status_sax_aborted);
				return false;
			}
		} stop(parser);
		CHECK(parser.parse_buffer(bytes.data(), bytes.size(), stop).status == status_sax_aborted);
		CHECK(parser.parse_buffer(bytes.data(), bytes.size(), view));
		CHECK(view.texts == 2);
	}

	void options_and_encoding()
	{
		const std::string mixed = "<?xml version='1.0'?><!DOCTYPE root [<!ELEMENT root ANY>"
			"<!--x--><?inside v?><!ENTITY y '>'><![IGNORE[<![x]]>]]>]>"
			"<?pi a?>\r\n<root a='  A\r\n\tB &amp; &#x1f600;  ' a=\"duplicate\" xmlns:p='urn:p'>"
			"<p:child />\t<ws> \r\n </ws><before><!--comment--> </before><!--a\r\nb-->"
			"<![CDATA[one\r\ntwo]]> text &lt; &unknown; &amp;  </root><second/>";

		for (unsigned int flags: {parse_default, parse_full, parse_minimal, parse_full | parse_ws_pcdata,
			parse_full | parse_ws_pcdata_single, parse_full | parse_trim_pcdata | parse_wnorm_attribute,
			parse_full | parse_fragment | parse_ws_pcdata})
			for (size_t step: {1, 3, 7, 65536})
				compare(mixed, encoding_auto, step, flags);

		compare("<?xml version='1.0' encoding='ISO-8859-1'?><r>caf\xe9</r>", encoding_auto, 1, parse_full);
		compare("<?xml " + std::string(70000, ' ') + "encoding='latin1'?><r>caf\xe9</r>", encoding_auto, 23, parse_full);
		compare("<r>caf\xe9</r>", encoding_latin1, 2, parse_default);
		compare("\xef\xbb\xbf<r>\xf0\x9f\x98\x80</r>", encoding_auto, 1, parse_default);
		compare("begin <r/> end ", encoding_auto, 1, parse_full | parse_fragment);
		compare("text<", encoding_auto, 1, parse_fragment);
		compare("", encoding_auto, 1, parse_fragment);

		xml_document source;
		CHECK(source.load_buffer(mixed.data(), mixed.size(), parse_full));
		for (xml_encoding encoding: {encoding_utf8, encoding_utf16_le, encoding_utf16_be, encoding_utf32_le, encoding_utf32_be})
		{
			string_writer writer;
			source.save(writer, T(""), format_raw | format_write_bom, encoding);
			for (size_t step: {1, 2, 3, 7, 65536})
				compare(writer.bytes, encoding_auto, step, parse_full);
		}
	}

	void window_boundaries()
	{
		for (size_t length = 0; length < 48; ++length)
		{
			const text_string prefix(length, T('x'));
			text_string xml = T("<r>") + prefix + T("&amp;\r\n</r>");
			xml_document doc;
			CHECK(doc.load_buffer_inplace(xml.data(), xml.size() * sizeof(char_t), parse_default,
				sizeof(char_t) == 1 ? encoding_utf8 : encoding_wchar));
			CHECK(doc.document_element().child_value() == prefix + T("&\n"));
		}

		const std::string large(180000, 'x');
		compare("<r " + large + "='" + large + "&amp;'/>", encoding_auto, 3, parse_default);
		compare("<r>" + large + "&#x1f600;\r\n" + large + "</r>", encoding_auto, 17, parse_default);
		compare("<r>" + large + "&#0;" + large + "</r>", encoding_auto, 7, parse_default);
		compare("<r>&#x" + std::string(180000, '0') + "41;</r>", encoding_auto, 7, parse_default);
		std::string references = "<r>";
		for (size_t i = 0; i < 5000; ++i)
			references += "&amp;&#65; &#x20;\r\n&lt;";
		references += "</r>";
		for (size_t step: {1, 7, 511, 65536})
			compare(references, encoding_auto, step, parse_default);
		compare("<r>   " + large + " \r\n " + large + "   </r>", encoding_auto, 23, parse_default | parse_trim_pcdata);
		compare("<r><![CDATA[" + large + "\r\n]]]x" + large + "]]><![CDATA[]]></r>", encoding_auto, 7, parse_default);
		compare("<r><!--" + large + "--><a/></r>", encoding_auto, 3, parse_default);
		compare("<!DOCTYPE r [<!ENTITY x '" + large + "'>]><r/>", encoding_auto, 3, parse_default);
		// Cover the initial inline input and the first full refill.
		for (size_t boundary: {512, 512 + 65536})
		{
			for (size_t extra = 0; extra < 10; ++extra)
			{
				std::string prefix(boundary - 8 + extra, 'x');
				compare("<r>" + prefix + "&#x1f600;\r\n&amp;\xf0\x9f\x98\x80</r>", encoding_auto, 65536, parse_default);
				compare("<r><![CDATA[" + prefix + "]]></r>", encoding_auto, 65536, parse_default);
				compare("<r a='" + prefix + "' b='y'><a /></r>", encoding_auto, 65536, parse_default);
				compare("<r a='" + prefix + "\r\n\t&amp;'/>", encoding_auto, 65536, parse_default);
			}
		}

	}

	void streaming_tokens()
	{
		const unsigned int stream = parse_sax_stream_attributes | parse_sax_stream_comments;
		const unsigned int variants[] = {
			parse_minimal, parse_eol, parse_escapes, parse_default,
			parse_wnorm_attribute, parse_wnorm_attribute | parse_escapes | parse_eol | parse_wconv_attribute
		};
		const std::string attrs = "<r empty='' a=' \n\tA\r\r\n\t  &lt;&amp;&#32;&#13; B&#9;  ' "
			"b='  &#32;&#9;  ' b=' &#32;x&#0;tail' c='a &bad; &a &amp' d=\"x'&quot;y\">"
			"<!--A\r\nB--><!-- --><!---dash--><empty/></r>";
		for (unsigned int flags: variants)
			for (size_t step: {1, 2, 7, 65536})
				compare(attrs, encoding_auto, step, flags | stream | parse_comments);

		const std::string value(180000, 'x');
		const std::string spaces(180000, ' ');
		std::string mixed;
		for (size_t i = 0; i < 50000; ++i) mixed += " \t\r\n";
		compare("<r" + spaces + "a" + spaces + "=" + spaces + "'" + value + "\r\n&amp;" + value + "'/>",
			encoding_auto, 65536, parse_default | stream);
		compare("<r a=' " + value + spaces + " \r\nz  &#32;" + spaces + "'><!--" + value + "\r\n-" + value + "--><!----></r>",
			encoding_auto, 65536, parse_full | stream | parse_wnorm_attribute);
		compare("<r>" + mixed + value + mixed + "&#32;y" + mixed + "</r>", encoding_auto, 65536, parse_default);
		compare("<r>" + mixed + value + mixed + "&#32;y" + mixed + "</r>", encoding_auto, 65536, parse_default | parse_trim_pcdata);
		compare("<r>" + spaces + "a" + spaces + "&#0;x</r>", encoding_auto, 65536, parse_default | parse_trim_pcdata);
		compare("<r>" + spaces + "</r>", encoding_auto, 65536, parse_default | parse_ws_pcdata_single);
		compare("<r> &#32; &#9; </r>", encoding_auto, 1, parse_default | parse_trim_pcdata);
	}

	void errors()
	{
		const char* invalid[] = {"", "whitespace \t", "<", "<r>", "<r/> <", "<r><", "<r>a<", "<r/ >", "<r><s></r>",
			"<r></rr>", "<rr></r>", "<r></r x>",
			"<r x=y/>", "<r x='y'z='w'/>", "<r x='y", "<r><!-- a --</r>", "<r><![CDATA[x]]</r>",
			"<?bad#?><r/>", "<r><?xml version='1.0'?></r>", "<r><!DOCTYPE r></r>", "<!DOCTYPE r [<!BAD"};
		for (const char* bytes: invalid)
		{
			for (size_t step: {1, 16})
			{
				xml_document doc;
				events handler;
				std::string input(bytes);
				short_reader reader(input, step);
				xml_parse_result expected = doc.load_buffer(input.data(), input.size(), parse_full);
				xml_parse_result actual = xml_sax_parse(reader, handler, parse_full);
				CHECK(!actual);
				if (actual.status != expected.status)
					std::cerr << bytes << " on " << step << "-byte reads: " << actual.description() << ", expected " << expected.description() << '\n';
				CHECK(actual.status == expected.status);
			}
		}

		struct stop: xml_sax_handler
		{
			bool start_element(const char_t*, const xml_sax_attribute*, size_t) override { return false; }
		} handler;

		std::string input = "<r>" + std::string(100000, 'a') + "</r>";
		short_reader reader(input, 1);
		CHECK(xml_sax_parse(reader, handler).status == status_sax_aborted);
		CHECK(reader.offset < input.size());

		short_reader eager_reader(input, 65536);
		CHECK(xml_sax_parse(eager_reader, handler).status == status_sax_aborted);
		CHECK(eager_reader.offset < eager_reader.limit);

		xml_sax_handler skip;
		short_reader error_reader(input, 4, true);
		CHECK(xml_sax_parse(error_reader, skip).status == status_io_error);
	}

	void lambda_callbacks()
	{
		const char xml[] = "<?xml version='1.0'?><?probe ok?><r a='&amp;'><!--hi-->text</r>";
		const auto bytes = std::as_bytes(std::span{xml}.first<sizeof(xml) - 1>());
		int starts = 0;
		int texts = 0;
		int ends = 0;
		auto start = [&](const sax::start_element& event) {
			CHECK(event.name == string_view_t(T("r")));
			CHECK(event.attributes.size() == 1);
			CHECK(event.attributes.front().value == string_view_t(T("&")));
			++starts;
		};
		auto handler = make_sax_handler(
			std::ref(start),
			[owned = std::make_unique<int>(0)](const sax::start_element&) { return ++*owned == 1; },
			[&](const sax::text& event) { if (event.type == node_pcdata) ++texts; },
			[&](const sax::end_element&) { ++ends; });
		CHECK(xml_sax_parse_buffer(bytes, handler, parse_full));
		CHECK(starts == 1 && texts == 1 && ends == 1);

		int seen = 0;
		auto streamed = make_sax_handler(
			[&](const sax::declaration& event) { CHECK(event.attributes.size() == 1); ++seen; },
			[&](const sax::processing_instruction& event) { CHECK(event.value == string_view_t(T("ok"))); ++seen; },
			[&](const sax::start_element& event) { CHECK(event.attributes.empty()); ++seen; },
			[&](const sax::start_attribute& event) { CHECK(event.name == string_view_t(T("a"))); ++seen; },
			[&](const sax::attribute_value& event) { CHECK(event.value == string_view_t(T("&"))); ++seen; },
			[&](const sax::end_attribute&) { ++seen; },
			[&](const sax::end_start_element&) { ++seen; },
			[&](const sax::comment_chunk& event) { CHECK(event.last); CHECK(event.value == string_view_t(T("hi"))); ++seen; });
		CHECK(xml_sax_parse_buffer(bytes, streamed, parse_full | parse_sax_stream_attributes | parse_sax_stream_comments));
		CHECK(seen == 8);

		int aborted = 0;
		auto stop = make_sax_handler(
			[&](const sax::start_element&) { CHECK(aborted == 0); ++aborted; },
			[&](const sax::start_element&) { CHECK(aborted == 1); ++aborted; return false; },
			[&](const sax::start_element&) { CHECK(false); });
		CHECK(xml_sax_parse_buffer(bytes, stop).status == status_sax_aborted);
		CHECK(aborted == 2);
	}

	struct alignas(std::max_align_t) allocation
	{
		size_t size;
	};

	size_t live_bytes, peak_bytes, remaining_allocations;

	void* track_allocate(size_t size)
	{
		if (!remaining_allocations) return nullptr;
		--remaining_allocations;

		auto* memory = static_cast<allocation*>(std::malloc(sizeof(allocation) + size));
		if (!memory) return nullptr;
		memory->size = size;
		live_bytes += size;
		peak_bytes = std::max(peak_bytes, live_bytes);
		return memory + 1;
	}

	void track_deallocate(void* ptr)
	{
		if (!ptr) return;
		auto* memory = static_cast<allocation*>(ptr) - 1;
		live_bytes -= memory->size;
		std::free(memory);
	}

	void memory_usage()
	{
		const size_t mib = 1024 * 1024;
		const std::string large(4 * mib, 'x');
		const std::string spaces(large.size(), ' ');
		std::string mixed = spaces;
		for (size_t i = 1; i < mixed.size(); i += 2) mixed[i] = '\t';
		struct testcase
		{
			std::string bytes;
			unsigned int flags;
			size_t limit;
		};
		const testcase cases[] = {
			{"<r>" + large + "</r>", parse_default, mib},
			{"<r><![CDATA[" + large + "]]></r>", parse_default, mib},
			{"<!DOCTYPE r [<!ENTITY x '" + large + "'>]><r/>", parse_default, mib},
			{"<r>" + spaces + large + "</r>", parse_default, mib},
			{"<r> " + large + spaces + "</r>", parse_default | parse_trim_pcdata, mib},
			{"<r><!--" + large + "--></r>", parse_full | parse_sax_stream_comments, mib},
			{"<r a='" + large + "'/>", parse_default | parse_sax_stream_attributes, mib},
			{"<r" + spaces + "a='1'/>", parse_default | parse_sax_stream_attributes, mib},
			{"<r>" + mixed + "x</r>", parse_default, 2 * mib}
		};
		auto allocate = get_memory_allocation_function();
		auto deallocate = get_memory_deallocation_function();
		set_memory_management_functions(track_allocate, track_deallocate);
		xml_sax_handler handler;
		for (const testcase& test: cases)
		{
			peak_bytes = 0;
			remaining_allocations = static_cast<size_t>(-1);
			CHECK(xml_sax_parse_buffer(test.bytes.data(), test.bytes.size(), handler, test.flags));
			CHECK(live_bytes == 0 && peak_bytes < test.limit);
		}

		const char_t small[] = T("<r><a x='y'/></r>");
		remaining_allocations = 0;
		CHECK(xml_sax_parse_buffer(small, sizeof(small) - sizeof(char_t), handler, parse_default,
			sizeof(char_t) == 1 ? encoding_utf8 : encoding_wchar));
		int elements = 0;
		auto callbacks = make_sax_handler([&](const sax::start_element&) { ++elements; });
		CHECK(xml_sax_parse_buffer(std::as_bytes(std::span{small}.first<std::size(small) - 1>()), callbacks,
			parse_default, sizeof(char_t) == 1 ? encoding_utf8 : encoding_wchar));
		CHECK(elements == 2);
		CHECK(live_bytes == 0);

		const char input[] = "<r><a x='y'/></r>";
		for (size_t budget = 0; budget < 8; ++budget)
		{
			remaining_allocations = budget;
			xml_parse_result result = xml_sax_parse_buffer(input, sizeof(input) - 1, handler);
			CHECK(result || result.status == status_out_of_memory);
			CHECK(live_bytes == 0);
		}
		set_memory_management_functions(allocate, deallocate);
	}

	void write_and_read()
	{
		const xml_sax_attribute decl[] = {{T("version"), T("1.0")}};
		const xml_sax_attribute attrs[] = {{T("a"), T("x<&'\"")}};
		string_writer sink;
		xml_sax_writer writer(sink, format_write_bom | format_attribute_single_quote | format_no_empty_element_tags, encoding_utf16_be);
		CHECK(!writer.end_element());
		CHECK(writer.declaration(T("xml"), decl, 1));
		CHECK(writer.text(node_doctype, T("r")));
		CHECK(writer.start_element(T("r"), attrs, 1));
		CHECK(!writer.finish());
		CHECK(!writer.declaration(T("xml")));
		CHECK(writer.text(node_pcdata, T("<&>")));
		CHECK(writer.text(node_cdata, T("x]]>y")));
		CHECK(writer.text(node_comment, T("a--b-")));
		CHECK(writer.processing_instruction(T("target"), T("a?>b")));
		CHECK(writer.start_element(T("empty")));
		CHECK(writer.end_element());
		CHECK(writer.end_element());
		CHECK(writer.finish());
		CHECK(writer.finish());
		CHECK(!writer.start_element(T("x")));

		std::istringstream stream(sink.bytes);
		events handler;
		xml_parse_result result = xml_sax_parse(stream, handler, parse_full);
		CHECK(result && result.encoding == encoding_utf16_be);
		CHECK(handler.items[2].attributes[1] == T("x<&'\""));
		CHECK(handler.items[3].value == T("<&>"));
		CHECK(handler.items[4].value == T("x]]>y"));
		CHECK(handler.items[5].value == T("a- -b- "));
		CHECK(handler.items[6].value == T("a? >b"));

		string_writer raw;
		xml_sax_writer empty(raw);
		CHECK(empty.start_element(T("root")));
		CHECK(empty.end_element());
		CHECK(empty.finish());
		CHECK(raw.bytes == "<root/>");

		for (size_t length = 0; length < 64; ++length)
		{
			const text_string value = text_string(length, T('x')) + T("\t\n\r&<>{{") + T('\x01');
			const std::string expected = "<r>" + std::string(length, 'x') + "\t\n\r&amp;&lt;&gt;{{&#01;</r>";

			string_writer dom_output;
			xml_document doc;
			CHECK(doc.append_child(T("r")).text().set(value.c_str()));
			doc.save(dom_output, T(""), format_raw | format_no_declaration);
			CHECK(dom_output.bytes == expected);

			string_writer sax_output;
			xml_sax_writer sax(sax_output);
			CHECK(sax.start_element(T("r")) && sax.text(node_pcdata, value.c_str()) && sax.end_element() && sax.finish());
			CHECK(sax_output.bytes == expected);
		}
	}

	void writer_tag_lengths()
	{
		xml_document doc;
		xml_node root = doc.append_child(T("root"));
		CHECK(root);
		string_writer sink;
		xml_sax_writer writer(sink, format_no_empty_element_tags, encoding_utf8);
		CHECK(writer.start_element(T("root")));
		std::string expected = "<root>";
		for (size_t length = 1; length <= 129; ++length)
		{
			std::string name;
			for (size_t i = 0; i < length; ++i)
				name += "abcdefghijklmnopqrstuvwxyz"[i % 26];
			const text_string native_name(name.begin(), name.end());
			CHECK(root.append_child(native_name.c_str()).text().set(T("v")));
			CHECK(writer.start_element(native_name.c_str()));
			CHECK(writer.text(node_pcdata, T("v")));
			CHECK(writer.end_element());
			expected += "<" + name + ">v</" + name + ">";
		}
		CHECK(writer.end_element());
		CHECK(writer.finish());
		expected += "</root>";
		CHECK(sink.bytes == expected);

		string_writer dom_sink;
		doc.save(dom_sink, T(""), format_raw | format_no_declaration, encoding_utf8);
		CHECK(dom_sink.bytes == expected);
	}

	void length_aware_writing()
	{
		for (unsigned int flags: {format_raw, format_raw | format_write_bom | format_attribute_single_quote | format_no_empty_element_tags})
			for (xml_encoding encoding: {encoding_utf8, encoding_utf16_be, encoding_utf32_le})
			{
				string_writer old_sink;
				xml_sax_writer old(old_sink, flags, encoding);
				const xml_sax_attribute version[] = {{T("version"), T("1.0")}};
				const xml_sax_attribute attrs[] = {{T("name"), T("a&b'\"")}};
				CHECK(old.declaration(T("xml"), version, 1));
				CHECK(old.text(node_doctype, T("root")));
				CHECK(old.start_element(T("root"), attrs, 1));
				CHECK(old.text(node_pcdata, T("x<&>")));
				CHECK(old.text(node_cdata, T("a]]>b")));
				CHECK(old.text(node_comment, T("a--b-")));
				CHECK(old.processing_instruction(T("pi"), T("a?>b")));
				CHECK(old.end_element() && old.finish());

				string_writer sink;
				xml_sax_writer writer(sink);
				CHECK(!writer.reset(sink));
				CHECK(writer.finish());
				CHECK(writer.reset(sink, flags, encoding, 0));
				const xml_sax_attribute_view version_view[] = {{string_view_t(T("version-extra"), 7), string_view_t(T("1.0-extra"), 3)}};
				const xml_sax_attribute_view attrs_view[] = {{string_view_t(T("name-extra"), 4), string_view_t(T("a&b'\"extra"), 5)}};
				CHECK(writer.declaration(string_view_t(T("xml-extra"), 3), version_view));
				CHECK(writer.text(node_doctype, string_view_t(T("root-extra"), 4)));
				text_string name = T("root-extra");
				CHECK(writer.start_element(string_view_t(name.data(), 4), attrs_view));
				name[0] = T('x');
				CHECK(writer.text(node_pcdata, string_view_t(T("x<&>extra"), 4)));
				CHECK(writer.text(node_cdata, string_view_t(T("a]]>bextra"), 5)));
				CHECK(writer.text(node_comment, string_view_t(T("a--b-extra"), 5)));
				CHECK(writer.processing_instruction(string_view_t(T("pi-extra"), 2), string_view_t(T("a?>bextra"), 4)));
				CHECK(writer.end_element() && writer.finish());
				CHECK(sink.bytes == old_sink.bytes);
			}
	}

	void storage_reuse()
	{
		auto allocate = get_memory_allocation_function();
		auto deallocate = get_memory_deallocation_function();
		set_memory_management_functions(track_allocate, track_deallocate);
		remaining_allocations = static_cast<size_t>(-1);

		{
			const char_t small[] = T("<r><x a='1'>text</x></r>");
			auto bytes = std::as_bytes(std::span{small}.first<std::size(small) - 1>());
			xml_document_parser doc;
			xml_encoding encoding = sizeof(char_t) == 1 ? encoding_utf8 : encoding_wchar;
			CHECK(doc.load_buffer(bytes, parse_default, encoding));
			size_t before = remaining_allocations;
			CHECK(doc.load_buffer(bytes, parse_default, encoding));
		#ifndef PUGIXML_COMPACT
			CHECK(remaining_allocations == before);
		#endif
			CHECK(doc.document().document_element().child(T("x")).text().as_string() == string_view_t(T("text")));
			xml_document_parser moved(std::move(doc));
			CHECK(moved.load_buffer(bytes, parse_default, encoding));
			CHECK(moved.document().document_element().child(T("x")));
			moved.reset();
			CHECK(!moved.document().first_child());
		}
		CHECK(live_bytes == 0);

		{
			const std::string large = "<r>" + std::string(120000, 'x') + "</r>";
			xml_sax_view_handler handler;
			if constexpr (sizeof(char_t) == 1)
			{
				size_t before = remaining_allocations;
				CHECK(xml_sax_parse_buffer(large.data(), large.size(), handler));
				CHECK(remaining_allocations == before && live_bytes == 0);
			}
			xml_sax_parser parser(128 * 1024);
			short_reader first(large, 65536);
			CHECK(parser.parse(first, handler));
			size_t before = remaining_allocations;
			short_reader second(large, 65536);
			CHECK(parser.parse(second, handler));
		#ifndef PUGIXML_WCHAR_MODE
			CHECK(remaining_allocations == before);
		#endif
		}
		CHECK(live_bytes == 0);

		{
			const std::string large = "<r>" + std::string(120000, 'x') + "</r>";
			xml_sax_parser parser(0);
			xml_sax_view_handler handler;
			size_t initial = live_bytes;
			short_reader reader(large, 65536);
			CHECK(parser.parse(reader, handler));
			CHECK(live_bytes == initial);
		}
		CHECK(live_bytes == 0);

		{
			string_writer sink;
			xml_sax_writer writer(sink);
			size_t initial = live_bytes;
			CHECK(writer.start_element(T("first")) && writer.end_element() && writer.finish());
			size_t before = remaining_allocations;
			CHECK(writer.reset(sink));
			CHECK(writer.start_element(T("second")) && writer.end_element() && writer.finish());
			CHECK(sink.bytes == "<first/><second/>");
			CHECK(remaining_allocations == before);
			CHECK(writer.reset(sink));
			const text_string name(1000, T('r'));
			CHECK(writer.start_element(string_view_t(name)) && writer.end_element() && writer.finish());
			CHECK(live_bytes > initial);
			CHECK(writer.reset(sink, format_raw, encoding_auto, 0));
			CHECK(live_bytes == initial);
		}
		CHECK(live_bytes == 0);
		set_memory_management_functions(allocate, deallocate);
	}

#ifndef PUGIXML_NO_EXCEPTIONS
	void writer_sink_failure()
	{
		struct sink_error {};
		struct throwing_writer: xml_writer
		{
			size_t writes = 0;
			void write(const void*, size_t) override
			{
				++writes;
				throw sink_error();
			}
		} sink;

		xml_sax_writer writer(sink);
		CHECK(writer.start_element(T("root")));
		try
		{
			const text_string content(65536, T('x'));
			writer.text(node_pcdata, content.c_str());
			CHECK(false);
		}
		catch (const sink_error&)
		{
			CHECK(sink.writes == 1);
		}

		CHECK(!writer.end_element());
		CHECK(!writer.finish());
		CHECK(!writer.reset(sink));
		CHECK(sink.writes == 1);
	}
#endif
}

int main()
{
	options_and_encoding();
	window_boundaries();
	streaming_tokens();
	errors();
	lambda_callbacks();
	memory_usage();
	write_and_read();
	writer_tag_lengths();
	length_aware_reading();
	length_aware_writing();
	storage_reuse();
#ifndef PUGIXML_NO_EXCEPTIONS
	writer_sink_failure();
#endif
}
