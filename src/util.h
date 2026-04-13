// util.h — UTF-8 string utilities: encoding, case conversion, comparison, formatting

#pragma once
#include <cstdint>
#include <format>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>

namespace u8
{
	constexpr uint32_t LEAD_SURROGATE_MIN = 0xd800u;
	constexpr uint32_t LEAD_SURROGATE_MAX = 0xdbffu;
	constexpr uint32_t TRAIL_SURROGATE_MIN = 0xdc00u;
	constexpr uint32_t TRAIL_SURROGATE_MAX = 0xdfffu;
	constexpr uint32_t LEAD_OFFSET = LEAD_SURROGATE_MIN - (0x10000 >> 10);
	constexpr uint32_t SURROGATE_OFFSET = 0xfca02400u; //  0x10000u - (LEAD_SURROGATE_MIN << 10) - TRAIL_SURROGATE_MIN;

	inline bool is_lead_surrogate(const uint32_t cp)
	{
		return cp >= LEAD_SURROGATE_MIN && cp <= LEAD_SURROGATE_MAX;
	}

	inline bool is_trail_surrogate(const uint32_t cp)
	{
		return cp >= TRAIL_SURROGATE_MIN && cp <= TRAIL_SURROGATE_MAX;
	}

	inline uint16_t mask16(const uint32_t oc)
	{
		return static_cast<uint16_t>(0xffff & oc);
	}

	constexpr bool is_utf8_continuation(const char8_t b)
	{
		return (static_cast<uint8_t>(b) & 0xC0) == 0x80;
	}

	constexpr int utf8_codepoint_count(const std::u8string_view s)
	{
		int count = 0;
		for (const auto b : s)
		{
			if (!is_utf8_continuation(b))
				count++;
		}
		return count;
	}

	constexpr size_t utf8_truncate(const std::u8string_view s, const int max_codepoints)
	{
		int cps = 0;
		size_t i = 0;
		while (i < s.size())
		{
			if (!is_utf8_continuation(s[i]))
			{
				if (cps >= max_codepoints)
					return i;
				cps++;
			}
			i++;
		}
		return i;
	}

	constexpr int utf8_next(const std::u8string_view s, int pos)
	{
		if (pos >= static_cast<int>(s.size())) return pos;
		pos++;
		while (pos < static_cast<int>(s.size()) && is_utf8_continuation(s[pos]))
			pos++;
		return pos;
	}

	constexpr int utf8_prev(const std::u8string_view s, int pos)
	{
		if (pos <= 0) return 0;
		pos--;
		while (pos > 0 && is_utf8_continuation(s[pos]))
			pos--;
		return pos;
	}

	std::u8string utf16_to_utf8(std::wstring_view wstr);
	std::u8string u32_to_utf8(std::u32string_view str);
	std::u32string utf8_to_u32(std::u8string_view str);
	std::wstring u32_to_wstr(std::u32string_view str);
	std::u32string wstr_to_u32(std::wstring_view str);

	constexpr uint32_t to_lower(const uint32_t c)
	{
		if (c < 128) return c >= U'A' && c <= U'Z' ? c - U'A' + U'a' : c;
		if (c > USHRT_MAX) return c;
		return towlower(c);
	}

	constexpr uint32_t to_upper(const uint32_t c)
	{
		if (c < 128) return c >= U'a' && c <= U'z' ? c - U'a' + U'A' : c;
		if (c > USHRT_MAX) return c;
		return towupper(c);
	}

	constexpr uint32_t pop_utf8_char(std::u8string_view::const_iterator& in_ptr,
	                                 const std::u8string_view::const_iterator& end)
	{
		const auto c1 = *in_ptr++;

		if (c1 < 0x80)
		{
			return c1;
		}
		if (c1 >> 5 == 0x6)
		{
			if (std::distance(in_ptr, end) < 1)
			{
				in_ptr = end;
				return 0;
			}

			uint32_t c = (c1 & 0x1F) << 6;
			c |= (*in_ptr++ & 0x3F) << 0;
			return c;
		}
		if (c1 >> 4 == 0xe)
		{
			if (std::distance(in_ptr, end) < 2)
			{
				in_ptr = end;
				return 0;
			}

			uint32_t c = (c1 & 0x0F) << 12;
			c |= (*in_ptr++ & 0x3F) << 6;
			c |= (*in_ptr++ & 0x3F) << 0;
			return c;
		}
		if (c1 >> 3 == 0x1e)
		{
			if (std::distance(in_ptr, end) < 3)
			{
				in_ptr = end;
				return 0;
			}

			uint32_t c = (c1 & 0x07) << 18;
			c |= (*in_ptr++ & 0x3F) << 12;
			c |= (*in_ptr++ & 0x3F) << 6;
			c |= (*in_ptr++ & 0x3F) << 0;
			return c;
		}

		return '?';
	}

	constexpr uint32_t peek_utf8_char(std::u8string_view::const_iterator in_ptr,
	                                  const std::u8string_view::const_iterator& end)
	{
		return pop_utf8_char(in_ptr, end);
	}

	inline std::u8string str_cast(const std::string_view s)
	{
		return {reinterpret_cast<const char8_t*>(s.data()), s.size()};
	}

	inline std::string_view str_cast(const std::u8string_view s)
	{
		return {reinterpret_cast<const char*>(s.data()), s.size()};
	}

	inline std::wstring utf8_to_utf16(const std::u8string_view s)
	{
		std::wstring result;
		result.reserve(s.size());
		auto i = s.begin();
		while (i < s.end())
		{
			const auto cp = pop_utf8_char(i, s.end());

			if (cp > 0xffff)
			{
				result += static_cast<uint16_t>((cp >> 10) + LEAD_OFFSET);
				result += static_cast<uint16_t>((cp & 0x3ff) + TRAIL_SURROGATE_MIN);
			}
			else
			{
				result += static_cast<uint16_t>(cp);
			}
		}
		return result;
	}

	template <class output_it>
	void char32_to_utf8(output_it&& inserter, const uint32_t ch)
	{
		if (ch < 0x80)
		{
			*inserter++ = static_cast<uint8_t>(ch);
		}
		else if (ch < 0x800)
		{
			*inserter++ = static_cast<uint8_t>(0xC0 | ch >> 6);
			*inserter++ = static_cast<uint8_t>(0x80 | ch >> 0 & 0x3F);
		}
		else if (ch < 0x10000)
		{
			*inserter++ = static_cast<uint8_t>(0xE0 | ch >> 12);
			*inserter++ = static_cast<uint8_t>(0x80 | ch >> 6 & 0x3F);
			*inserter++ = static_cast<uint8_t>(0x80 | ch >> 0 & 0x3F);
		}
		else
		{
			*inserter++ = static_cast<uint8_t>(ch >> 18 | 0xf0);
			*inserter++ = static_cast<uint8_t>(ch >> 12 & 0x3f | 0x80);
			*inserter++ = static_cast<uint8_t>(ch >> 6 & 0x3f | 0x80);
			*inserter++ = static_cast<uint8_t>(ch & 0x3f | 0x80);
		}
	}

	inline void utf16_to_utf8(const std::wstring_view s, std::u8string& result)
	{
		result.clear();
		result.reserve(std::max(result.capacity(), s.size()));
		auto inserter = std::back_inserter(result);

		auto start = s.begin();
		const auto end = s.end();

		while (start != end)
		{
			uint32_t cp = mask16(*start++);

			if (is_lead_surrogate(cp))
			{
				if (start != end)
				{
					const uint32_t trail_surrogate = mask16(*start++);

					if (is_trail_surrogate(trail_surrogate))
					{
						cp = (cp << 10) + trail_surrogate + SURROGATE_OFFSET;
					}
					else
					{
						throw std::invalid_argument("Invalid input string");
					}
				}
				else
				{
					throw std::invalid_argument("Invalid input string");
				}
			}
			else if (is_trail_surrogate(cp))
			{
				throw std::invalid_argument("Invalid input string");
			}

			char32_to_utf8(inserter, cp);
		}
	}

	inline std::u8string utf16_to_utf8(const std::wstring_view s)
	{
		std::u8string result;
		utf16_to_utf8(s, result);
		return result;
	};

	inline std::u8string to_lower(const std::u8string_view s)
	{
		std::u8string result;
		result.reserve(s.size());
		auto inserter = std::back_inserter(result);

		auto i = s.begin();
		while (i < s.end())
		{
			const auto cp = pop_utf8_char(i, s.end());
			char32_to_utf8(inserter, to_lower(cp));
		}

		return result;
	}

	inline std::u8string to_upper(const std::u8string_view s)
	{
		std::u8string result;
		result.reserve(s.size());
		auto inserter = std::back_inserter(result);

		auto i = s.begin();
		while (i < s.end())
		{
			const auto cp = pop_utf8_char(i, s.end());
			char32_to_utf8(inserter, to_upper(cp));
		}

		return result;
	}

	constexpr int icmp(const std::u8string_view ll, const std::u8string_view rr)
	{
		if (ll.data() == rr.data() || (ll.empty() && rr.empty())) return 0;
		if (ll.empty()) return -1;
		if (rr.empty()) return 1;

		auto cl = 0;
		auto cr = 0;

		auto il = ll.begin();
		auto ir = rr.begin();
		const auto el = ll.end();
		const auto er = rr.end();

		while (il < el && ir < er)
		{
			cl = to_lower(pop_utf8_char(il, el));
			cr = to_lower(pop_utf8_char(ir, er));
			if (cl < cr) return -1;
			if (cl > cr) return 1;
		}

		if (il == el) cl = 0;
		if (ir == er) cr = 0;
		return cl - cr;
	}

	static constexpr uint32_t FNV_PRIME_32 = 16777619u;
	static constexpr uint32_t OFFSET_BASIS_32 = 2166136261u;

	static constexpr uint64_t FNV_PRIME_64 = 1099511628211ULL;
	static constexpr uint64_t OFFSET_BASIS_64 = 14695981039346656037ULL;

	inline uint32_t fnv1a_i(std::u8string_view sv1)
	{
		auto p = sv1.begin();
		uint32_t result = OFFSET_BASIS_32;

		while (p < sv1.end())
		{
			result ^= to_lower(pop_utf8_char(p, sv1.end()));
			result *= FNV_PRIME_32;
		}

		return result;
	}

	inline uint64_t fnv1a_i_64(std::u8string_view sv1)
	{
		auto p = sv1.begin();
		uint64_t result = OFFSET_BASIS_64;

		while (p < sv1.end())
		{
			result ^= to_lower(pop_utf8_char(p, sv1.end()));
			result *= FNV_PRIME_64;
		}

		return result;
	}


	struct ihash
	{
		size_t operator()(const std::u8string_view s) const
		{
			return fnv1a_i(s);
		}
	};

	struct iless
	{
		bool operator()(const std::u8string_view l, const std::u8string_view r) const
		{
			return icmp(l, r) < 0;
		}
	};

	struct ieq
	{
		bool operator()(const std::u8string_view l, const std::u8string_view r) const
		{
			return icmp(l, r) == 0;
		}
	};

	namespace detail
	{
		template <typename T>
		auto to_fmt_arg(T&& arg)
		{
			using D = std::remove_cvref_t<T>;
			if constexpr (std::is_same_v<D, std::u8string_view> || std::is_same_v<D, std::u8string>)
			{
				const std::u8string_view sv = arg;
				return std::string_view(reinterpret_cast<const char*>(sv.data()), sv.size());
			}
			else if constexpr (std::is_same_v<D, const char8_t*> || std::is_same_v<D, char8_t*>)
			{
				return reinterpret_cast<const char*>(arg);
			}
			else
			{
				return std::forward<T>(arg);
			}
		}
	}

	template <typename... Args>
	[[nodiscard]] std::u8string format(const std::u8string_view fmt, Args&&... args)
	{
		const auto char_fmt = std::string_view(reinterpret_cast<const char*>(fmt.data()), fmt.size());
		auto arg_tuple = std::tuple{detail::to_fmt_arg(std::forward<Args>(args))...};
		return std::apply([&char_fmt](auto&... cargs) -> std::u8string
		{
			const auto r = std::vformat(char_fmt, std::make_format_args(cargs...));
			return std::u8string(reinterpret_cast<const char8_t*>(r.data()), r.size());
		}, arg_tuple);
	}

	template <>
	[[nodiscard]] inline std::u8string format(const std::u8string_view fmt)
	{
		const auto char_fmt = std::string_view(reinterpret_cast<const char*>(fmt.data()), fmt.size());
		const auto r = std::vformat(char_fmt, std::make_format_args());
		return std::u8string(reinterpret_cast<const char8_t*>(r.data()), r.size());
	}


	[[nodiscard]] constexpr bool is_empty(const char8_t* sz)
	{
		return sz == nullptr || sz[0] == 0;
	}

	[[nodiscard]] constexpr bool is_empty(const wchar_t* sz)
	{
		return sz == nullptr || sz[0] == 0;
	}

	inline int32_t stoi(const std::u8string_view u8_string)
	{
		const auto sv = std::string_view(reinterpret_cast<const char*>(u8_string.data()), u8_string.size());
		int32_t result = 0;
		std::from_chars(sv.data(), sv.data() + sv.size(), result);
		return result;
	}

	inline double stod(const std::u8string_view u8_string)
	{
		const auto sv = std::string_view(reinterpret_cast<const char*>(u8_string.data()), u8_string.size());
		double result = 0.0;
		std::from_chars(sv.data(), sv.data() + sv.size(), result);
		return result;
	}
}
