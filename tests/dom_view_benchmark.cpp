#include "pugixml_dom.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>

namespace
{
	using namespace pugi;

	constexpr size_t root_count = 4096;
	constexpr size_t iterations = 1000000;
	constexpr size_t trials = 9;

	unsigned int named_lookup(xml_node root)
	{
		xml_node server = root.child(PUGIXML_TEXT("server"));
		unsigned int port = server.child(PUGIXML_TEXT("network")).child(PUGIXML_TEXT("port")).text().as_uint();
		return port * 2 + server.attribute(PUGIXML_TEXT("enabled")).as_bool();
	}

	unsigned int view_lookup(xml_node root)
	{
		xml_node_view server = xml_node_view(root)[PUGIXML_TEXT("server")];
		unsigned int port = server[PUGIXML_TEXT("network")][PUGIXML_TEXT("port")].text().as_uint();
		return port * 2 + server[attribute_name(PUGIXML_TEXT("enabled"))].as_bool();
	}

	struct measurement
	{
		double ns_per_lookup;
		unsigned long long checksum;
	};

	// Both lookup functions run through the same timed loop.
#if defined(_MSC_VER)
	__declspec(noinline)
#elif defined(__GNUC__)
	__attribute__((noinline))
#endif
	measurement measure(const xml_node* roots, size_t mask, unsigned int (*lookup)(xml_node))
	{
		unsigned long long checksum = 0;
		auto start = std::chrono::steady_clock::now();
		for (size_t i = 0; i < iterations; ++i)
			checksum += lookup(roots[i & mask]);
		auto elapsed = std::chrono::steady_clock::now() - start;

		return {std::chrono::duration<double, std::nano>(elapsed).count() / iterations, checksum};
	}

	bool compare(const xml_node* roots, size_t count)
	{
		std::array<double, trials> named_times, view_times;
		unsigned long long checksum = 0;
		for (size_t i = 0; i <= trials; ++i)
		{
			measurement named, view;
			if (i % 2 == 0)
			{
				named = measure(roots, count - 1, named_lookup);
				view = measure(roots, count - 1, view_lookup);
			}
			else
			{
				view = measure(roots, count - 1, view_lookup);
				named = measure(roots, count - 1, named_lookup);
			}
			if (named.checksum != view.checksum || named.checksum == 0) return false;
			if (i == 0) continue;

			checksum = named.checksum;
			named_times[i - 1] = named.ns_per_lookup;
			view_times[i - 1] = view.ns_per_lookup;
		}

		std::sort(named_times.begin(), named_times.end());
		std::sort(view_times.begin(), view_times.end());
		std::printf("roots=%zu named=%.3f ns view=%.3f ns checksum=%llu\n",
			count, named_times[trials / 2], view_times[trials / 2], checksum);
		return true;
	}
}

int main()
{
	pugi::xml_document doc;
	pugi::xml_node configurations = doc.append_child(PUGIXML_TEXT("configurations"));
	std::array<pugi::xml_node, root_count> roots;
	for (size_t i = 0; i < roots.size(); ++i)
	{
		roots[i] = configurations.append_child(PUGIXML_TEXT("config"));
		pugi::xml_node server = roots[i].append_child(PUGIXML_TEXT("server"));
		if (!server.append_attribute(PUGIXML_TEXT("enabled")).set_value((i & 1) != 0)) return 1;
		if (!server.append_child(PUGIXML_TEXT("network")).append_child(PUGIXML_TEXT("port")).text().set(
			8080 + static_cast<unsigned int>(i))) return 1;
	}

	return compare(roots.data(), 1) && compare(roots.data(), roots.size()) ? 0 : 1;
}
