#include "pugixml_dom.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

#define CHECK(expression) do { if (!(expression)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #expression); std::abort(); } } while (false)
#define T(value) PUGIXML_TEXT(value)

namespace
{
	using namespace pugi;

	allocation_function original_allocate;
	size_t allocation_count;

	void* count_allocation(size_t size)
	{
		++allocation_count;
		return original_allocate(size);
	}

	void lookup()
	{
		static_assert(sizeof(xml_node_view) == sizeof(xml_node), "The view stores a single node handle");

		xml_document doc;
		CHECK(doc.load_string(T("<config><server port='443' port='999' empty=''><port>8080</port>"
			"<port>9090</port><enabled>false</enabled></server><server port='7000'/></config>")));

		original_allocate = get_memory_allocation_function();
		deallocation_function deallocate = get_memory_deallocation_function();
		set_memory_management_functions(count_allocation, deallocate);

		const xml_node_view root(doc.document_element());
		const xml_node native_server = doc.document_element().child(T("server"));
		const xml_node_view server = root[T("server")];
		CHECK(server && server.node() == native_server);
		CHECK(server[T("port")].node() == native_server.child(T("port")));
		CHECK(server[T("port")].text().as_int() == 8080);
		CHECK(server[attribute_name(T("port"))] == native_server.attribute(T("port")));
		CHECK(server[attribute_name(T("port"))].as_int() == 443);
		CHECK(server[attribute_name(T("empty"))]);

		const xml_node_view missing = root[T("missing")][T("port")];
		CHECK(!missing && missing.empty() && !missing.node());
		CHECK(missing.text().as_int(9000) == 9000);
		CHECK(!missing[attribute_name(T("port"))]);
		CHECK(!server[attribute_name(T("missing"))]);
		CHECK(!xml_node_view()[T("server")]);

	#ifdef PUGIXML_HAS_STRING_VIEW
		const string_view_t server_name(T("server-suffix"), 6);
		const string_view_t port_name(T("port-suffix"), 4);
		CHECK(root[server_name][port_name].node() == native_server.child(T("port")));
		CHECK(root[server_name][attribute_name(port_name)] == native_server.attribute(T("port")));
	#endif

	#ifndef PUGIXML_NO_STL
		auto ports = server.children(T("port"));
		static_assert(std::ranges::bidirectional_range<decltype(ports)>);
		CHECK(std::ranges::distance(ports) == 2);
		CHECK((*ports.begin()).text().as_int() == 8080);
		CHECK((*std::ranges::prev(ports.end())).text().as_int() == 9090);
		CHECK(std::ranges::distance(server.children()) == 3);
		CHECK(std::ranges::empty(missing.children()));
		CHECK(std::ranges::empty(server.children(T("missing"))));
		CHECK(try_value<int>(server[T("port")].text()) == 8080);
		CHECK(try_value<int>(server[attribute_name(T("port"))]) == 443);
	#endif

		set_memory_management_functions(original_allocate, deallocate);
		CHECK(allocation_count == 0);

		server[T("enabled")].text() = true;
		server[attribute_name(T("port"))] = 8443;
		CHECK(native_server.child(T("enabled")).text().as_bool());
		CHECK(native_server.attribute(T("port")).as_int() == 8443);
		CHECK(native_server.attribute(T("port")).next_attribute().as_int() == 999);

		server[T("missing")].text() = true;
		server[attribute_name(T("missing"))] = true;
		CHECK(!native_server.child(T("missing")) && !native_server.attribute(T("missing")));
	}

#ifndef PUGIXML_NO_STL
	void checked_values()
	{
		const auto missing = std::unexpected(xml_value_error::missing);
		const auto invalid = std::unexpected(xml_value_error::invalid);
		const auto out_of_range = std::unexpected(xml_value_error::out_of_range);
		xml_document doc;
		xml_node root = doc.append_child(T("r"));
		xml_attribute attr = root.append_attribute(T("number"));
		CHECK(try_value<int>(xml_text()) == missing);
		CHECK(try_value<int>(xml_attribute()) == missing);
		CHECK(try_value<int>(attr) == invalid);
		CHECK(attr.set_value(T(" \t +42\r\n ")));
		CHECK(try_value<int>(attr) == 42);
		CHECK(attr.set_value((std::numeric_limits<int>::min)()));
		CHECK(try_value<int>(attr) == (std::numeric_limits<int>::min)());
		CHECK(attr.set_value((std::numeric_limits<unsigned int>::max)()));
		CHECK(try_value<unsigned int>(attr) == (std::numeric_limits<unsigned int>::max)());
		CHECK(attr.set_value(T("999999999999999999999999999999999999999")));
		CHECK(try_value<int>(attr) == out_of_range);

		for (const char_t* value: {T(""), T(" \t"), T("42x"), T("0x2a"), T("+-1"), T("+"), T("4 2")})
		{
			CHECK(attr.set_value(value));
			CHECK(try_value<int>(attr) == invalid);
		}
		CHECK(attr.set_value(T("-1")));
		CHECK(try_value<unsigned int>(attr) == invalid);
		CHECK(attr.set_value(T(" +1.25e2 ")));
		CHECK(try_value<double>(attr) == 125.0);
		CHECK(try_value<float>(attr) == 125.0f);
		CHECK(attr.set_value(T("1e9999")));
		CHECK(try_value<double>(attr) == out_of_range);
		CHECK(attr.set_value(T("1e-9999")));
		CHECK(try_value<double>(attr) == out_of_range);
		CHECK(attr.set_value(T("nan")));
		CHECK(try_value<double>(attr) == invalid);
		CHECK(attr.set_value(T("infinity")));
		CHECK(try_value<double>(attr) == invalid);
		CHECK(attr.set_value(T("-NaN")));
		CHECK(try_value<double>(attr) == invalid);
		CHECK(attr.set_value(T("+INF")));
		CHECK(try_value<double>(attr) == invalid);
		CHECK(attr.set_value(T("-.5")));
		CHECK(try_value<double>(attr) == -0.5);

		CHECK(root.text().set(T(" true\t")));
		CHECK(try_value<bool>(root.text()) == true);
		CHECK(root.text().set(T("0")));
		CHECK(try_value<bool>(root.text()) == false);
		CHECK(root.text().set(T("yes")));
		CHECK(try_value<bool>(root.text()) == invalid);
		CHECK(root.text().set(T("true false")));
		CHECK(try_value<bool>(root.text()) == invalid);

		string_t long_value(200, T('0'));
		long_value += T("42.5");
		CHECK(root.text().set(long_value.c_str()));
		CHECK(try_value<double>(root.text()) == 42.5);
	}

	void buffers()
	{
		const char copied[] = "<copy/>";
		const char appended[] = "<child/>";
		char_t in_place[] = T("<borrowed/>");
		xml_document doc;

		CHECK(doc.load_buffer(std::as_bytes(std::span{copied}.first<sizeof(copied) - 1>())));
		CHECK(doc.document_element().append_buffer(std::as_bytes(std::span{appended}.first<sizeof(appended) - 1>())));
		CHECK(doc.document_element().child(T("child")));

		auto bytes = std::as_writable_bytes(std::span{in_place}.first<std::size(in_place) - 1>());
		CHECK(doc.load_buffer_inplace(bytes, parse_default, sizeof(char_t) == 1 ? encoding_utf8 : encoding_wchar));
		CHECK(doc.document_element().name() == string_view_t(T("borrowed")));
		CHECK(doc.load_buffer({}, 0, parse_fragment));
		CHECK(doc.load_buffer(std::span<const std::byte>(), parse_fragment));

	#ifdef PUGIXML_WCHAR_MODE
		const wchar_t native_xml[] = L"<unaligned/>";
		const auto native_bytes = std::as_bytes(std::span{native_xml}.first<std::size(native_xml) - 1>());
		alignas(wchar_t) std::byte storage[sizeof(native_xml) - sizeof(wchar_t) + 1];
		auto unaligned = std::span{storage}.subspan(1);
		std::memcpy(unaligned.data(), native_bytes.data(), native_bytes.size());
		CHECK(doc.load_buffer_inplace(unaligned, parse_default, encoding_wchar));
		CHECK(doc.document_element().name() == string_view_t(T("unaligned")));

		for (size_t offset = 0; offset < unaligned.size(); offset += sizeof(wchar_t))
			for (size_t i = 0; i < sizeof(wchar_t); ++i)
				unaligned[offset + i] = native_bytes[offset + sizeof(wchar_t) - 1 - i];

		const bool little_endian = native_bytes.front() == std::byte{'<'};
		const xml_encoding swapped = sizeof(wchar_t) == 2
			? (little_endian ? encoding_utf16_be : encoding_utf16_le)
			: (little_endian ? encoding_utf32_be : encoding_utf32_le);
		CHECK(doc.load_buffer(unaligned, parse_default, swapped));
		CHECK(doc.document_element().name() == string_view_t(T("unaligned")));
		CHECK(doc.load_buffer_inplace(unaligned, parse_default, swapped));
		CHECK(doc.document_element().name() == string_view_t(T("unaligned")));
	#endif
	}
#endif
}

int main()
{
	lookup();
#ifndef PUGIXML_NO_STL
	checked_values();
	buffers();
#endif
}
