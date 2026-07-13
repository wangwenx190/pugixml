#include "pugixml.hpp"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#if defined(__APPLE__) || defined(__linux__)
#	include <sys/resource.h>
#endif

namespace
{
	const char row[] = "<row id=\"123456\" kind=\"sample\"><name>Alpha &amp; beta</name><value>123.45</value></row>";
	const char payload[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_: repeated text payload. ";
	const pugi::xml_sax_attribute attrs[] = {{PUGIXML_TEXT("id"), PUGIXML_TEXT("123456")}, {PUGIXML_TEXT("kind"), PUGIXML_TEXT("sample")}};

	struct count_handler: pugi::xml_sax_handler
	{
		size_t elements = 0, attributes = 0, text_size = 0;

		bool start_element(const pugi::char_t*, const pugi::xml_sax_attribute*, size_t count) override
		{
			++elements;
			attributes += count;
			return true;
		}

		bool text(pugi::xml_node_type, const pugi::char_t* value) override
		{
		#ifdef PUGIXML_WCHAR_MODE
			text_size += std::wcslen(value);
		#else
			text_size += std::strlen(value);
		#endif
			return true;
		}
	};

	struct dom_counter: pugi::xml_tree_walker
	{
		count_handler& count;
		explicit dom_counter(count_handler& count_): count(count_) {}

		bool for_each(pugi::xml_node& node) override
		{
			if (node.type() == pugi::node_element)
			{
				size_t attributes = 0;
				for (pugi::xml_attribute attr = node.first_attribute(); attr; attr = attr.next_attribute())
					++attributes;
				count.start_element(node.name(), nullptr, attributes);
			}
			else
				count.text(node.type(), node.value());
			return true;
		}
	};

	struct count_writer: pugi::xml_writer
	{
		size_t bytes = 0;
		void write(const void*, size_t size) override { bytes += size; }
	};

	bool generate(const char* path, size_t rows, bool long_text)
	{
		std::FILE* file = std::fopen(path, "wb");
		if (!file) return false;

		bool success = std::fputs("<root>", file) != EOF;
		for (size_t i = 0; i < rows && success; ++i)
			success = std::fputs(long_text ? payload : row, file) != EOF;
		if (success) success = std::fputs("</root>", file) != EOF;
		return std::fclose(file) == 0 && success;
	}

	bool write_sax(count_writer& sink, size_t rows)
	{
		pugi::xml_sax_writer writer(sink);
		if (!writer.start_element(PUGIXML_TEXT("root"))) return false;
		for (size_t i = 0; i < rows; ++i)
			if (!writer.start_element(PUGIXML_TEXT("row"), attrs, 2) ||
				!writer.start_element(PUGIXML_TEXT("name")) ||
				!writer.text(pugi::node_pcdata, PUGIXML_TEXT("Alpha & beta")) ||
				!writer.end_element() ||
				!writer.start_element(PUGIXML_TEXT("value")) ||
				!writer.text(pugi::node_pcdata, PUGIXML_TEXT("123.45")) ||
				!writer.end_element() ||
				!writer.end_element()) return false;
		return writer.end_element() && writer.finish();
	}
}

int main(int argc, char** argv)
{
	if (argc != 4)
	{
		std::fprintf(stderr, "Usage: %s generate|generate-text|dom-read|dom-visit|sax-read|dom-write|sax-write FILE ROWS\n", argv[0]);
		return 1;
	}

	const char* mode = argv[1];
	const char* path = argv[2];
	char* end = nullptr;
	errno = 0;
	const unsigned long long count_arg = std::strtoull(argv[3], &end, 10);
	const size_t rows = static_cast<size_t>(count_arg);
	if (argv[3][0] < '0' || argv[3][0] > '9' || *end || errno == ERANGE || rows != count_arg)
	{
		std::fputs("ROWS must be a nonnegative integer representable as size_t.\n", stderr);
		return 1;
	}
	if (std::strcmp(mode, "generate") == 0 || std::strcmp(mode, "generate-text") == 0)
		return generate(path, rows, std::strcmp(mode, "generate-text") == 0) ? 0 : 1;

	const bool dom_write = std::strcmp(mode, "dom-write") == 0;
	pugi::xml_document doc;
	if (dom_write && !doc.load_file(path)) return 1;

	count_handler count;
	count_writer sink;
	pugi::xml_parse_result result;
	result.status = pugi::status_ok;
	auto start = std::chrono::steady_clock::now();
	if (std::strcmp(mode, "sax-read") == 0)
		result = pugi::xml_sax_parse_file(path, count);
	else if (std::strcmp(mode, "dom-read") == 0)
		result = doc.load_file(path);
	else if (std::strcmp(mode, "dom-visit") == 0)
	{
		result = doc.load_file(path);
		dom_counter walker(count);
		if (result) doc.traverse(walker);
	}
	else if (dom_write)
		doc.save(sink, PUGIXML_TEXT(""), pugi::format_raw | pugi::format_no_declaration);
	else if (std::strcmp(mode, "sax-write") == 0)
	{
		if (!write_sax(sink, rows)) return 1;
	}
	else return 1;

	double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	if (!result)
	{
		std::fprintf(stderr, "%s at offset %td\n", result.description(), result.offset);
		return 1;
	}

	std::printf("%s %.6f s bytes=%zu elements=%zu attributes=%zu text=%zu",
		mode, seconds, sink.bytes, count.elements, count.attributes, count.text_size);
#if defined(__APPLE__) || defined(__linux__)
	struct rusage usage = {};
	if (getrusage(RUSAGE_SELF, &usage) == 0)
	{
		double rss = static_cast<double>(usage.ru_maxrss) / 1024;
	#ifdef __APPLE__
		rss /= 1024;
	#endif
		std::printf(" peak_rss=%.2f MiB", rss);
	}
#endif
	std::puts("");
}
