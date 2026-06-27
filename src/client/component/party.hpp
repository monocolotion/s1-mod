#pragma once

namespace party
{
	void reset_connect_state();

	void connect(const game::netadr_s& target);

	void clear_sv_motd();
	int server_client_count();

	// The connected server's hostname, but only for a public remote (dedicated) server we joined
	// as a pure client; empty when in menu, hosting a listen server, or in a private match.
	[[nodiscard]] std::string get_public_server_name();

	[[nodiscard]] int get_client_num_by_name(const std::string& name);

	[[nodiscard]] int get_client_count();
	[[nodiscard]] int get_bot_count();

	[[nodiscard]] game::netadr_s& get_target();
}
