// json_parser.h — Recursive-descent JSON parser producing u8string-based values

#pragma once

#include <string>
#include <vector>
#include <map>
#include <variant>
#include <stdexcept>
#include <cctype>
#include <charconv>

struct json_value;
using json_object = std::map<std::u8string, json_value, std::less<>>;
using json_array = std::vector<json_value>;

struct json_value
{
	std::variant<std::nullptr_t, bool, double, std::u8string, std::u8string_view, json_array, json_object> data;

	bool is_null() const { return std::holds_alternative<std::nullptr_t>(data); }
	bool is_bool() const { return std::holds_alternative<bool>(data); }
	bool is_number() const { return std::holds_alternative<double>(data); }

	bool is_string() const
	{
		return std::holds_alternative<std::u8string>(data) || std::holds_alternative<std::u8string_view>(data);
	}

	bool is_array() const { return std::holds_alternative<json_array>(data); }
	bool is_object() const { return std::holds_alternative<json_object>(data); }

	std::u8string as_string() const
	{
		if (auto* sv = std::get_if<std::u8string_view>(&data)) return std::u8string(*sv);
		return std::get<std::u8string>(data);
	}

	double as_number() const { return std::get<double>(data); }
	const json_array& as_array() const { return std::get<json_array>(data); }
	const json_object& as_object() const { return std::get<json_object>(data); }

	const json_value& operator[](const std::u8string_view key) const
	{
		static const json_value null_val{nullptr};
		if (!is_object()) return null_val;
		const auto it = as_object().find(key);
		return it != as_object().end() ? it->second : null_val;
	}

	const json_value& operator[](const size_t idx) const
	{
		static const json_value null_val{nullptr};
		if (!is_array() || idx >= as_array().size()) return null_val;
		return as_array()[idx];
	}
};

class json_parser
{
	std::u8string_view src_;
	size_t pos_ = 0;

	void skip_ws() { while (pos_ < src_.size() && isspace(static_cast<unsigned char>(src_[pos_]))) pos_++; }

	char peek()
	{
		skip_ws();
		return pos_ < src_.size() ? static_cast<char>(src_[pos_]) : '\0';
	}

	char next()
	{
		skip_ws();
		return pos_ < src_.size() ? static_cast<char>(src_[pos_++]) : '\0';
	}

	void append_unicode_cp(std::u8string& r, const unsigned long cp)
	{
		if (cp < 0x80) r += static_cast<char8_t>(cp);
		else if (cp < 0x800)
		{
			r += static_cast<char8_t>(0xC0 | (cp >> 6));
			r += static_cast<char8_t>(0x80 | (cp & 0x3F));
		}
		else if (cp < 0x10000)
		{
			r += static_cast<char8_t>(0xE0 | (cp >> 12));
			r += static_cast<char8_t>(0x80 | ((cp >> 6) & 0x3F));
			r += static_cast<char8_t>(0x80 | (cp & 0x3F));
		}
		else
		{
			r += static_cast<char8_t>(0xF0 | (cp >> 18));
			r += static_cast<char8_t>(0x80 | ((cp >> 12) & 0x3F));
			r += static_cast<char8_t>(0x80 | ((cp >> 6) & 0x3F));
			r += static_cast<char8_t>(0x80 | (cp & 0x3F));
		}
	}

	unsigned long parse_unicode_escape()
	{
		std::string h;
		for (int i = 0; i < 4 && pos_ < src_.size(); i++) h += static_cast<char>(src_[pos_++]);
		auto cp = std::stoul(h, nullptr, 16);
		if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 1 < src_.size() && src_[pos_] == u8'\\' && src_[pos_ + 1] == u8'u')
		{
			pos_ += 2;
			std::string h2;
			for (int i = 0; i < 4 && pos_ < src_.size(); i++) h2 += static_cast<char>(src_[pos_++]);
			const auto lo = std::stoul(h2, nullptr, 16);
			if (lo >= 0xDC00 && lo <= 0xDFFF)
				cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
		}
		return cp;
	}

	std::u8string parse_str()
	{
		if (next() != '"') throw std::runtime_error("expected '\"'");
		std::u8string r;
		while (pos_ < src_.size())
		{
			auto c = static_cast<char>(src_[pos_++]);
			if (c == '"') return r;
			if (c == '\\' && pos_ < src_.size())
			{
				c = static_cast<char>(src_[pos_++]);
				switch (c)
				{
				case '"':
				case '\\':
				case '/': r += static_cast<char8_t>(c);
					break;
				case 'n': r += u8'\n';
					break;
				case 'r': r += u8'\r';
					break;
				case 't': r += u8'\t';
					break;
				case 'b': r += u8'\b';
					break;
				case 'f': r += u8'\f';
					break;
				case 'u':
					{
						append_unicode_cp(r, parse_unicode_escape());
						break;
					}
				default: r += static_cast<char8_t>(c);
				}
			}
			else r += static_cast<char8_t>(c);
		}
		throw std::runtime_error("unterminated string");
	}

	json_value parse_num()
	{
		skip_ws();
		const size_t s = pos_;
		if (src_[pos_] == u8'-') pos_++;
		while (pos_ < src_.size() && isdigit(static_cast<unsigned char>(src_[pos_]))) pos_++;
		if (pos_ < src_.size() && src_[pos_] == u8'.')
		{
			pos_++;
			while (pos_ < src_.size() && isdigit(static_cast<unsigned char>(src_[pos_]))) pos_++;
		}
		if (pos_ < src_.size() && (src_[pos_] == u8'e' || src_[pos_] == u8'E'))
		{
			pos_++;
			if (pos_ < src_.size() && (src_[pos_] == u8'+' || src_[pos_] == u8'-')) pos_++;
			while (pos_ < src_.size() && isdigit(static_cast<unsigned char>(src_[pos_]))) pos_++;
		}
		double val = 0;
		const auto src_chars = reinterpret_cast<const char*>(src_.data());
		std::from_chars(src_chars + s, src_chars + pos_, val);
		return json_value{val};
	}

	json_value parse_str_value()
	{
		skip_ws();
		if (pos_ >= src_.size() || src_[pos_] != u8'"') throw std::runtime_error("expected '\"'");
		pos_++;
		const size_t start = pos_;
		while (pos_ < src_.size() && src_[pos_] != u8'"' && src_[pos_] != u8'\\') pos_++;
		if (pos_ >= src_.size()) throw std::runtime_error("unterminated string");
		if (src_[pos_] == u8'"')
		{
			auto sv = src_.substr(start, pos_ - start);
			pos_++;
			return json_value{sv};
		}
		std::u8string r(src_.data() + start, pos_ - start);
		while (pos_ < src_.size())
		{
			auto c = static_cast<char>(src_[pos_++]);
			if (c == '"') return json_value{std::move(r)};
			if (c == '\\' && pos_ < src_.size())
			{
				c = static_cast<char>(src_[pos_++]);
				switch (c)
				{
				case '"':
				case '\\':
				case '/': r += static_cast<char8_t>(c);
					break;
				case 'n': r += u8'\n';
					break;
				case 'r': r += u8'\r';
					break;
				case 't': r += u8'\t';
					break;
				case 'b': r += u8'\b';
					break;
				case 'f': r += u8'\f';
					break;
				case 'u':
					{
						append_unicode_cp(r, parse_unicode_escape());
						break;
					}
				default: r += static_cast<char8_t>(c);
				}
			}
			else r += static_cast<char8_t>(c);
		}
		throw std::runtime_error("unterminated string");
	}

	json_value parse_val()
	{
		const char c = peek();
		if (c == '"') return parse_str_value();
		if (c == '{') return parse_obj();
		if (c == '[') return parse_arr();
		if (c == 't')
		{
			pos_ += 4;
			return json_value{true};
		}
		if (c == 'f')
		{
			pos_ += 5;
			return json_value{false};
		}
		if (c == 'n')
		{
			pos_ += 4;
			return json_value{nullptr};
		}
		if (c == '-' || isdigit(static_cast<unsigned char>(c))) return parse_num();
		throw std::runtime_error(std::string("unexpected char: ") + c);
	}

	json_value parse_obj()
	{
		next(); // '{'
		json_object m;
		if (peek() == '}')
		{
			next();
			return json_value{std::move(m)};
		}
		for (;;)
		{
			auto k = parse_str();
			if (next() != ':') throw std::runtime_error("expected ':'");
			m[std::move(k)] = parse_val();
			const char c = next();
			if (c == '}') break;
			if (c != ',') throw std::runtime_error("expected ',' or '}'");
		}
		return json_value{std::move(m)};
	}

	json_value parse_arr()
	{
		next(); // '['
		json_array a;
		if (peek() == ']')
		{
			next();
			return json_value{std::move(a)};
		}
		for (;;)
		{
			a.push_back(parse_val());
			const char c = next();
			if (c == ']') break;
			if (c != ',') throw std::runtime_error("expected ',' or ']'");
		}
		return json_value{std::move(a)};
	}

public:
	json_parser(const std::u8string_view s) : src_(s)
	{
	}

	json_value parse() { return parse_val(); }
};

inline json_value parse_json(const std::u8string_view s) { return json_parser(s).parse(); }
