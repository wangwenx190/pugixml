#ifndef HEADER_PUGIXML_DOM_HPP
#define HEADER_PUGIXML_DOM_HPP

#include "pugixml.hpp"

#ifndef PUGIXML_NO_STL
#include <charconv>
#include <expected>
#include <ranges>
#include <system_error>
#include <type_traits>
#endif

namespace pugi
{
	// A borrowed lookup key. The name must stay valid until the lookup ends.
	class attribute_name
	{
		friend class xml_node_view;

	public:
		explicit attribute_name(const char_t* name): _data(name)
		#ifdef PUGIXML_HAS_STRING_VIEW
			, _size(string_view_t::npos)
		#endif
		{
		}

	#ifdef PUGIXML_HAS_STRING_VIEW
		explicit attribute_name(string_view_t name): _data(name.data()), _size(name.size())
		{
		}
	#endif

	private:
		const char_t* _data;

	#ifdef PUGIXML_HAS_STRING_VIEW
		size_t _size;
	#endif
	};

	// A non-owning DOM handle with indexed lookup. Its lifetime and constness
	// follow xml_node. Lookups return empty handles when the name is absent.
	class xml_node_view
	{
	public:
		xml_node_view() {}
		explicit xml_node_view(xml_node node): _node(node) {}

		explicit operator bool() const { return !_node.empty(); }
		bool empty() const { return _node.empty(); }

		xml_node node() const { return _node; }
		xml_text text() const { return _node.text(); }

		xml_node_view operator[](const char_t* name) const
		{
			return xml_node_view(_node.child(name));
		}

	#ifdef PUGIXML_HAS_STRING_VIEW
		xml_node_view operator[](string_view_t name) const
		{
			return xml_node_view(_node.child(name));
		}
	#endif

		xml_attribute operator[](attribute_name name) const
		{
		#ifdef PUGIXML_HAS_STRING_VIEW
			// npos selects the native null-terminated lookup.
			if (name._size != string_view_t::npos)
				return _node.attribute(string_view_t(name._data, name._size));
		#endif
			return _node.attribute(name._data);
		}

	#ifndef PUGIXML_NO_STL
		// The document and, for named children, the name must outlive traversal.
		auto children() const
		{
			return std::ranges::subrange(_node.begin(), _node.end())
				| std::views::transform([](xml_node node) { return xml_node_view(node); });
		}

		auto children(const char_t* name) const
		{
			auto range = _node.children(name);
			return std::ranges::subrange(range.begin(), range.end())
				| std::views::transform([](xml_node node) { return xml_node_view(node); });
		}
	#endif

	private:
		xml_node _node;
	};

#ifndef PUGIXML_NO_STL
	enum class xml_value_error
	{
		missing,
		invalid,
		out_of_range
	};

	namespace detail
	{
		template <typename T>
		concept xml_number = std::same_as<T, std::remove_cv_t<T>> &&
			(std::integral<T> || std::floating_point<T>) &&
			requires (const char* data, T& result) { std::from_chars(data, data, result); };

		template <typename T>
		concept xml_value_type = std::same_as<T, bool> || xml_number<T>;

		template <xml_number T>
		std::expected<T, xml_value_error> read_xml_number(std::string_view value)
		{
		if constexpr (std::floating_point<T>)
		{
			// from_chars also recognizes infinity and NaN. Require a decimal
			// prefix so these spellings are rejected even under fast-math.
			auto magnitude = value;
			if (magnitude.starts_with('-')) magnitude.remove_prefix(1);
			if (magnitude.empty() || (magnitude.front() != '.' && (magnitude.front() < '0' || magnitude.front() > '9')))
				return std::unexpected(xml_value_error::invalid);
		}

			T result{};
			const char* end = value.data() + value.size();
			auto parsed = std::from_chars(value.data(), end, result);
			if (parsed.ptr != end || parsed.ec == std::errc::invalid_argument)
				return std::unexpected(xml_value_error::invalid);
			if (parsed.ec == std::errc::result_out_of_range)
				return std::unexpected(xml_value_error::out_of_range);
			return result;
		}

		template <xml_value_type T>
		std::expected<T, xml_value_error> read_xml_value(const char_t* text)
		{
			if (!text) return std::unexpected(xml_value_error::missing);

			string_view_t value(text);
			const auto first = value.find_first_not_of(PUGIXML_TEXT(" \t\r\n"));
			if (first == string_view_t::npos) return std::unexpected(xml_value_error::invalid);
			value = value.substr(first, value.find_last_not_of(PUGIXML_TEXT(" \t\r\n")) - first + 1);

			if constexpr (std::same_as<T, bool>)
			{
				if (value == PUGIXML_TEXT("true") || value == PUGIXML_TEXT("1")) return true;
				if (value == PUGIXML_TEXT("false") || value == PUGIXML_TEXT("0")) return false;
				return std::unexpected(xml_value_error::invalid);
			}
			else
			{
				if (value.front() == '+')
				{
					value.remove_prefix(1);
					if (value.empty() || value.front() == '+' || value.front() == '-')
						return std::unexpected(xml_value_error::invalid);
				}

			#ifdef PUGIXML_WCHAR_MODE
				// from_chars accepts narrow input. Keep short wide values on the stack.
				char storage[128];
				std::string long_value;
				char* narrow = storage;
				if (value.size() > sizeof(storage))
				{
					long_value.resize(value.size());
					narrow = long_value.data();
				}
				for (size_t i = 0; i < value.size(); ++i)
				{
					if (static_cast<unsigned int>(value[i]) > 127) return std::unexpected(xml_value_error::invalid);
					narrow[i] = static_cast<char>(value[i]);
				}
				return read_xml_number<T>({narrow, value.size()});
			#else
				return read_xml_number<T>(value);
			#endif
			}
		}
	}

	// Reads a complete decimal number or one of true, false, 1 and 0.
	// Leading and trailing XML whitespace are accepted.
	template <detail::xml_value_type T>
	std::expected<T, xml_value_error> try_value(xml_attribute attribute)
	{
		const char_t* value = attribute.as_string(nullptr);
		return detail::read_xml_value<T>(value || !attribute ? value : PUGIXML_TEXT(""));
	}

	template <detail::xml_value_type T>
	std::expected<T, xml_value_error> try_value(xml_text text)
	{
		const char_t* value = text.as_string(nullptr);
		return detail::read_xml_value<T>(value || !text ? value : PUGIXML_TEXT(""));
	}
#endif
}

#endif
