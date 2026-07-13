#ifndef HEADER_PUGIXML_SAX_HPP
#define HEADER_PUGIXML_SAX_HPP

#include "pugixml.hpp"

#ifndef PUGIXML_NO_STL
#include <functional>
#include <tuple>
#include <type_traits>
#include <utility>

namespace pugi
{
	// Event strings and attribute spans borrow data for the callback duration.
	namespace sax
	{
		struct start_element { const char_t* name; std::span<const xml_sax_attribute> attributes; };
		struct end_element { const char_t* name; };
		struct text { xml_node_type type; const char_t* value; };
		struct processing_instruction { const char_t* name; const char_t* value; };
		struct declaration { const char_t* name; std::span<const xml_sax_attribute> attributes; };
		struct start_attribute { const char_t* name; };
		struct attribute_value { const char_t* value; };
		struct end_attribute {};
		struct end_start_element {};
		struct comment_chunk { const char_t* value; bool last; };
	}

	namespace detail
	{
		template <typename Callback>
		concept sax_callback =
			std::invocable<Callback&, const sax::start_element&> ||
			std::invocable<Callback&, const sax::end_element&> ||
			std::invocable<Callback&, const sax::text&> ||
			std::invocable<Callback&, const sax::processing_instruction&> ||
			std::invocable<Callback&, const sax::declaration&> ||
			std::invocable<Callback&, const sax::start_attribute&> ||
			std::invocable<Callback&, const sax::attribute_value&> ||
			std::invocable<Callback&, const sax::end_attribute&> ||
			std::invocable<Callback&, const sax::end_start_element&> ||
			std::invocable<Callback&, const sax::comment_chunk&>;

		template <typename... Callbacks>
		class sax_callback_handler final: public xml_sax_handler
		{
		public:
			template <typename... F>
			requires std::constructible_from<std::tuple<Callbacks...>, F...>
			explicit sax_callback_handler(F&&... callbacks): _callbacks(std::forward<F>(callbacks)...)
			{
			}

			bool start_element(const char_t* name, const xml_sax_attribute* attributes, size_t count) override
			{
				return dispatch(sax::start_element{name, {attributes, count}});
			}

			bool end_element(const char_t* name) override { return dispatch(sax::end_element{name}); }
			bool text(xml_node_type type, const char_t* value) override { return dispatch(sax::text{type, value}); }

			bool processing_instruction(const char_t* name, const char_t* value) override
			{
				return dispatch(sax::processing_instruction{name, value});
			}

			bool declaration(const char_t* name, const xml_sax_attribute* attributes, size_t count) override
			{
				return dispatch(sax::declaration{name, {attributes, count}});
			}

			bool start_attribute(const char_t* name) override { return dispatch(sax::start_attribute{name}); }
			bool attribute_value(const char_t* value) override { return dispatch(sax::attribute_value{value}); }
			bool end_attribute() override { return dispatch(sax::end_attribute{}); }
			bool end_start_element() override { return dispatch(sax::end_start_element{}); }
			bool comment_chunk(const char_t* value, bool last) override { return dispatch(sax::comment_chunk{value, last}); }

		private:
			template <typename Callback, typename Event>
			static bool notify(Callback& callback, const Event& event)
			{
				if constexpr (!std::invocable<Callback&, const Event&>)
					return true;
				else if constexpr (std::is_void_v<std::invoke_result_t<Callback&, const Event&>>)
				{
					std::invoke(callback, event);
					return true;
				}
				else
				{
					static_assert(std::same_as<std::invoke_result_t<Callback&, const Event&>, bool>,
						"SAX callbacks must return void or bool");
					return std::invoke(callback, event);
				}
			}

			template <typename Event>
			bool dispatch(const Event& event)
			{
				return std::apply([&](auto&... callbacks) { return (notify(callbacks, event) && ...); }, _callbacks);
			}

			std::tuple<Callbacks...> _callbacks;
		};
	}

	// Matching callbacks run in argument order. Returning false aborts parsing.
	// Callbacks are stored by value. Use std::ref to borrow an existing callable.
	template <typename... Callbacks>
	requires (sizeof...(Callbacks) > 0 && (detail::sax_callback<std::decay_t<Callbacks>> && ...))
	auto make_sax_handler(Callbacks&&... callbacks)
	{
		return detail::sax_callback_handler<std::decay_t<Callbacks>...>(std::forward<Callbacks>(callbacks)...);
	}
}
#endif

#endif
