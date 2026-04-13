// html_to_md.h — 5-pass HTML-to-markdown converter for SEC EDGAR filings

#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <cctype>
#include <iostream>

// ============================================================================
// html_to_md - converts SEC EDGAR inline XBRL HTML filings to markdown
//
// SEC filings (10-K, 10-Q) from EDGAR are HTML with:
//   - large hidden ix:header blocks (XBRL metadata, display:none)
//   - ix:nonNumeric / ix:nonFraction tags wrapping data values
//   - heavy inline style= attributes on every element
//   - tables for both layout and financial data
//   - boilerplate: SEC cover page, exhibit index, signature block
//
// conversion strategy (5 passes):
//   1. strip_xbrl_and_head    - remove XML decl, comments, <head>, hidden divs
//   2. walk tags              - structural HTML -> markdown conversion
//   3. decode_entities        - &#160; -> space, &#8212; -> em-dash, etc.
//   4. remove_boilerplate     - SEC cover page, exhibits, signature, footers
//   5. collapse_whitespace    - normalize blank lines and trailing spaces
// ============================================================================

namespace html_to_md_detail
{
	// ----------------------------------------------------------------------------
	// conversion log - collects diagnostics printed to stderr
	// ----------------------------------------------------------------------------

	struct conversion_log
	{
		size_t input_size = 0;
		size_t after_strip = 0;
		size_t after_walk = 0;
		size_t after_entities = 0;
		size_t after_headings = 0;
		size_t after_boilerplate = 0;
		size_t after_collapse = 0;

		int hidden_divs_stripped = 0;
		int tables_converted = 0;
		int tables_skipped_empty = 0;
		int headings_detected = 0;
		int cover_page_lines_stripped = 0;
		int footer_lines_stripped = 0;

		void print(std::ostream& os) const
		{
			os << "  conversion log:\n";
			os << "    input:            " << input_size << " bytes\n";
			os << "    after strip xbrl: " << after_strip << " bytes";
			if (hidden_divs_stripped > 0) os << " (" << hidden_divs_stripped << " hidden divs removed)";
			os << "\n";
			os << "    after walk tags:  " << after_walk << " bytes";
			os << " (" << tables_converted << " tables";
			if (tables_skipped_empty > 0) os << ", " << tables_skipped_empty << " empty tables skipped";
			os << ")\n";
			os << "    after entities:   " << after_entities << " bytes\n";
			os << "    after headings:   " << after_headings << " bytes (" << headings_detected << " headings)\n";
			os << "    after boilerplate:" << after_boilerplate << " bytes";
			if (cover_page_lines_stripped > 0) os << " (" << cover_page_lines_stripped << " cover lines";
			if (footer_lines_stripped > 0) os << ", " << footer_lines_stripped << " footer lines";
			if (cover_page_lines_stripped > 0 || footer_lines_stripped > 0) os << " removed)";
			os << "\n";
			os << "    final output:     " << after_collapse << " bytes";
			if (input_size > 0) os << " (" << (100 - after_collapse * 100 / input_size) << "% reduction)";
			os << "\n";
		}
	};

	// ----------------------------------------------------------------------------
	// entity decoding
	// ----------------------------------------------------------------------------

	inline void decode_entities(std::u8string& s)
	{
		std::u8string out;
		out.reserve(s.size());
		size_t i = 0;
		while (i < s.size())
		{
			if (s[i] == u8'&')
			{
				auto semi = s.find(u8';', i);
				if (semi != std::u8string::npos && semi - i < 12)
				{
					auto entity = std::u8string_view(s).substr(i, semi - i + 1);
					if (entity == u8"&amp;")
					{
						out += u8'&';
						i = semi + 1;
						continue;
					}
					if (entity == u8"&lt;")
					{
						out += u8'<';
						i = semi + 1;
						continue;
					}
					if (entity == u8"&gt;")
					{
						out += u8'>';
						i = semi + 1;
						continue;
					}
					if (entity == u8"&quot;")
					{
						out += u8'"';
						i = semi + 1;
						continue;
					}
					if (entity == u8"&apos;")
					{
						out += u8'\'';
						i = semi + 1;
						continue;
					}
					if (entity == u8"&nbsp;")
					{
						out += u8' ';
						i = semi + 1;
						continue;
					}
					if (entity == u8"&mdash;")
					{
						out += u8"\xe2\x80\x94";
						i = semi + 1;
						continue;
					} // —
					if (entity == u8"&ndash;")
					{
						out += u8"\xe2\x80\x93";
						i = semi + 1;
						continue;
					} // –
					if (entity == u8"&bull;")
					{
						out += u8"\xe2\x80\xa2";
						i = semi + 1;
						continue;
					} // •
					if (entity == u8"&trade;")
					{
						out += u8"\xe2\x84\xa2";
						i = semi + 1;
						continue;
					} // ™
					if (entity == u8"&reg;")
					{
						out += u8"\xc2\xae";
						i = semi + 1;
						continue;
					} // ®
					if (entity == u8"&copy;")
					{
						out += u8"\xc2\xa9";
						i = semi + 1;
						continue;
					} // ©
					if (entity == u8"&sect;")
					{
						out += u8"\xc2\xa7";
						i = semi + 1;
						continue;
					} // §
					if (entity == u8"&para;")
					{
						out += u8"\xc2\xb6";
						i = semi + 1;
						continue;
					} // ¶
					if (entity == u8"&deg;")
					{
						out += u8"\xc2\xb0";
						i = semi + 1;
						continue;
					} // °
					if (entity == u8"&lsquo;")
					{
						out += u8"\xe2\x80\x98";
						i = semi + 1;
						continue;
					} // '
					if (entity == u8"&rsquo;")
					{
						out += u8"\xe2\x80\x99";
						i = semi + 1;
						continue;
					} // '
					if (entity == u8"&ldquo;")
					{
						out += u8"\xe2\x80\x9c";
						i = semi + 1;
						continue;
					} // "
					if (entity == u8"&rdquo;")
					{
						out += u8"\xe2\x80\x9d";
						i = semi + 1;
						continue;
					} // "
					if (entity == u8"&hellip;")
					{
						out += u8"\xe2\x80\xa6";
						i = semi + 1;
						continue;
					} // …
					if (entity == u8"&eacute;")
					{
						out += u8"\xc3\xa9";
						i = semi + 1;
						continue;
					} // é
					if (entity == u8"&agrave;")
					{
						out += u8"\xc3\xa0";
						i = semi + 1;
						continue;
					} // à
					if (entity == u8"&uuml;")
					{
						out += u8"\xc3\xbc";
						i = semi + 1;
						continue;
					} // ü
					if (entity == u8"&ouml;")
					{
						out += u8"\xc3\xb6";
						i = semi + 1;
						continue;
					} // ö
					if (entity == u8"&times;")
					{
						out += u8"\xc3\x97";
						i = semi + 1;
						continue;
					} // ×
					if (entity == u8"&minus;")
					{
						out += u8"\xe2\x88\x92";
						i = semi + 1;
						continue;
					} // −
					if (entity == u8"&frac12;")
					{
						out += u8"\xc2\xbd";
						i = semi + 1;
						continue;
					} // ½
					if (entity == u8"&frac14;")
					{
						out += u8"\xc2\xbc";
						i = semi + 1;
						continue;
					} // ¼

					// numeric entities: &#160; &#8212; &#9746; etc.
					if (entity.size() >= 4 && entity[1] == u8'#')
					{
						unsigned cp = 0;
						bool valid = true;
						if (entity[2] == u8'x' || entity[2] == u8'X')
						{
							for (size_t j = 3; j < entity.size() - 1; j++)
							{
								auto c = entity[j];
								if (c >= u8'0' && c <= u8'9') cp = cp * 16 + (c - u8'0');
								else if (c >= u8'a' && c <= u8'f') cp = cp * 16 + (c - u8'a' + 10);
								else if (c >= u8'A' && c <= u8'F') cp = cp * 16 + (c - u8'A' + 10);
								else
								{
									valid = false;
									break;
								}
							}
						}
						else
						{
							for (size_t j = 2; j < entity.size() - 1; j++)
							{
								if (entity[j] >= u8'0' && entity[j] <= u8'9') cp = cp * 10 + (entity[j] - u8'0');
								else
								{
									valid = false;
									break;
								}
							}
						}
						if (valid && cp > 0)
						{
							if (cp == 160) { out += u8' '; } // &nbsp;
							else if (cp < 0x80) { out += static_cast<char8_t>(cp); }
							else if (cp < 0x800)
							{
								out += static_cast<char8_t>(0xC0 | (cp >> 6));
								out += static_cast<char8_t>(0x80 | (cp & 0x3F));
							}
							else if (cp < 0x10000)
							{
								out += static_cast<char8_t>(0xE0 | (cp >> 12));
								out += static_cast<char8_t>(0x80 | ((cp >> 6) & 0x3F));
								out += static_cast<char8_t>(0x80 | (cp & 0x3F));
							}
							else
							{
								out += static_cast<char8_t>(0xF0 | (cp >> 18));
								out += static_cast<char8_t>(0x80 | ((cp >> 12) & 0x3F));
								out += static_cast<char8_t>(0x80 | ((cp >> 6) & 0x3F));
								out += static_cast<char8_t>(0x80 | (cp & 0x3F));
							}
							i = semi + 1;
							continue;
						}
					}
				}
			}
			out += s[i++];
		}
		s = std::move(out);
	}

	// ----------------------------------------------------------------------------
	// simple tag parser - extracts tag name, checks self-closing, gets attributes
	// ----------------------------------------------------------------------------

	struct parsed_tag
	{
		std::u8string name; // lowercase, e.g. "div", "ix:nonfraction", "/div"
		bool is_closing = false; // </div>
		bool is_self_closing = false; // <br/> or <hr/>
		std::u8string_view raw; // full tag text including < >

		// extract a specific attribute value (unquoted)
		std::u8string attr(const std::u8string_view attr_name) const
		{
			// search in raw for attr_name="value" or attr_name='value'
			std::u8string needle(attr_name);
			needle += u8'=';
			auto pos = raw.find(needle);
			while (pos != std::u8string_view::npos)
			{
				// make sure it's preceded by whitespace (not part of another attr name)
				if (pos > 0 && raw[pos - 1] != u8' ' && raw[pos - 1] != u8'\t')
				{
					pos = raw.find(needle, pos + needle.size());
					continue;
				}
				pos += needle.size();
				if (pos < raw.size() && (raw[pos] == u8'"' || raw[pos] == u8'\''))
				{
					const auto q = raw[pos++];
					const auto end = raw.find(q, pos);
					if (end != std::u8string_view::npos)
						return std::u8string(raw.substr(pos, end - pos));
				}
				break;
			}
			return {};
		}
	};

	inline parsed_tag parse_tag(const std::u8string_view tag_text)
	{
		parsed_tag t;
		t.raw = tag_text;
		// skip < and find tag name
		size_t i = 1; // skip '<'
		if (i < tag_text.size() && tag_text[i] == u8'/')
		{
			t.is_closing = true;
			i++;
		}
		const size_t name_start = i;
		while (i < tag_text.size() && tag_text[i] != u8' ' && tag_text[i] != u8'>' && tag_text[i] != u8'/' && tag_text[
			i] != u8'\t')
			i++;
		t.name = std::u8string(tag_text.substr(name_start, i - name_start));
		std::transform(t.name.begin(), t.name.end(), t.name.begin(), [](const char8_t c) -> char8_t
		{
			return static_cast<char8_t>(std::tolower(static_cast<unsigned char>(c)));
		});

		if (tag_text.size() >= 2 && tag_text[tag_text.size() - 2] == u8'/')
			t.is_self_closing = true;

		return t;
	}

	// ----------------------------------------------------------------------------
	// pass 1: strip XBRL metadata, <head>, XML decl, comments
	// ----------------------------------------------------------------------------

	inline std::u8string strip_xbrl_and_head(const std::u8string_view html, conversion_log& log)
	{
		std::u8string out;
		out.reserve(html.size());
		size_t i = 0;

		while (i < html.size())
		{
			// skip XML declaration <?xml ... ?>
			if (i + 4 < html.size() && html[i] == u8'<' && html[i + 1] == u8'?')
			{
				const auto end = html.find(u8"?>", i);
				if (end != std::u8string::npos)
				{
					i = end + 2;
					continue;
				}
			}

			// skip comments <!-- ... -->
			if (i + 3 < html.size() && html[i] == u8'<' && html[i + 1] == u8'!' && html[i + 2] == u8'-' && html[i + 3]
				== u8'-')
			{
				const auto end = html.find(u8"-->", i);
				if (end != std::u8string::npos)
				{
					i = end + 3;
					continue;
				}
			}

			// skip <head>...</head>
			if (i + 5 < html.size() && html[i] == u8'<')
			{
				const auto rest = std::u8string_view(html).substr(i, 6);
				if (rest == u8"<head>" || rest == u8"<HEAD>")
				{
					auto end = html.find(u8"</head>", i);
					if (end == std::u8string::npos) end = html.find(u8"</HEAD>", i);
					if (end != std::u8string::npos)
					{
						i = end + 7;
						continue;
					}
				}
			}

			// skip hidden divs containing ix:header (display:none XBRL metadata)
			// pattern: <div style="display:none"><ix:header>...</ix:header></div>
			if (i + 3 < html.size() && html[i] == u8'<')
			{
				const auto tag_end = html.find(u8'>', i);
				if (tag_end != std::u8string::npos)
				{
					const auto tag_text = std::u8string_view(html).substr(i, tag_end - i + 1);
					auto t = parse_tag(tag_text);
					if (t.name == u8"div")
					{
						auto style = t.attr(u8"style");
						if (style.find(u8"display:none") != std::u8string::npos ||
							style.find(u8"display: none") != std::u8string::npos)
						{
							// skip to matching </div> - but we need to handle nested divs
							int depth = 1;
							size_t j = tag_end + 1;
							while (j < html.size() && depth > 0)
							{
								if (html[j] == u8'<')
								{
									const auto c1 = j + 1 < html.size()
										                ? std::tolower(static_cast<unsigned char>(html[j + 1]))
										                : 0;
									if (c1 == '/' && j + 5 < html.size())
									{
										const auto c2 = std::tolower(static_cast<unsigned char>(html[j + 2]));
										const auto c3 = std::tolower(static_cast<unsigned char>(html[j + 3]));
										const auto c4 = std::tolower(static_cast<unsigned char>(html[j + 4]));
										if (c2 == 'd' && c3 == 'i' && c4 == 'v')
											depth--;
									}
									else if (j + 4 < html.size())
									{
										const auto c2 = std::tolower(static_cast<unsigned char>(html[j + 2]));
										const auto c3 = std::tolower(static_cast<unsigned char>(html[j + 3]));
										if (c1 == 'd' && c2 == 'i' && c3 == 'v' && (html[j + 4] == u8' ' || html[j + 4]
											== u8'>'))
											depth++;
									}
								}
								if (depth > 0) j++;
							}
							const auto close = html.find(u8'>', j);
							if (close != std::u8string::npos)
							{
								log.hidden_divs_stripped++;
								i = close + 1;
								continue;
							}
						}
					}
				}
			}

			out += html[i++];
		}
		return out;
	}

	// ----------------------------------------------------------------------------
	// pass 2: walk HTML tags and convert to markdown
	// ----------------------------------------------------------------------------

	// table state for building markdown tables
	struct table_builder
	{
		std::vector<std::vector<std::u8string>> rows;
		std::vector<std::u8string> current_row;
		std::u8string current_cell;
		bool in_table = false;
		bool in_row = false;
		bool in_cell = false;

		void start_table()
		{
			rows.clear();
			current_row.clear();
			current_cell.clear();
			in_table = true;
		}

		void start_row()
		{
			current_row.clear();
			current_cell.clear();
			in_row = true;
		}

		void start_cell()
		{
			current_cell.clear();
			in_cell = true;
		}

		void end_cell()
		{
			if (in_cell)
			{
				// trim whitespace
				auto s = current_cell;
				while (!s.empty() && (s.front() == u8' ' || s.front() == u8'\t')) s.erase(s.begin());
				while (!s.empty() && (s.back() == u8' ' || s.back() == u8'\t')) s.pop_back();
				current_row.push_back(std::move(s));
				current_cell.clear();
				in_cell = false;
			}
		}

		void end_row()
		{
			end_cell();
			if (in_row && !current_row.empty())
			{
				// check if row is entirely empty/whitespace
				bool all_empty = true;
				for (auto& c : current_row)
					if (!c.empty())
					{
						all_empty = false;
						break;
					}
				if (!all_empty)
					rows.push_back(std::move(current_row));
			}
			current_row.clear();
			in_row = false;
		}

		std::u8string end_table(conversion_log& log)
		{
			end_row();
			in_table = false;
			if (rows.empty())
			{
				log.tables_skipped_empty++;
				return {};
			}

			// determine max columns
			size_t max_cols = 0;
			for (auto& r : rows) max_cols = (std::max)(max_cols, r.size());
			if (max_cols == 0) return {};

			// skip purely decorative tables (all cells empty, or single-column single-row)
			bool has_content = false;
			for (auto& r : rows)
				for (auto& c : r)
					if (!c.empty())
					{
						has_content = true;
						break;
					}
			if (!has_content)
			{
				log.tables_skipped_empty++;
				return {};
			}

			// pad rows to max_cols
			for (auto& r : rows) r.resize(max_cols);

			// compute column widths
			std::vector<size_t> widths(max_cols, 3);
			for (auto& r : rows)
				for (size_t c = 0; c < max_cols; c++)
					widths[c] = (std::max)(widths[c], r[c].size());

			// emit markdown table
			std::u8string md;
			// header row
			md += u8'|';
			for (size_t c = 0; c < max_cols; c++)
			{
				md += u8' ';
				md += rows[0][c];
				md += std::u8string(widths[c] - rows[0][c].size(), u8' ');
				md += u8" |";
			}
			md += u8'\n';
			// separator
			md += u8'|';
			for (size_t c = 0; c < max_cols; c++)
			{
				md += u8' ';
				md += std::u8string(widths[c], u8'-');
				md += u8" |";
			}
			md += u8'\n';
			// data rows
			for (size_t r = 1; r < rows.size(); r++)
			{
				md += u8'|';
				for (size_t c = 0; c < max_cols; c++)
				{
					md += u8' ';
					md += rows[r][c];
					md += std::u8string(widths[c] - rows[r][c].size(), u8' ');
					md += u8" |";
				}
				md += u8'\n';
			}
			log.tables_converted++;
			rows.clear();
			return md;
		}
	};

	inline bool style_has(const std::u8string_view style, const std::u8string_view prop)
	{
		return style.find(prop) != std::u8string_view::npos;
	}

	inline std::u8string walk_and_convert(const std::u8string_view html, conversion_log& log)
	{
		std::u8string out;
		out.reserve(html.size() / 3);
		size_t i = 0;
		table_builder tbl;
		int skip_depth = 0; // for skipping nested content inside certain tags

		while (i < html.size())
		{
			// handle tags
			if (html[i] == u8'<')
			{
				auto tag_end = html.find(u8'>', i);
				if (tag_end == std::u8string::npos)
				{
					i++;
					continue;
				}

				auto tag_text = std::u8string_view(html).substr(i, tag_end - i + 1);
				auto tag = parse_tag(tag_text);
				i = tag_end + 1;

				// strip ix:* tags entirely (keep content inside them)
				if (tag.name.starts_with(u8"ix:")) continue;
				// strip <html>, <body> wrappers
				if (tag.name == u8"html" || tag.name == u8"body") continue;
				// strip <span> tags (keep content)
				if (tag.name == u8"span") continue;
				// strip <a> tags (internal links only, keep text)
				if (tag.name == u8"a") continue;
				// strip <img> tags
				if (tag.name == u8"img") continue;

				// <br/> -> newline
				if (tag.name == u8"br")
				{
					if (tbl.in_cell) tbl.current_cell += u8' ';
					else out += u8'\n';
					continue;
				}

				// <hr> -> markdown hr
				if (tag.name == u8"hr" && !tbl.in_table)
				{
					out += u8"\n---\n";
					continue;
				}

				// --- tables ---
				if (tag.name == u8"table" && !tag.is_closing)
				{
					tbl.start_table();
					continue;
				}
				if (tag.name == u8"table" && tag.is_closing)
				{
					auto table_md = tbl.end_table(log);
					if (!table_md.empty())
					{
						out += u8'\n';
						out += table_md;
						out += u8'\n';
					}
					continue;
				}
				if (tag.name == u8"tr" && !tag.is_closing)
				{
					tbl.start_row();
					continue;
				}
				if (tag.name == u8"tr" && tag.is_closing)
				{
					tbl.end_row();
					continue;
				}
				if ((tag.name == u8"td" || tag.name == u8"th") && !tag.is_closing)
				{
					tbl.start_cell();
					continue;
				}
				if ((tag.name == u8"td" || tag.name == u8"th") && tag.is_closing)
				{
					tbl.end_cell();
					continue;
				}

				// --- divs -> potential headings or paragraphs ---
				if (tag.name == u8"div" && !tag.is_closing)
				{
					if (!tbl.in_table) out += u8'\n';
					continue;
				}
				if (tag.name == u8"div" && tag.is_closing)
				{
				}

				// skip all other tags
				continue;
			}

			// text content
			auto c = html[i++];
			if (tbl.in_cell)
			{
				tbl.current_cell += c;
			}
			else
			{
				out += c;
			}
		}

		return out;
	}

	// ----------------------------------------------------------------------------
	// pass 3: detect headings from text patterns
	// ----------------------------------------------------------------------------

	inline bool is_heading_line(const std::u8string_view line)
	{
		// lines that look like section headings in SEC filings:
		// "PART I", "PART II", "Item 1.", "Item 1A.", "CONDENSED CONSOLIDATED...",
		// "NOTES TO CONDENSED...", etc.
		auto trimmed = line;
		while (!trimmed.empty() && trimmed.front() == u8' ') trimmed.remove_prefix(1);
		while (!trimmed.empty() && trimmed.back() == u8' ') trimmed.remove_suffix(1);
		if (trimmed.empty()) return false;
		// skip table rows - they start with '|'
		if (trimmed.front() == u8'|') return false;

		// PART headings
		if (trimmed.starts_with(u8"PART ")) return true;
		// Item headings
		if (trimmed.starts_with(u8"Item ") || trimmed.starts_with(u8"ITEM ")) return true;
		// Financial statement titles (all-caps multi-word lines)
		if (trimmed.size() > 15)
		{
			bool mostly_upper = true;
			int upper_count = 0, letter_count = 0;
			for (const auto ch : trimmed)
			{
				if (std::isalpha(static_cast<unsigned char>(ch)))
				{
					letter_count++;
					if (std::isupper(static_cast<unsigned char>(ch))) upper_count++;
				}
			}
			if (letter_count > 10 && upper_count * 100 / letter_count > 70) return true;
		}
		return false;
	}

	inline std::u8string add_heading_markers(const std::u8string_view text, conversion_log& log)
	{
		std::u8string out;
		out.reserve(text.size());
		size_t i = 0;

		while (i < text.size())
		{
			// find next line
			auto nl = text.find(u8'\n', i);
			if (nl == std::u8string::npos) nl = text.size();
			const auto line = std::u8string_view(text).substr(i, nl - i);

			auto trimmed = line;
			while (!trimmed.empty() && trimmed.front() == u8' ') trimmed.remove_prefix(1);
			while (!trimmed.empty() && trimmed.back() == u8' ') trimmed.remove_suffix(1);

			if (!trimmed.empty() && is_heading_line(trimmed))
			{
				log.headings_detected++;
				if (trimmed.starts_with(u8"PART "))
					out += u8"## ";
				else if (trimmed.starts_with(u8"Item ") || trimmed.starts_with(u8"ITEM "))
					out += u8"### ";
				else
					out += u8"#### ";
				out += trimmed;
			}
			else
			{
				out += line;
			}
			if (nl < text.size()) out += u8'\n';
			i = nl + 1;
			if (nl == text.size()) break;
		}
		return out;
	}

	// ----------------------------------------------------------------------------
	// pass 4: remove boilerplate
	// ----------------------------------------------------------------------------

	inline std::u8string remove_boilerplate(const std::u8string_view text, conversion_log& log)
	{
		std::u8string out;
		out.reserve(text.size());

		// split into lines for pattern matching
		std::vector<std::u8string_view> lines;
		size_t i = 0;
		while (i < text.size())
		{
			auto nl = text.find(u8'\n', i);
			if (nl == std::u8string::npos) nl = text.size();
			lines.push_back(std::u8string_view(text).substr(i, nl - i));
			i = nl + 1;
			if (nl == text.size()) break;
		}

		bool in_cover_page = false;
		bool past_cover = false;

		for (size_t li = 0; li < lines.size(); li++)
		{
			const auto line = lines[li];
			auto trimmed = line;
			while (!trimmed.empty() && trimmed.front() == u8' ') trimmed.remove_prefix(1);
			while (!trimmed.empty() && trimmed.back() == u8' ') trimmed.remove_suffix(1);

			// detect start of SEC cover page
			if (!past_cover && trimmed.find(u8"UNITED STATES") != std::u8string_view::npos &&
				li + 1 < lines.size())
			{
				auto next = lines[li + 1];
				while (!next.empty() && next.front() == u8' ') next.remove_prefix(1);
				if (next.find(u8"SECURITIES AND EXCHANGE COMMISSION") != std::u8string_view::npos)
				{
					in_cover_page = true;
					log.cover_page_lines_stripped++;
					continue;
				}
			}

			// detect end of cover page (first PART heading or TABLE OF CONTENTS)
			if (in_cover_page)
			{
				if (trimmed.starts_with(u8"## PART ") ||
					trimmed.find(u8"TABLE OF CONTENTS") != std::u8string_view::npos ||
					trimmed.find(u8"Table of Contents") != std::u8string_view::npos)
				{
					in_cover_page = false;
					past_cover = true;
					// keep this line (it's the start of real content)
				}
				else
				{
					log.cover_page_lines_stripped++;
					continue; // skip cover page content
				}
			}

			// skip page footer lines: "Company Name | Q3 2025 Form 10-Q | 23"
			if (trimmed.find(u8"Form 10-") != std::u8string_view::npos && trimmed.find(u8'|') !=
				std::u8string_view::npos)
			{
				// looks like a page footer
				int pipe_count = 0;
				for (const auto c : trimmed) if (c == u8'|') pipe_count++;
				if (pipe_count >= 1 && trimmed.size() < 80)
				{
					log.footer_lines_stripped++;
					continue;
				}
			}

			out += line;
			out += u8'\n';
		}
		return out;
	}

	// ----------------------------------------------------------------------------
	// pass 5: collapse whitespace
	// ----------------------------------------------------------------------------

	inline std::u8string collapse_whitespace(const std::u8string_view text)
	{
		std::u8string out;
		out.reserve(text.size());
		int blank_count = 0;

		size_t i = 0;
		while (i < text.size())
		{
			auto nl = text.find(u8'\n', i);
			if (nl == std::u8string::npos) nl = text.size();
			auto line = std::u8string_view(text).substr(i, nl - i);

			// trim trailing whitespace from each line
			while (!line.empty() && (line.back() == u8' ' || line.back() == u8'\t'))
				line.remove_suffix(1);

			// also trim leading whitespace
			while (!line.empty() && (line.front() == u8' ' || line.front() == u8'\t'))
				line.remove_prefix(1);

			if (line.empty())
			{
				blank_count++;
				if (blank_count <= 2) out += u8'\n';
			}
			else
			{
				blank_count = 0;
				out += line;
				out += u8'\n';
			}

			i = nl + 1;
			if (nl == text.size()) break;
		}

		// trim leading/trailing blank lines
		while (out.size() > 1 && out[0] == u8'\n') out.erase(out.begin());
		while (out.size() > 1 && out.back() == u8'\n' && out[out.size() - 2] == u8'\n') out.pop_back();

		return out;
	}
} // namespace html_to_md_detail

// ============================================================================
// public API
// ============================================================================

inline std::u8string html_to_md(const std::u8string_view html)
{
	using namespace html_to_md_detail;
	conversion_log log;
	log.input_size = html.size();

	auto s = strip_xbrl_and_head(html, log);
	log.after_strip = s.size();

	s = walk_and_convert(s, log);
	log.after_walk = s.size();

	decode_entities(s);
	log.after_entities = s.size();

	s = add_heading_markers(s, log);
	log.after_headings = s.size();

	s = remove_boilerplate(s, log);
	log.after_boilerplate = s.size();

	s = collapse_whitespace(s);
	log.after_collapse = s.size();

	log.print(std::cerr);
	return s;
}
