#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include "game/game.hpp"
#include "game/dvars.hpp"

#include "lan_server_list.hpp"
#include "network.hpp"
#include "scheduler.hpp"
#include "party.hpp"
#include "console.hpp"

#include <utils/string.hpp>

namespace lan_server_list
{
	namespace
	{
		// Process ID for self-detection. LAN servers include their PID
		// as "lan_sid" in the response. If it matches ours, skip.
		std::string my_pid;

		const int server_limit = 14;

		struct server_info
		{
			int clients;
			int max_clients;
			int bots;
			int ping;
			int last_seen;
			std::string host_name;
			std::string map_name;
			std::string game_type;
			std::string mapname_raw;
			std::string gametype_raw;
			game::CodPlayMode play_mode;
			char in_game;
			game::netadr_s address;
		};

		std::mutex mutex;
		std::vector<server_info> servers;

		size_t server_list_page = 0;
		volatile bool update_server_list = false;
		std::chrono::high_resolution_clock::time_point last_scroll{};

		size_t get_page_count()
		{
			const auto count = servers.size() / server_limit;
			return count + (servers.size() % server_limit > 0);
		}

		size_t get_page_base_index()
		{
			return server_list_page * server_limit;
		}

		void trigger_refresh()
		{
			update_server_list = true;
		}

		void sort_serverlist()
		{
			std::ranges::stable_sort(servers, [](const server_info& a, const server_info& b)
			{
				if (a.clients == b.clients)
				{
					return a.ping < b.ping;
				}

				return a.clients > b.clients;
			});
		}

		void insert_server(server_info&& server)
		{
			std::lock_guard<std::mutex> _(mutex);

			// Check for duplicate
			for (auto& existing : servers)
			{
				if (existing.address == server.address)
				{
					existing = std::move(server);
					sort_serverlist();
					trigger_refresh();
					return;
				}
			}

			servers.emplace_back(std::move(server));
			sort_serverlist();
			trigger_refresh();
		}

		void resize_host_name(std::string& name)
		{
			name = utils::string::split(name, '\n').front();

			game::Font_s* font;
			if (game::Com_GetCurrentCoDPlayMode() == game::CODPLAYMODE_ZOMBIES)
			{
				font = game::R_RegisterFont("fonts/zmBodyFont");
			}
			else
			{
				font = game::R_RegisterFont("fonts/bodyFont");
			}
			auto text_size = game::UI_TextWidth(name.data(), 32, font, 1.0f);

			while (text_size > 450)
			{
				text_size = game::UI_TextWidth(name.data(), 32, font, 1.0f);
				name.pop_back();
			}
		}

		int broadcast_timestamp = 0;

		void do_broadcast()
		{
			// Broadcast "getLANInfo" to discover LAN servers on net_port
			const auto* net_port = game::Dvar_FindVar("net_port");
			const auto port = net_port ? static_cast<uint16_t>(net_port->current.integer) : static_cast<uint16_t>(27016);

			game::netadr_s broadcast_addr{};
			broadcast_addr.type = game::NA_IP;
			broadcast_addr.localNetID = game::NS_CLIENT1;
			broadcast_addr.ip[0] = 255;
			broadcast_addr.ip[1] = 255;
			broadcast_addr.ip[2] = 255;
			broadcast_addr.ip[3] = 255;
			broadcast_addr.port = htons(port);

			broadcast_timestamp = game::Sys_Milliseconds();
			network::send(broadcast_addr, "getLANInfo", std::to_string(broadcast_timestamp));
		}

		// Removes servers that haven't responded recently (stale > 10 seconds)
		void clean_stale_servers()
		{
			std::lock_guard<std::mutex> _(mutex);

			const auto now = game::Sys_Milliseconds();
			std::erase_if(servers, [now](const server_info& s)
			{
				return now - s.last_seen > 10'000;
			});
		}

		int last_broadcast = 0;
		const int broadcast_interval = 3000; // Re-broadcast every 3 seconds

		void do_frame_work()
		{
			if (!is_menu_open())
			{
				return;
			}

			const auto now = game::Sys_Milliseconds();
			if (now - last_broadcast > broadcast_interval)
			{
				last_broadcast = now;
				do_broadcast();
				clean_stale_servers();
			}
		}
	}

	bool is_menu_open()
	{
		return game::Menu_IsMenuOpenAndVisible(0, "menu_systemlink_join");
	}

	void refresh()
	{
		{
			std::lock_guard<std::mutex> _(mutex);
			servers.clear();
			server_list_page = 0;
		}

		party::reset_connect_state();
		last_broadcast = 0;
		do_broadcast();
	}

	void join_server(int index)
	{
		std::lock_guard<std::mutex> _(mutex);

		const auto i = static_cast<size_t>(index) + get_page_base_index();
		if (i < servers.size())
		{
				printf("Connecting to LAN server: %s\n", servers[i].host_name.data());
				party::connect_lan(servers[i].address, servers[i].mapname_raw, servers[i].gametype_raw);
		}
	}

	void handle_info_response(const game::netadr_s& address, const utils::info_string& info)
	{
		// Don't show servers that aren't running!
		const auto sv_running = info.get("sv_running");
		if (sv_running != "1"s)
		{
			return;
		}

		// Only handle servers of the same playmode
		const auto playmode_str = info.get("playmode");
		if (playmode_str.empty())
		{
			return; // reject servers without a playmode field
		}
		const auto playmode = static_cast<game::CodPlayMode>(std::atoi(playmode_str.data()));
		if (playmode == game::CODPLAYMODE_NONE)
		{
			return; // reject uninitialized playmode
		}
		if (game::Com_GetCurrentCoDPlayMode() != playmode)
		{
			return;
		}

		// For LAN: accept both dedicated and listen servers
		// (no dedicated==1 filter, unlike the internet server list)

		const auto now = game::Sys_Milliseconds();
		int ping = 0;

		// Calculate ping using the echoed client timestamp (clock-skew-free round-trip)
		const auto challenge = info.get("challenge");
		if (!challenge.empty())
		{
			const auto sent_time = std::atoi(challenge.data());
			ping = std::min(now - sent_time, 999);
		}

		server_info server{};
		server.address = address;
		server.host_name = info.get("hostname");
			server.gametype_raw = info.get("gametype");
			server.mapname_raw = info.get("mapname");
		server.map_name = game::UI_GetMapDisplayName(info.get("mapname").data());
		server.game_type = game::UI_GetGameTypeDisplayName(info.get("gametype").data());
		server.play_mode = playmode;
		server.clients = std::atoi(info.get("clients").data());
		server.max_clients = std::atoi(info.get("sv_maxclients").data());
		server.bots = std::atoi(info.get("bots").data());
		server.ping = ping;
		server.last_seen = now;
		server.in_game = 1;

		resize_host_name(server.host_name);


		// Filter out our own listen server (same machine).
		if (address.type == game::NA_LOOPBACK)
		{
			return;
		}

		// Self-detection: LAN servers include their process ID as "lan_sid".
		// If it matches our PID, the response is from our own listen server.
		const auto lan_sid = info.get("lan_sid");
		if (!lan_sid.empty() && lan_sid == my_pid)
		{
			return;
		}

		insert_server(std::move(server));
	}

	int ui_feeder_count()
	{
		std::lock_guard<std::mutex> _(mutex);

		const auto count = static_cast<int>(servers.size());
		const auto index = static_cast<int>(get_page_base_index());
		const auto diff = count - index;
		return diff > server_limit ? server_limit : std::max(diff, 0);
	}

	const char* ui_feeder_item_text(int index, int column)
	{
		std::lock_guard<std::mutex> _(mutex);

		const auto i = static_cast<size_t>(get_page_base_index() + index);

		if (i >= servers.size())
		{
			return "";
		}

		if (column == 0)
		{
			return servers[i].host_name.empty() ? "" : utils::string::va("%s", servers[i].host_name.data());
		}

		if (column == 1)
		{
			return servers[i].map_name.empty() ? "" : utils::string::va("%s", servers[i].map_name.data());
		}

		if (column == 2)
		{
			return servers[i].game_type.empty() ? "" : utils::string::va("%s", servers[i].game_type.data());
		}

		if (column == 3)
		{
			return utils::string::va("%d/%d [%d]", servers[i].clients, servers[i].max_clients,
				servers[i].bots);
		}

		if (column == 4)
		{
			return servers[i].ping ? utils::string::va("%d", servers[i].ping) : "...";
		}

		return "";
	}

	// ---- Scroll & Input ----
	namespace
	{
		bool is_scrolling_disabled()
		{
			return update_server_list || (std::chrono::high_resolution_clock::now() - last_scroll) < 500ms;
		}

		bool scroll_down()
		{
			if (!is_menu_open())
			{
				return false;
			}

			if (!is_scrolling_disabled() && server_list_page + 1 < get_page_count())
			{
				last_scroll = std::chrono::high_resolution_clock::now();
				++server_list_page;
				trigger_refresh();
			}

			return true;
		}

		bool scroll_up()
		{
			if (!is_menu_open())
			{
				return false;
			}

			if (!is_scrolling_disabled() && server_list_page > 0)
			{
				last_scroll = std::chrono::high_resolution_clock::now();
				--server_list_page;
				trigger_refresh();
			}

			return true;
		}
	}

	bool sl_key_event(const int key, const int down)
	{
		if (down)
		{
			if (key == game::keyNum_t::K_MWHEELUP)
			{
				return !scroll_up();
			}

			if (key == game::keyNum_t::K_MWHEELDOWN)
			{
				return !scroll_down();
			}
		}

		return true;
	}

	// ---- Component Registration ----
	class component final : public component_interface
	{
	public:
		void post_unpack() override
		{
			if (game::environment::is_sp() || game::environment::is_dedi())
			{
				return;
			}

			my_pid = std::to_string(GetCurrentProcessId());
			console::info("lan_server_list: my_pid = %s\n", my_pid.c_str());


			// Handle LAN discovery responses
			network::on("lanInfoResponse", [](const game::netadr_s& target, const std::string& data)
			{
				if (!is_menu_open())
				{
					return;
				}

				const utils::info_string info(data);
				handle_info_response(target, info);
			});

			scheduler::loop(do_frame_work, scheduler::pipeline::main);
		}
	};
}

REGISTER_COMPONENT(lan_server_list::component)
