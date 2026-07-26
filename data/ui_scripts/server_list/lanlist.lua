if (game:issingleplayer() or not Engine.InFrontend()) then
	return
end

local SystemLinkJoinMenu = LUI.mp_menus.SystemLinkJoinMenu

game:addlocalizedstring("PLATFORM_LAN_LIST_TITLE", "LAN LIST")

function menu_lan_join(f19_arg0, f19_arg1)
	Engine.ExecNow("setLanMode 1")

	local menu = LUI.MenuTemplate.new(f19_arg0, {
		menu_title = "@PLATFORM_LAN_LIST_TITLE",
		menu_width = CoD.DesignGridHelper(28)
	})
	Lobby.BuildServerList(Engine.GetFirstActiveController())
	Lobby.RefreshServerList(Engine.GetFirstActiveController())

	SystemLinkJoinMenu.UpdateGameList(menu)
	menu:registerEventHandler("updateGameList", SystemLinkJoinMenu.UpdateGameList)
	menu:addElement(LUI.UITimer.new(250, "updateGameList"))

	menu:AddHelp({
		name = "add_button_helper_text",
		button_ref = "button_alt1",
		helper_text = Engine.Localize("@MENU_SB_TOOLTIP_BTN_REFRESH"),
		side = "right",
		clickable = true,
		priority = -1000
	}, function(f10_arg0, f10_arg1)
		SystemLinkJoinMenu.RefreshServers(f10_arg0, f10_arg1, menu)
	end)

	menu:AddHelp({
		name = "add_button_helper_text",
		button_ref = "button_action",
		helper_text = Engine.Localize("@MENU_JOIN_GAME1"),
		side = "right",
		clickable = false,
		priority = -1000
	})

	menu:AddBackButton()

	return menu
end

LUI.MenuBuilder.m_types_build["menu_lan_join"] = menu_lan_join
