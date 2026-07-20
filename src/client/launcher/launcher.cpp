#include <std_include.hpp>
#include "launcher.hpp"

#include <utils/nt.hpp>
#include <utils/io.hpp>

launcher::launcher()
{
	this->create_main_menu();
}

void launcher::create_main_menu()
{
	this->main_window_.register_callback("openUrl", [](html_frame::callback_params* params)
	{
		if (params->arguments.empty()) return;

		const auto param = params->arguments[0];
		if (!param.is_string()) return;

		const auto url = param.get_string();
		ShellExecuteA(nullptr, "open", url.data(), nullptr, nullptr, SW_SHOWNORMAL);
	});

	this->main_window_.register_callback("selectMode", [this](html_frame::callback_params* params)
	{
		if (params->arguments.empty()) return;

		const auto param = params->arguments[0];
		if (!param.is_number()) return;

		const auto number = static_cast<mode>(param.get_number());
		this->select_mode(number);
	});

	this->main_window_.set_callback(
		[](window* window, const UINT message, const WPARAM w_param, const LPARAM l_param) -> LRESULT
		{
			if (message == WM_CLOSE)
			{
				window::close_all();
			}

			return DefWindowProcA(*window, message, w_param, l_param);
		});

	// Read language preference from data/language.txt
	std::string lang = "english";
	{
		std::string data;
		if (utils::io::read_file("data/language.txt", &data))
		{
			data.erase(0, data.find_first_not_of(" \t\r\n"));
			data.erase(data.find_last_not_of(" \t\r\n") + 1);
			lang = data;
		}
	}

	std::string html = load_content(MENU_MAIN);

	// Replace English UI strings with Chinese when language.txt = schinese
	if (lang == "schinese" || lang == "chinese" || lang == "zh" || lang == "zh-cn")
	{
		auto replace = [](std::string& s, const std::string& from, const std::string& to)
		{
			size_t pos = 0;
			while ((pos = s.find(from, pos)) != std::string::npos)
			{
				s.replace(pos, from.length(), to);
				pos += to.length();
			}
		};
		// Chinese translations as UTF-8 byte sequences to avoid
		// source file encoding issues (GBK vs UTF-8 mismatch).
		// UTF-8 byte sequences split into separate string literals
		// because C++ \x greedily consumes all following hex digits.
		replace(html, ">Singleplayer<",
			">" "\xE5\x8D\x95" "\xE4\xBA\xBA" "\xE6\xB8\xB8" "\xE6\x88\x8F" "<");  // 单人游戏
		replace(html, ">Multiplayer<",
			">" "\xE5\xA4\x9A" "\xE4\xBA\xBA" "\xE6\xB8\xB8" "\xE6\x88\x8F" "<");  // 多人游戏
		replace(html, ">Zombies<",
			">" "\xE5\x83\xB5" "\xE5\xB0\xB8" "\xE6\xA8\xA1" "\xE5\xBC\x8F" "<");  // 僵尸模式
		replace(html, ">Survival<",
			">" "\xE7\x94\x9F" "\xE5\xAD\x98" "\xE6\xA8\xA1" "\xE5\xBC\x8F" "<");  // 生存模式
		replace(html, ">Server<",
			">" "\xE4\xB8\x93" "\xE7\x94\xA8" "\xE6\x9C\x8D" "\xE5\x8A\xA1" "\xE5\x99\xA8" "<");  // 专用服务器
		// Additional UI text
		replace(html, ">No settings yet!<",
			">" "\xE6\x9A\x82\xE6\x97\xA0\xE8\xAE\xBE\xE7\xBD\xAE" "<");  // 暂无设置
		replace(html, ">Developed by <",
			">" "\xE5\xBC\x80\xE5\x8F\x91\xE8\x80\x85\xEF\xBC\x9A" " <");  // 开发者：
		replace(html, ">Forked and modified by <",
			">" "\xE5\x88\x86\xE5\x8F\x89\xE4\xBF\xAE\xE6\x94\xB9\xE8\x80\x85\xEF\xBC\x9A" " <");  // 分叉修改者：
		replace(html, ">Big thanks to all the past contributors and supporters.<",
			">" "\xE6\x84\x9F\xE8\xB0\xA2\xE6\x89\x80\xE6\x9C\x89\xE8\xBF\x87\xE5\xBE\x80\xE7\x9A\x84\xE8\xB4\xA1\xE7\x8C\xAE\xE8\x80\x85\xE5\x92\x8C\xE6\x94\xAF\xE6\x8C\x81\xE8\x80\x85\xE3\x80\x82" "<");  // 感谢所有过往的贡献者和支持者。
		replace(html, ">S1x/s1-mod<",
			">S1x/s1-mod\xE4\xB8\xAD\xE6\x96\x87\xE7\x89\x88" "<");  // S1x/s1-mod中文版
	}

	this->main_window_.create("s1-mod", 750, 420);

	// Set Unicode title via SetWindowTextW to avoid ANSI code page issues.
	// CreateWindowExA can't handle UTF-8 Chinese regardless of conversion.
	if (lang == "schinese")
	{
		const char* utf8_title = "s1-mod - " "\xE5\x90\xAF" "\xE5\x8A\xA8" "\xE5\x99\xA8";  // 启动器
		int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8_title, -1, nullptr, 0);
		std::wstring wtitle(wlen, L'\0');
		MultiByteToWideChar(CP_UTF8, 0, utf8_title, -1, wtitle.data(), wlen);
		SetWindowTextW(this->main_window_, wtitle.c_str());
	}

	this->main_window_.load_html(html);
	this->main_window_.show();
}

launcher::mode launcher::run() const
{
	window::run();
	return this->mode_;
}

void launcher::select_mode(const mode mode)
{
	this->mode_ = mode;
	this->main_window_.close();
}

std::string launcher::load_content(const int res)
{
	return utils::nt::load_resource(res);
}
