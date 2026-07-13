#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include "game/game.hpp"
#include "game/dvars.hpp"

#include "game/scripting/functions.hpp"

#include "component/command.hpp"
#include "component/console.hpp"
#include "component/notifies.hpp"
#include "component/language.hpp"

#include "script_error.hpp"
#include "script_extension.hpp"
#include "script_loading.hpp"

#include <utils/hook.hpp>
#include <utils/string.hpp>
#include <utils/io.hpp>

namespace gsc
{
	std::uint16_t function_id_start = 0x2DF;
	void* func_table[0x1000];

	namespace
	{
		#define RVA(ptr) static_cast<std::uint32_t>(reinterpret_cast<std::size_t>(ptr) - 0x140000000)

		struct gsc_error : public std::runtime_error
		{
			using std::runtime_error::runtime_error;
		};

		std::unordered_map<std::uint16_t, game::BuiltinFunction> functions;

		bool force_error_print = false;
		std::optional<std::string> gsc_error_msg;

		std::unordered_map<std::uint32_t, game::BuiltinFunction> builtin_funcs_overrides;

		utils::hook::detour scr_register_function_hook;

		std::vector<devmap_entry> devmap_entries{};

		std::optional<devmap_entry> get_devmap_entry(const std::uint8_t* codepos)
		{
			const auto itr = std::ranges::find_if(devmap_entries, [codepos](const devmap_entry& entry) -> bool
			{
				return codepos >= entry.bytecode && codepos < entry.bytecode + entry.size;
			});

			if (itr != devmap_entries.end())
			{
				return *itr;
			}

			return {};
		}

		std::optional<std::pair<std::uint16_t, std::uint16_t>> get_line_and_col_for_codepos(const std::uint8_t* codepos)
		{
			const auto entry = get_devmap_entry(codepos);

			if (!entry.has_value())
			{
				return {};
			}

			std::optional<std::pair<std::uint16_t, std::uint16_t>> best_line_info{};
			std::uint32_t best_codepos = 0;

			assert(codepos >= entry->bytecode);
			const std::uint32_t codepos_offset = static_cast<std::uint32_t>(codepos - entry->bytecode);

			for (const auto& instruction : entry->devmap)
			{
				if (instruction.codepos > codepos_offset)
				{
					continue;
				}

				if (best_line_info.has_value() && codepos_offset - instruction.codepos > codepos_offset - best_codepos)
				{
					continue;
				}

				best_line_info = { { instruction.line, instruction.col } };
				best_codepos = instruction.codepos;
			}

			return best_line_info;
		}

		unsigned int scr_get_function_stub(const char** p_name, int* type)
		{
			const auto result = game::Scr_GetFunction(p_name, type);

			for (const auto& [name, func] : functions)
			{
				game::Scr_RegisterFunction(func, 0, name);
			}

			return result;
		}

		void execute_custom_function(game::BuiltinFunction function)
		{
			auto error = false;

			try
			{
				function();
			}
			catch (const std::exception& ex)
			{
				error = true;
				force_error_print = true;
				gsc_error_msg = ex.what();
			}

			if (error)
			{
				game::Scr_ErrorInternal();
			}
		}

		void vm_call_builtin_function(const std::uint32_t index)
		{
			const auto func = reinterpret_cast<game::BuiltinFunction>(scripting::get_function_by_index(index));

			const auto custom = functions.contains(static_cast<std::uint16_t>(index));
			if (!custom)
			{
				func();
			}
			else
			{
				execute_custom_function(func);
			}
		}

		void builtin_call_error(const std::string& error)
		{
			const auto pos = game::scr_function_stack->pos;
			const auto function_id = *reinterpret_cast<std::uint16_t*>(reinterpret_cast<std::size_t>(pos - 2));

			if (function_id > 0x1000)
			{
				console::warn("in call to builtin method \"%s\"%s", gsc_ctx->meth_name(function_id).data(), error.data());
			}
			else
			{
				console::warn("in call to builtin function \"%s\"%s", gsc_ctx->func_name(function_id).data(), error.data());
			}
		}

		std::optional<std::string> get_opcode_name(const std::uint8_t opcode)
		{
			try
			{
				const auto index = gsc_ctx->opcode_enum(opcode);
				return {gsc_ctx->opcode_name(index)};
			}
			catch (...)
			{
				return {};
			}
		}

		void print_callstack()
		{
			for (auto frame = game::scr_VmPub->function_frame; frame != game::scr_VmPub->function_frame_start; --frame)
			{
				const auto pos = frame == game::scr_VmPub->function_frame ? game::scr_function_stack->pos : frame->fs.pos;
				const auto function = find_function(frame->fs.pos);

				const char* location;
				if (function.has_value())
				{
					location = utils::string::va("function \"%s\" in file \"%s\"", function.value().first.data(), function.value().second.data());
				}
				else
				{
					location = utils::string::va("unknown location %p", pos);
				}

				const auto line_info = get_line_and_col_for_codepos(reinterpret_cast<const std::uint8_t*>(pos));
				if (line_info.has_value())
				{
					location = utils::string::va("%s line \"%d\" column \"%d\"", location, line_info->first, line_info->second);
				}

				console::warn("\tat %s\n", location);
			}
		}

		void vm_error_stub(unsigned __int64 mark_pos)
		{
			if (!dvars::com_developer_script->current.enabled && !force_error_print)
			{
				game::LargeLocalResetToMark(mark_pos);
				return;
			}

			console::warn("******* script runtime error ********\n");
			const auto opcode_id = *reinterpret_cast<std::uint8_t*>(SELECT_VALUE(0x14A19DE68, 0x148806768));

			const std::string error = gsc_error_msg.has_value() ? std::format(": {}", gsc_error_msg.value()) : std::string();

			if ((opcode_id >= 0x1A && opcode_id <= 0x20) || (opcode_id >= 0xA9 && opcode_id <= 0xAF))
			{
				builtin_call_error(error);
			}
			else
			{
				const auto opcode = get_opcode_name(opcode_id);
				if (opcode.has_value())
				{
					console::warn("while processing instruction %s%s\n", opcode.value().data(), error.data());
				}
				else
				{
					console::warn("while processing instruction 0x%X%s\n", opcode_id, error.data());
				}
			}

			force_error_print = false;
			gsc_error_msg = {};

			print_callstack();
			console::warn("************************************\n");
			game::LargeLocalResetToMark(mark_pos);
		}

		void scr_register_function_stub(void* func, int type, unsigned int name)
		{
			if (const auto itr = builtin_funcs_overrides.find(name); itr != builtin_funcs_overrides.end())
			{
				func = itr->second;
			}

			scr_register_function_hook.invoke<void>(func, type, name);
		}

		void scr_print()
		{
			for (auto i = 0u; i < game::Scr_GetNumParam(); ++i)
			{
				console::info("%s", game::Scr_GetString(i));
			}
		}

		void scr_print_ln()
		{
			for (auto i = 0u; i < game::Scr_GetNumParam(); ++i)
			{
				console::info("%s", game::Scr_GetString(i));
			}

			console::info("\n");
		}

		void assert_cmd()
		{
			if (!game::Scr_GetInt(0))
			{
				scr_error("Assert fail");
			}
		}

		void assert_ex_cmd()
		{
			if (!game::Scr_GetInt(0))
			{
				scr_error(utils::string::va("Assert fail: %s", game::Scr_GetString(1)));
			}
		}

		void assert_msg_cmd()
		{
			scr_error(utils::string::va("Assert fail: %s", game::Scr_GetString(0)));
		}

		void scr_cmd_is_dedicated_server()
		{
			game::Scr_AddInt(game::environment::is_dedi());
		}

		const char* get_code_pos(const int index)
		{
			if (static_cast<unsigned int>(index) >= game::scr_VmPub->outparamcount)
			{
				scr_error("Scr_GetCodePos: index is out of range");
				return "";
			}

			const auto* value = &game::scr_VmPub->top[-index];

			if (value->type != game::VAR_FUNCTION)
			{
				scr_error("Scr_GetCodePos requires a function as parameter");
				return "";
			}

			return value->u.codePosValue;
		}
	}

	void add_devmap_entry(std::uint8_t* codepos, std::size_t size, const std::string& name, xsk::gsc::buffer devmap_buf)
	{
		std::vector<dev_map_instruction> devmap{};
		const auto* devmap_ptr = reinterpret_cast<const dev_map*>(devmap_buf.data);

		devmap.resize(devmap_ptr->num_instructions);
		std::memcpy(devmap.data(), devmap_ptr->instructions, sizeof(dev_map_instruction) * devmap_ptr->num_instructions);

		devmap_entries.emplace_back(codepos, size, name, std::move(devmap));
	}

	void clear_devmap()
	{
		devmap_entries.clear();
	}

	void scr_error(const char* error)
	{
		force_error_print = true;
		gsc_error_msg = error;

		game::Scr_ErrorInternal();
	}

	void override_function(const std::string& name, game::BuiltinFunction func)
	{
		const auto id = gsc_ctx->func_id(name);
		builtin_funcs_overrides.emplace(id, func);
	}

	void add_function(const std::string& name, game::BuiltinFunction function)
	{
		++function_id_start;
		functions[function_id_start] = function;
		gsc_ctx->func_add(name, function_id_start);
	}

	void check_path(const std::filesystem::path& path)
	{
		if (path.generic_string().find("..") != std::string::npos)
		{
			throw std::runtime_error("Directory traversal is not allowed");
		}
	}

	std::string convert_path(const std::filesystem::path& path)
	{
		//don't allow GSC to modify files outside of fs_basepath
		check_path(path);

		static const auto fs_base_game = game::Dvar_FindVar("fs_basepath");
		const std::filesystem::path fs_base_game_path(fs_base_game->current.string);
		return (fs_base_game_path / path).generic_string();
	}

	class extension final : public component_interface
	{
	public:
		void post_unpack() override
		{
			scr_register_function_hook.create(game::Scr_RegisterFunction, &scr_register_function_stub);

			override_function("print", &scr_print);
			override_function("println", &scr_print_ln);

			override_function("assert", &assert_cmd);
			override_function("assertex", &assert_ex_cmd);
			override_function("assertmsg", &assert_msg_cmd);

			override_function("isdedicatedserver", &scr_cmd_is_dedicated_server);

			add_function("replacefunc", []
			{
				if (scr_get_type(0) != game::VAR_FUNCTION || scr_get_type(1) != game::VAR_FUNCTION)
				{
					throw gsc_error("Parameter must be a function");
				}

				notifies::set_gsc_hook(get_code_pos(0), get_code_pos(1));
			});

			add_function("executecommand", []() -> void
			{
				const auto* cmd = game::Scr_GetString(0);
				command::execute(cmd);
			});

			//IO GSC FUNCS

			add_function("fileexists", []
			{
				const auto* path = game::Scr_GetString(0);
				const auto converted_path = convert_path(path);
				game::Scr_AddInt(utils::io::file_exists(converted_path));
			});

			add_function("writefile", []
			{
				const auto* path = game::Scr_GetString(0);
				const auto converted_path = convert_path(path);
				const auto* data = game::Scr_GetString(1);

				auto append = false;
				if (game::Scr_GetNumParam() > 2)
				{
					append = game::Scr_GetInt(2);
				}

				game::Scr_AddInt(utils::io::write_file(converted_path, data, append));
			});

			add_function("readfile", []
			{
				const auto* path = game::Scr_GetString(0);
				const auto converted_path = convert_path(path);
				game::Scr_AddString(utils::io::read_file(converted_path).data());
			});

			add_function("filesize", []
			{
				const auto* path = game::Scr_GetString(0);
				const auto converted_path = convert_path(path);
				game::Scr_AddInt(static_cast<uint32_t>(utils::io::file_size(converted_path)));
			});

			add_function("createdirectory", []
			{
				const auto* path = game::Scr_GetString(0);
				const auto converted_path = convert_path(path);
				game::Scr_AddInt(utils::io::create_directory(converted_path));
			});

			add_function("directoryexists", []
			{
				const auto* path = game::Scr_GetString(0);
				const auto converted_path = convert_path(path);
				game::Scr_AddInt(utils::io::create_directory(converted_path));
			});

			add_function("copyfolder", []
			{
				const auto* source_path = game::Scr_GetString(0);
				const auto source_converted = convert_path(source_path);

				const auto* target_path = game::Scr_GetString(0);
				const auto target_converted = convert_path(target_path);

				game::Scr_AddInt(utils::io::copy_folder(source_converted, target_converted));
			});

			add_function("removefile", []
			{
				const auto* path = game::Scr_GetString(0);
				const auto converted_path = convert_path(path);
				game::Scr_AddInt(utils::io::remove_file(converted_path));
			});


			add_function("gettranslatedstring_gsc", []
			{
				if (game::Scr_GetNumParam() < 1)
					return;
				const char* str = game::Scr_GetString(0);
				const char* translated = language::get_translation_any(str);
				if (translated)
					game::Scr_AddString(translated);
			});
			utils::hook::set<std::uint32_t>(SELECT_VALUE(0x1403115BC, 0x1403EDAEC), 0x1000); // Scr_RegisterFunction

			utils::hook::set<std::uint32_t>(SELECT_VALUE(0x1403115C2 + 4, 0x1403EDAF2 + 4), RVA(&func_table)); // Scr_RegisterFunction
			// utils::hook::set<std::uint32_t>(SELECT_VALUE(0x14031F097 + 3, 0x1403FB7F7 + 3), RVA(&func_table)); // VM_Execute_0
			utils::hook::inject(SELECT_VALUE(0x140311964 + 3, 0x1403EDEE4 + 3), &func_table); // Scr_BeginLoadScripts

			utils::hook::nop(SELECT_VALUE(0x14031F097 + 5, 0x1403FB7F7 + 5), 2);
			utils::hook::call(SELECT_VALUE(0x14031F097, 0x1403FB7F7), vm_call_builtin_function);

			utils::hook::call(SELECT_VALUE(0x14032034F, 0x1403FCAB0), vm_error_stub); // LargeLocalResetToMark

			utils::hook::call(SELECT_VALUE(0x14031199F, 0x1403EDF1F), scr_get_function_stub);
		}
	};
}

REGISTER_COMPONENT(gsc::extension)
