#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include "game/game.hpp"

#include "command.hpp"
#include "console.hpp"
#include "nat.hpp"
#include "network.hpp"
#include "party.hpp"
#include "scheduler.hpp"

#include <utils/string.hpp>
#include <utils/cryptography.hpp>

#include <discord_rpc.h>

namespace discord
{
	namespace
	{
		constexpr auto* JOIN_SECRET_PREFIX = "s1:1:";

		DiscordRichPresence discord_presence;

		std::mutex pending_join_mutex;
		std::string pending_join_secret;

		std::string get_join_address()
		{
			if (!game::CL_IsCgameInitialized())
			{
				return {};
			}

			// Host: our reachable endpoint, paired with a real token to hole-punch.
			if (auto endpoint = nat::get_host_endpoint(); !endpoint.empty())
			{
				return endpoint;
			}

			// Client on a directly-reachable server: advertise it (token will be "-").
			const auto& connected = party::get_target();
			if (network::is_connectable_address(connected) && !network::is_private_ip(connected))
			{
				return network::address_to_string(connected);
			}

			return {};
		}

		std::string make_join_secret(const std::string& address)
		{
			if (address.empty())
			{
				return {};
			}

			// "s1:1:<token>:<ip>:<port>"; token "-" means direct-only (no punch).
			const auto token = nat::current_token();
			const auto token_field = token.empty() ? std::string("-") : token;

			const auto secret = std::string(JOIN_SECRET_PREFIX) + token_field + ":" + address;
			if (secret.size() >= 128)
			{
				return {};
			}

			return secret;
		}

		bool parse_join_secret(const std::string& secret, std::string& token, std::string& address)
		{
			if (!utils::string::starts_with(secret, JOIN_SECRET_PREFIX))
			{
				return false;
			}

			// "<token>:<ip>:<port>"
			const auto raw = secret.substr(std::strlen(JOIN_SECRET_PREFIX));
			const auto sep = raw.find(':');
			if (sep == std::string::npos)
			{
				return false;
			}

			token = raw.substr(0, sep);

			const auto raw_address = raw.substr(sep + 1);
			const auto parsed = network::address_from_string(raw_address);
			if (!network::is_connectable_address(parsed))
			{
				return false;
			}

			address = network::address_to_string(parsed);
			return true;
		}

		void process_pending_join()
		{
			std::string secret;
			{
				std::lock_guard<std::mutex> lock(pending_join_mutex);
				secret = std::move(pending_join_secret);
				pending_join_secret.clear();
			}

			if (secret.empty())
			{
				return;
			}

			std::string token;
			std::string address;
			if (!parse_join_secret(secret, token, address))
			{
				// Legacy/raw-address invite (pre-token secrets were just "ip:port").
				const auto parsed = network::address_from_string(secret);
				if (network::is_connectable_address(parsed))
				{
					command::execute("connect " + network::address_to_string(parsed));
				}
				else
				{
					console::error("Discord: invalid join secret\n");
				}
				return;
			}

			// "-" / empty token => friend is on a directly reachable server.
			if (token.empty() || token == "-")
			{
				command::execute("connect " + address);
				return;
			}

			// Hole-punch toward the host; falls back to `address` (port-forward/VPN).
			nat::begin_join(token, address);
		}

		void join_game(const char* join_secret)
		{
			if (!join_secret || !join_secret[0])
			{
				return;
			}

			std::lock_guard<std::mutex> lock(pending_join_mutex);
			pending_join_secret = join_secret;
		}

		void join_request(const DiscordUser* request)
		{
#ifdef _DEBUG
			console::info("Discord: Join request from %s (%s)\n", request->username, request->userId);
#endif
			Discord_Respond(request->userId, DISCORD_REPLY_IGNORE);
		}

		void update_discord()
		{
			std::string join_secret_storage;
			std::string party_id_storage;

			auto* dvar = game::Dvar_FindVar("virtualLobbyActive");
			if (!game::CL_IsCgameInitialized() || (dvar && dvar->current.enabled == 1))
			{
				discord_presence.details = game::environment::is_sp() ? "Singleplayer" : "Multiplayer";

				dvar = game::Dvar_FindVar("virtualLobbyInFiringRange");
				if (dvar && dvar->current.enabled == 1)
				{
					discord_presence.state = "Firing Range";
				}
				else
				{
					discord_presence.state = "Main Menu";
				}

				discord_presence.partySize = 0;
				discord_presence.partyMax = 0;
				discord_presence.partyId = nullptr;
				discord_presence.joinSecret = nullptr;
				discord_presence.partyPrivacy = 0;
				discord_presence.startTimestamp = 0;
			}
			else
			{
				if (game::environment::is_sp()) return;

				const auto* gametype = game::UI_GetGameTypeDisplayName(game::Dvar_FindVar("ui_gametype")->current.string);
				const auto* map = game::UI_GetMapDisplayName(game::Dvar_FindVar("ui_mapname")->current.string);

				discord_presence.details = utils::string::va("%s on %s", gametype, map);

				// get server host name
				auto* const host_name = reinterpret_cast<char*>(0x141646CC4);
				utils::string::strip(host_name, host_name, std::strlen(host_name) + 1);

				// get number of clients in game
				auto clients = reinterpret_cast<int*>(0x1414CC290);
				int clientsNum = *clients;
				discord_presence.partySize = clientsNum;

				if (game::Dvar_FindVar("name") && !strcmp(host_name, game::Dvar_FindVar("name")->current.string)) // host_name == name, most likely private match
				{
					discord_presence.state = "Private Match";
					discord_presence.partyMax = game::Dvar_FindVar("sv_maxclients")->current.integer;
				}
				else
				{
					discord_presence.state = host_name;
					discord_presence.partyMax = party::server_client_count();
				}

				// Join secret: an open private match (hole-punch) OR a directly reachable server.
				const auto join_address = get_join_address();
				join_secret_storage = make_join_secret(join_address);

				if (!join_secret_storage.empty())
				{
					static const auto nonce = utils::cryptography::random::get_integer();
					std::hash<std::string> hash_fn;
					party_id_storage = utils::string::va("%zu", hash_fn(join_address) ^ nonce);

					discord_presence.partyId = party_id_storage.data();
					discord_presence.joinSecret = join_secret_storage.data();
					discord_presence.partyPrivacy = DISCORD_PARTY_PUBLIC;
				}
				else
				{
					discord_presence.partyId = nullptr;
					discord_presence.joinSecret = nullptr;
					discord_presence.partyPrivacy = 0;
				}

				if (!discord_presence.startTimestamp)
				{
					discord_presence.startTimestamp = std::chrono::duration_cast<std::chrono::seconds>(
						std::chrono::system_clock::now().time_since_epoch()).count();
				}
			}

			Discord_UpdatePresence(&discord_presence);
		}
	}

	class component final : public component_interface
	{
	public:
		void post_load() override
		{
			if (game::environment::is_dedi())
			{
				return;
			}

			DiscordEventHandlers handlers;
			ZeroMemory(&handlers, sizeof(handlers));
			handlers.ready = ready;
			handlers.errored = errored;
			handlers.disconnected = errored;
			handlers.joinGame = join_game;
			handlers.spectateGame = nullptr;
			handlers.joinRequest = join_request;

			Discord_Initialize("1117777088301240350", &handlers, 1, nullptr);

			scheduler::loop(update_discord, scheduler::pipeline::main, 5s);

			// Discord callbacks (and the resulting joins) must run on the main thread,
			// since join handling drives the NAT punch and the game's network socket.
			scheduler::loop([]
			{
				Discord_RunCallbacks();
				process_pending_join();
			}, scheduler::pipeline::main, 250ms);

			initialized_ = true;
		}

		void pre_destroy() override
		{
			if (!initialized_)
			{
				return;
			}

			Discord_Shutdown();
		}

	private:
		bool initialized_ = false;

		static void ready(const DiscordUser* /*request*/)
		{
			ZeroMemory(&discord_presence, sizeof(discord_presence));

			discord_presence.instance = 1;

			console::info("Discord: Ready\n");

			Discord_UpdatePresence(&discord_presence);
		}

		static void errored(const int error_code, const char* message)
		{
			console::error("Discord: Error (%i): %s\n", error_code, message);
		}
	};
}

#ifndef DEV_BUILD
REGISTER_COMPONENT(discord::component)
#endif
