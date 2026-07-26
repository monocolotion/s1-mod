#pragma once
#include <utils/info_string.hpp>

namespace lan_server_list
{
	void refresh();
	void handle_info_response(const game::netadr_s& address, const utils::info_string& info);

	bool is_menu_open();
	int ui_feeder_count();
	const char* ui_feeder_item_text(int index, int column);
	void join_server(int index);
	bool sl_key_event(int key, int down);
}
