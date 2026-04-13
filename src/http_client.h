// http_client.h — HTTP request/response types and WinINet connection interface

#pragma once

#include <memory>
#include <string>
#include <vector>

// network
bool is_online();

using web_params = std::vector<std::pair<std::u8string, std::u8string>>;

enum class web_request_verb
{
	POST,
	GET
};

struct web_request
{
	std::u8string command;
	std::u8string path;
	std::u8string body;

	web_params query;
	web_params headers;
	web_params form_data;

	std::u8string file_form_data_name;
	std::u8string file_name;
	std::u8string upload_file_path;

	std::u8string download_file_path;

	web_request_verb verb = web_request_verb::GET;
};

struct web_response
{
	std::u8string headers;
	std::u8string body;
	std::u8string content_type;
	int status_code = 0;
};

struct web_host;
using web_host_ptr = std::shared_ptr<web_host>;

web_host_ptr connect_to_host(std::u8string_view host, bool secure = true, int port = 0,
                             std::u8string_view user_agent = {}, int timeout_ms = 0);
web_response send_request(const web_host_ptr& host, const web_request& req);

// URL parsing
struct parsed_url
{
	std::u8string host;
	std::u8string path;
	bool secure = true;
	int port = 0;
};

parsed_url parse_url(std::u8string_view url);
